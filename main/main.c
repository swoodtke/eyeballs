/**
 * @file main.c
 * @brief Animated eyeball – dual-board support
 *
 * Supported boards (auto-detected at runtime):
 *   1.46" : ESP32-S3-Touch-LCD-1.46   — SPD2010, 412×412
 *   1.75" : ESP32-S3-Touch-AMOLED-1.75 — CO5300,  466×466
 *
 * See board_config.h for pin maps, GPIO overlap warnings, and detection logic.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
// Define USE_NEW_I2C_API to use the new i2c_master driver instead of the legacy one.
// The legacy driver is more tolerant of clock stretching on this board's shared bus.
// #define USE_NEW_I2C_API
#ifdef USE_NEW_I2C_API
#include "driver/i2c_master.h"
#else
#include "driver/i2c.h"
#endif
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_spd2010.h"
#include "esp_lcd_co5300.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_adc/adc_oneshot.h"
#include "ble.h"
#include "board_config.h"

static const char *TAG = "eye";

// Board config — set once at boot by detect_board(), used everywhere
static const board_config_t *board;
static int lcd_pixels;  // board->lcd_w * board->lcd_h, set once

// I2C device addresses
#define TCA9554_ADDR   0x20
#define TCA9554_INPUT  0x00
#define TCA9554_OUTPUT 0x01
#define TCA9554_CFG    0x03
// EXIO bit positions on TCA9554 (from 1.46" schematic — only used when lcd_rst_via_tca)
#define EXIO_TP_RST    (1 << 1)   // EXIO1
#define EXIO_LCD_RST   (1 << 2)   // EXIO2

typedef enum { MODE_CAT_EYE, MODE_HYPNOTOAD, MODE_SAURON,
               MODE_SPIRAL_RINGS, MODE_HEART } display_mode_t;
#define NUM_MODES 5
static display_mode_t display_mode = MODE_CAT_EYE;
static float current_fps = 0;
static uint8_t display_brightness = 100;  // 0-100%, only used on CO5300 (1.75" board)
static uint8_t prev_brightness = 100;

// ─────────────────────────────────────────────────────────────────────────────
// Framebuffer  (double-buffered in PSRAM, sized at runtime from board config)
// Core 1 renders into the back buffer while Core 0 DMA-flushes the front.
// ─────────────────────────────────────────────────────────────────────────────
static uint16_t *framebuf[2];   // two full-screen buffers in PSRAM
static uint16_t *draw_fb;       // points to whichever buffer the render task writes
static int front_idx = 0;
static int back_idx  = 1;

// Dirty row band per framebuffer (inclusive), set by the render task and
// consumed by lcd_flush: only rows that differ from the panel's GRAM get
// sent. Modes that don't track dirtiness use the full screen.
static volatile int flush_band_y0[2] = {0, 0};
static volatile int flush_band_y1[2] = {INT32_MAX, INT32_MAX};
static esp_lcd_panel_handle_t panel;

// Dual-core synchronisation
static SemaphoreHandle_t render_done_sem;  // Core 1 → Core 0: frame rendered
static SemaphoreHandle_t flush_done_sem;   // Core 0 → Core 1: back buffer safe
static SemaphoreHandle_t flush_dma_sem;    // SPI ISR → Core 0: frame DMA drained

// Snapshot of all render inputs passed from Core 0 → Core 1
typedef struct {
    float px, py;
    float rpx, rpy;
    float blink_pos;
    float mic_loudness;
    display_mode_t display_mode;
    float spiral_phase;
    float spiral_zoom;
    uint8_t spiral_color_a[3];
    uint8_t spiral_color_b[3];
    uint8_t spiral_color_c[3];
    uint8_t spiral_color_d[3];
    float sauron_blink_pos;
    uint16_t *target_fb;
} render_params_t;

static render_params_t render_params;

// ─────────────────────────────────────────────────────────────────────────────
// Colour helpers — byte-swapped RGB565 for little-endian DMA to big-endian LCD
// ─────────────────────────────────────────────────────────────────────────────
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = ((uint16_t)(r & 0xF8) << 8)
               | ((uint16_t)(g & 0xFC) << 3)
               | (b >> 3);
    return (c >> 8) | (c << 8);   // byte-swap for SPI big-endian
}

#define COL_BLACK    rgb(  0,   0,   0)
#define COL_SCLERA   rgb(240, 240, 246)
#define COL_IRIS_RIM rgb( 10,  42, 105)
#define COL_IRIS     rgb( 25,  82, 190)
#define COL_IRIS_IN  rgb( 15,  55, 130)
#define COL_PUPIL    rgb(  5,   5,   8)
#define COL_GLINT1   rgb(255, 255, 255)
#define COL_GLINT2   rgb(195, 208, 238)
#define COL_SKIN     rgb(235, 190, 152)
#define COL_LASH     rgb( 15,   8,   4)
#define COL_VESSEL   rgb(205,  45,  45)

// ─────────────────────────────────────────────────────────────────────────────
// Drawing primitives  (operate on draw_fb, the full-screen staging buffer)
// ─────────────────────────────────────────────────────────────────────────────
static inline void fb_set(int x, int y, uint16_t col)
{
    if ((unsigned)x < (unsigned)board->lcd_w && (unsigned)y < (unsigned)board->lcd_h)
        draw_fb[y * board->lcd_w + x] = col;
}

static inline void hline(int x0, int x1, int y, uint16_t col)
{
    if ((unsigned)y >= (unsigned)board->lcd_h) return;
    if (x0 < 0)              x0 = 0;
    if (x1 >= board->lcd_w)  x1 = board->lcd_w - 1;
    uint16_t *row = draw_fb + y * board->lcd_w;
    for (int x = x0; x <= x1; x++) row[x] = col;
}

static void draw_circle(int cx, int cy, int r, uint16_t col)
{
    int x = 0, y = r, d = 3 - 2 * r;
    while (y >= x) {
        hline(cx - x, cx + x, cy + y, col);
        hline(cx - x, cx + x, cy - y, col);
        hline(cx - y, cx + y, cy + x, col);
        hline(cx - y, cx + y, cy - x, col);
        if (d < 0) d += 4 * x + 6;
        else { d += 4 * (x - y) + 10; y--; }
        x++;
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// I2C helpers  (shared bus: TCA9554 + QMI8658 + touch + RTC)
//
// Both legacy and new driver expose the same interface:
//   i2c_init(), i2c_write_reg(addr, reg, val), i2c_read_reg(addr, reg, out, len)
// ─────────────────────────────────────────────────────────────────────────────

// Per-device I2C transaction counters (keyed by address)
static uint32_t i2c_txn_tca = 0, i2c_txn_qmi = 0, i2c_txn_tp = 0;
static uint8_t qmi_addr = 0x6B;   // resolved at runtime in imu_init

static void i2c_count(uint8_t addr)
{
    if (addr == TCA9554_ADDR)       i2c_txn_tca++;
    else if (addr == qmi_addr)      i2c_txn_qmi++;
    else if (addr == board->tp_addr) i2c_txn_tp++;
}

#ifdef USE_NEW_I2C_API

static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t i2c_devs[128];  // handle cache by 7-bit addr

static void i2c_init(void)
{
    memset(i2c_devs, 0, sizeof(i2c_devs));
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = board->pin_i2c_sda,
        .scl_io_num = board->pin_i2c_scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));
}

static i2c_master_dev_handle_t i2c_get_dev(uint8_t addr)
{
    if (!i2c_devs[addr]) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = 400000,
            .scl_wait_us = 5000,
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &i2c_devs[addr]));
    }
    return i2c_devs[addr];
}

static esp_err_t i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val)
{
    i2c_count(addr);
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(i2c_get_dev(addr), buf, 2, 50);
}

static esp_err_t i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *out, size_t len)
{
    i2c_count(addr);
    return i2c_master_transmit_receive(i2c_get_dev(addr), &reg, 1, out, len, 100);
}

#else  // Legacy I2C driver

static void i2c_init(void)
{
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = board->pin_i2c_sda,
        .scl_io_num       = board->pin_i2c_scl,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &cfg));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, cfg.mode, 0, 0, 0));
}

static esp_err_t i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val)
{
    i2c_count(addr);
    uint8_t buf[2] = { reg, val };
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(h, buf, 2, true);
    i2c_master_stop(h);
    esp_err_t e = i2c_master_cmd_begin(I2C_NUM_0, h, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(h);
    return e;
}

static esp_err_t i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *out, size_t len)
{
    i2c_count(addr);
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(h, reg, true);
    i2c_master_start(h);
    i2c_master_write_byte(h, (addr << 1) | I2C_MASTER_READ, true);
    i2c_master_read(h, out, len, I2C_MASTER_LAST_NACK);
    i2c_master_stop(h);
    esp_err_t e = i2c_master_cmd_begin(I2C_NUM_0, h, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(h);
    return e;
}

#endif  // USE_NEW_I2C_API

// ─────────────────────────────────────────────────────────────────────────────
// TCA9554 GPIO expander
// ─────────────────────────────────────────────────────────────────────────────
static uint8_t tca_output = 0xFF;   // cached output register

static void tca9554_init(void)
{
    i2c_write_reg(TCA9554_ADDR, TCA9554_CFG, 0x00);
    tca_output = 0xFF;
    i2c_write_reg(TCA9554_ADDR, TCA9554_OUTPUT, tca_output);
}

static void tca9554_set(uint8_t mask, bool high)
{
    if (high) tca_output |=  mask;
    else      tca_output &= ~mask;
    i2c_write_reg(TCA9554_ADDR, TCA9554_OUTPUT, tca_output);
}

// ─────────────────────────────────────────────────────────────────────────────
// LCD init — supports SPD2010 (1.46") and CO5300 (1.75")
// ─────────────────────────────────────────────────────────────────────────────

// CO5300 init command data (must be at file scope for static const)
static const uint8_t co5300_d_fe20[]  = {0x20};
static const uint8_t co5300_d_19[]    = {0x10};
static const uint8_t co5300_d_1c[]    = {0xA0};
static const uint8_t co5300_d_fe00[]  = {0x00};
static const uint8_t co5300_d_c4[]    = {0x80};
static const uint8_t co5300_d_3a[]    = {0x55};
static const uint8_t co5300_d_35[]    = {0x00};
static const uint8_t co5300_d_53[]    = {0x20};
static const uint8_t co5300_d_51[]    = {0xFF};
static const uint8_t co5300_d_63[]    = {0xFF};
static const uint8_t co5300_d_2a[]    = {0x00, 0x06, 0x01, 0xD7};
static const uint8_t co5300_d_2b[]    = {0x00, 0x00, 0x01, 0xD1};

static const co5300_lcd_init_cmd_t co5300_init_cmds[] = {
    {0xFE, co5300_d_fe20, 1, 0},
    {0x19, co5300_d_19,   1, 0},
    {0x1C, co5300_d_1c,   1, 0},
    {0xFE, co5300_d_fe00, 1, 0},
    {0xC4, co5300_d_c4,   1, 0},
    {0x3A, co5300_d_3a,   1, 0},   // COLMOD 16bpp
    {0x35, co5300_d_35,   1, 0},   // TE on
    {0x53, co5300_d_53,   1, 0},   // Brightness control
    {0x51, co5300_d_51,   1, 0},   // Max brightness
    {0x63, co5300_d_63,   1, 0},
    {0x2A, co5300_d_2a,   4, 0},   // CASET
    {0x2B, co5300_d_2b,   4, 600}, // RASET
    {0x11, NULL,          0, 600}, // Sleep out
    {0x29, NULL,          0, 0},   // Display on
};

static bool IRAM_ATTR lcd_color_trans_done_cb(esp_lcd_panel_io_handle_t io,
                                              esp_lcd_panel_io_event_data_t *edata,
                                              void *user_ctx)
{
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(flush_dma_sem, &wake);
    return wake == pdTRUE;
}

static void lcd_init(void)
{
    // Backlight off during init (only 1.46" has a backlight GPIO)
    if (board->has_backlight_gpio) {
        gpio_config_t bl_cfg = {
            .pin_bit_mask = (1ULL << board->pin_lcd_bl),
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&bl_cfg);
        gpio_set_level(board->pin_lcd_bl, 0);
    }

    // Hardware reset
    if (board->lcd_rst_via_tca) {
        // 1.46": reset via TCA9554 IO expander
        tca9554_set(EXIO_LCD_RST, false);
        vTaskDelay(pdMS_TO_TICKS(20));
        tca9554_set(EXIO_LCD_RST, true);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        // 1.75": reset via direct GPIO
        gpio_config_t rst_cfg = {
            .pin_bit_mask = (1ULL << board->pin_lcd_rst),
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&rst_cfg);
        gpio_set_level(board->pin_lcd_rst, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(board->pin_lcd_rst, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    // Create QSPI panel IO
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num        = -1,
        .cs_gpio_num        = board->pin_lcd_cs,
        .pclk_hz            = 80 * 1000 * 1000,
        .lcd_cmd_bits       = 32,
        .lcd_param_bits     = 8,
        .spi_mode           = board->spi_mode,
        // Frames are sent as one draw_bitmap, chunked by max_transfer_sz.
        // Each in-flight chunk needs an internal-RAM bounce buffer (the
        // framebuffer is in PSRAM), so keep depth × chunk size bounded.
        .trans_queue_depth  = 4,
        .on_color_trans_done = lcd_color_trans_done_cb,
        .flags = {
            .quad_mode = true,
        },
    };
    // Given by the trans-done ISR when the last DMA chunk of a frame completes
    flush_dma_sem = xSemaphoreCreateBinary();

    spi_bus_config_t bus_cfg = {
        .data0_io_num   = board->pin_lcd_sda0,
        .data1_io_num   = board->pin_lcd_sda1,
        .data2_io_num   = board->pin_lcd_sda2,
        .data3_io_num   = board->pin_lcd_sda3,
        .sclk_io_num    = board->pin_lcd_sck,
        // Chunk size for color transfers: small enough that 4 in-flight
        // bounce buffers fit in internal RAM, big enough to amortize the
        // per-chunk ISR overhead. Copy of chunk N+1 overlaps DMA of chunk N.
        .max_transfer_sz = 40960,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                              &io_config, &io_handle));

    if (board->use_spd2010) {
        // ── SPD2010 (1.46") ──
        spd2010_vendor_config_t vendor_cfg = {
            .flags = { .use_qspi_interface = 1 },
        };
        esp_lcd_panel_dev_config_t panel_cfg = {
            .reset_gpio_num = -1,
            .data_endian    = LCD_RGB_DATA_ENDIAN_BIG,
            .bits_per_pixel = 16,
            .vendor_config  = &vendor_cfg,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_spd2010(io_handle, &panel_cfg, &panel));
    } else {
        // ── CO5300 (1.75") ──
        co5300_vendor_config_t vendor_cfg = {
            .init_cmds      = co5300_init_cmds,
            .init_cmds_size = sizeof(co5300_init_cmds) / sizeof(co5300_init_cmds[0]),
            .flags = { .use_qspi_interface = 1 },
        };
        esp_lcd_panel_dev_config_t panel_cfg = {
            .reset_gpio_num = -1,
            .data_endian    = LCD_RGB_DATA_ENDIAN_BIG,
            .bits_per_pixel = 16,
            .vendor_config  = &vendor_cfg,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(io_handle, &panel_cfg, &panel));
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    if (board->x_gap || board->y_gap)
        esp_lcd_panel_set_gap(panel, board->x_gap, board->y_gap);
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    if (board->has_backlight_gpio)
        gpio_set_level(board->pin_lcd_bl, 1);

    ESP_LOGI(TAG, "Display ready (%s)", board->name);
}

// ─────────────────────────────────────────────────────────────────────────────
// Touch — SPD2010 integrated (1.46") or CST9217 (1.75")
// ─────────────────────────────────────────────────────────────────────────────

static uint8_t ble_display_mode = 0;
static int64_t last_touch_us = 0;

static SemaphoreHandle_t tp_sem;
static volatile uint32_t tp_isr_count = 0;

static void IRAM_ATTR tp_isr(void *arg)
{
    tp_isr_count++;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(tp_sem, &wake);
    if (wake) portYIELD_FROM_ISR();
}

static esp_err_t tp_i2c_write(const uint8_t *data, size_t len)
{
    uint8_t addr = board->tp_addr;
    i2c_txn_tp++;
#ifdef USE_NEW_I2C_API
    return i2c_master_transmit(i2c_get_dev(addr), data, len, 50);
#else
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(h, data, len, true);
    i2c_master_stop(h);
    esp_err_t e = i2c_master_cmd_begin(I2C_NUM_0, h, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(h);
    return e;
#endif
}

static esp_err_t tp_i2c_write_read(const uint8_t *cmd, size_t cmd_len, uint8_t *out, size_t out_len)
{
    uint8_t addr = board->tp_addr;
    i2c_txn_tp++;
#ifdef USE_NEW_I2C_API
    return i2c_master_transmit_receive(i2c_get_dev(addr), cmd, cmd_len, out, out_len, 100);
#else
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(h, cmd, cmd_len, true);
    i2c_master_start(h);
    i2c_master_write_byte(h, (addr << 1) | I2C_MASTER_READ, true);
    i2c_master_read(h, out, out_len, I2C_MASTER_LAST_NACK);
    i2c_master_stop(h);
    esp_err_t e = i2c_master_cmd_begin(I2C_NUM_0, h, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(h);
    return e;
#endif
}

// ── SPD2010 touch init (1.46") ──
static void touch_init_spd2010(void)
{
    uint8_t cmd[2] = {0x20, 0x00};
    uint8_t status[4] = {0};
    tp_i2c_write_read(cmd, 2, status, 4);

    bool in_bios = (status[1] >> 6) & 1;
    bool in_cpu  = (status[1] >> 5) & 1;

    if (in_bios) {
        uint8_t clr[] = {0x02, 0x00, 0x01, 0x00};
        tp_i2c_write(clr, 4); esp_rom_delay_us(200);
        uint8_t cpu[] = {0x04, 0x00, 0x01, 0x00};
        tp_i2c_write(cpu, 4); esp_rom_delay_us(200);
        ESP_LOGI(TAG, "Touch: started CPU from BIOS");
        vTaskDelay(pdMS_TO_TICKS(100));
    } else if (in_cpu) {
        uint8_t pm[] = {0x50, 0x00, 0x00, 0x00};
        tp_i2c_write(pm, 4); esp_rom_delay_us(200);
        uint8_t st[] = {0x46, 0x00, 0x00, 0x00};
        tp_i2c_write(st, 4); esp_rom_delay_us(200);
        uint8_t clr[] = {0x02, 0x00, 0x01, 0x00};
        tp_i2c_write(clr, 4); esp_rom_delay_us(200);
        ESP_LOGI(TAG, "Touch: configured point mode");
    }
}

// ── CST9217 touch init (1.75") ──
static void touch_init_cst9217(void)
{
    // Hardware reset via direct GPIO
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << board->pin_tp_rst),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(board->pin_tp_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(board->pin_tp_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "Touch: CST9217 reset complete");
}

static void touch_init(void)
{
    if (board->use_spd2010)
        touch_init_spd2010();
    else
        touch_init_cst9217();

    // Set up interrupt on TP_INT (active low)
    tp_sem = xSemaphoreCreateBinary();
    gpio_config_t tp_cfg = {
        .pin_bit_mask = (1ULL << board->pin_tp_int),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&tp_cfg);
    gpio_isr_handler_add(board->pin_tp_int, tp_isr, NULL);

    ESP_LOGI(TAG, "Touch ready (tap to toggle mode, INT on GPIO%d)", board->pin_tp_int);
}

static uint32_t tp_serviced_count = 0;
static uint32_t tp_spurious_count = 0;

// ── SPD2010 touch check ──
static bool touch_check_spd2010(void)
{
    uint8_t cmd[2] = {0x20, 0x00};
    uint8_t status[4] = {0};
    if (tp_i2c_write_read(cmd, 2, status, 4) != ESP_OK) return false;

    bool pt_exist = status[0] & 0x01;
    uint16_t read_len = (status[3] << 8) | status[2];

    if (pt_exist && read_len > 0) {
        tp_serviced_count++;
        uint8_t hdr[2] = {0x00, 0x03};
        uint8_t data[64] = {0};
        int rlen = read_len > sizeof(data) ? sizeof(data) : read_len;
        tp_i2c_write_read(hdr, 2, data, rlen);

        uint8_t clr[] = {0x02, 0x00, 0x01, 0x00};
        tp_i2c_write(clr, 4);
        return true;
    } else {
        tp_spurious_count++;
        uint8_t clr[] = {0x02, 0x00, 0x01, 0x00};
        tp_i2c_write(clr, 4);
    }
    return false;
}

// ── CST9217 touch check — minimal: any touch event = tap ──
static bool touch_check_cst9217(void)
{
    // Read touch count from register 0x02 (standard HYN protocol)
    uint8_t reg = 0x02;
    uint8_t count = 0;
    if (i2c_read_reg(board->tp_addr, reg, &count, 1) != ESP_OK) return false;
    count &= 0x0F;
    if (count > 0) {
        tp_serviced_count++;
        return true;
    }
    tp_spurious_count++;
    return false;
}

/** Check touch — only reads I2C when TP_INT fires. */
static bool touch_check(void)
{
    if (xSemaphoreTake(tp_sem, 0) != pdTRUE) return false;

    bool touched;
    if (board->use_spd2010)
        touched = touch_check_spd2010();
    else
        touched = touch_check_cst9217();

    if (touched) {
        int64_t now = esp_timer_get_time();
        if (now - last_touch_us > 1500000) {
            last_touch_us = now;
            return true;
        }
    }
    return false;
}

// TE (tearing effect) sync — wait for display vsync before flushing
static SemaphoreHandle_t te_sem;
static volatile uint32_t te_isr_count = 0;

static void IRAM_ATTR te_isr(void *arg)
{
    te_isr_count++;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(te_sem, &wake);
    if (wake) portYIELD_FROM_ISR();
}

static void te_init(void)
{
    if (board->pin_lcd_te < 0) {
        ESP_LOGI(TAG, "TE sync not available on this board");
        te_sem = xSemaphoreCreateBinary();
        gpio_install_isr_service(0);
        return;
    }
    te_sem = xSemaphoreCreateBinary();
    gpio_config_t te_cfg = {
        .pin_bit_mask = (1ULL << board->pin_lcd_te),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&te_cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(board->pin_lcd_te, te_isr, NULL);
}

/** Push the front buffer to the display as one draw_bitmap call.
 *  The esp_lcd SPI IO layer splits it into hardware-max DMA chunks that
 *  pipeline back-to-back with CS held — one CASET/RASET setup per frame.
 *  (Flushing in strips forced a pipeline drain + 3 synchronous command
 *  transactions per strip, stretching the write past one panel refresh.) */
static void lcd_flush(void)
{
    const int W = board->lcd_w, H = board->lcd_h;

    // Only the dirty row band needs to reach the panel — GRAM retains the
    // rest from previous flushes
    int y0 = flush_band_y0[front_idx];
    int y1 = flush_band_y1[front_idx];
    if (y0 < 0) y0 = 0;
    if (y1 > H - 1) y1 = H - 1;
    if (y1 < y0) return;   // nothing changed on the panel this frame

    // Start writing right after a TE (vsync) pulse so the GRAM write stays
    // behind the panel's scan-out instead of crossing it mid-frame
    if (board->pin_lcd_te >= 0) {
        xSemaphoreTake(te_sem, 0);                  // drain a stale pulse
        xSemaphoreTake(te_sem, pdMS_TO_TICKS(25));  // wait for the next one
    }

    esp_lcd_panel_draw_bitmap(panel, 0, y0, W, y1 + 1, framebuf[front_idx] + y0 * W);

    // Block until the DMA has drained (callback fires on the last chunk):
    // this buffer becomes the render target next iteration, and Core 1
    // must not overwrite it while the SPI is still streaming from it
    xSemaphoreTake(flush_dma_sem, pdMS_TO_TICKS(100));
}

// ─────────────────────────────────────────────────────────────────────────────
// QMI8658 IMU driver
// ─────────────────────────────────────────────────────────────────────────────
#define QMI_WHO_AM_I 0x00
#define QMI_CTRL1    0x02
#define QMI_CTRL2    0x03
#define QMI_CTRL7    0x08
#define QMI_STATUS0  0x2E
#define QMI_AX_L     0x35

static void imu_init(void)
{
    // Try 0x6B first, fallback to 0x6A
    qmi_addr = 0x6B;
    uint8_t who = 0;
    if (i2c_read_reg(qmi_addr, QMI_WHO_AM_I, &who, 1) != ESP_OK || who != 0x05) {
        qmi_addr = 0x6A;
        i2c_read_reg(qmi_addr, QMI_WHO_AM_I, &who, 1);
    }
    ESP_LOGI(TAG, "QMI8658 WHO_AM_I=0x%02X @ 0x%02X", who, qmi_addr);

    i2c_write_reg(qmi_addr, QMI_CTRL1, 0x40);   // addr auto-inc, little-endian
    i2c_write_reg(qmi_addr, QMI_CTRL2, 0x16);   // accel ±4 g, 58.75 Hz
    i2c_write_reg(qmi_addr, QMI_CTRL7, 0x03);   // enable accel + gyro
    vTaskDelay(pdMS_TO_TICKS(50));

    // Verify config registers took
    uint8_t ctrl1 = 0, ctrl2 = 0, ctrl7 = 0;
    i2c_read_reg(qmi_addr, QMI_CTRL1, &ctrl1, 1);
    i2c_read_reg(qmi_addr, QMI_CTRL2, &ctrl2, 1);
    i2c_read_reg(qmi_addr, QMI_CTRL7, &ctrl7, 1);
    ESP_LOGI(TAG, "IMU verify: CTRL1=0x%02X CTRL2=0x%02X CTRL7=0x%02X", ctrl1, ctrl2, ctrl7);
    ESP_LOGI(TAG, "IMU ready");
}

static int imu_log_counter = 0;

static float last_ax = 0, last_ay = 0, last_az = 0;
static uint32_t imu_err_count = 0;

static void imu_accel(float *ax, float *ay, float *az)
{
    uint8_t raw[6];
    esp_err_t err = i2c_read_reg(qmi_addr, QMI_AX_L, raw, 6);
    if (err != ESP_OK) {
        imu_err_count++;
        *ax = last_ax; *ay = last_ay; *az = last_az;
        return;
    }

    // Dump raw bytes periodically
    if (++imu_log_counter >= 200) {
        imu_log_counter = 0;
        ESP_LOGI(TAG, "IMU raw[0x35..0x3A]: %02X %02X %02X %02X %02X %02X (errs: %lu)",
                 raw[0], raw[1], raw[2], raw[3], raw[4], raw[5],
                 (unsigned long)imu_err_count);
    }

    // ±4 g range: 1 g = 8192 LSB
    *ax = last_ax = (int16_t)((raw[1] << 8) | raw[0]) / 8192.0f;
    *ay = last_ay = (int16_t)((raw[3] << 8) | raw[2]) / 8192.0f;
    *az = last_az = (int16_t)((raw[5] << 8) | raw[4]) / 8192.0f;
}

// ─────────────────────────────────────────────────────────────────────────────
// Audio codecs (1.75" board):
//   ES8311 (0x18) — DAC for speaker (provides I2S slave clocking)
//   ES7210 (0x40) — ADC for microphone input
// Both share the same I2S bus (MCLK/BCLK/WS), with separate data lines.
// ─────────────────────────────────────────────────────────────────────────────
#define ES8311_ADDR  0x18
#define ES7210_ADDR  0x40

static bool codec_available = false;

/** Helper: read-modify-write a register with mask */
static void i2c_update_reg(uint8_t addr, uint8_t reg, uint8_t mask, uint8_t val)
{
    uint8_t old = 0;
    i2c_read_reg(addr, reg, &old, 1);
    i2c_write_reg(addr, reg, (old & ~mask) | (val & mask));
}

static void es7210_init(void)
{
    // Verify ES7210 is present
    uint8_t val = 0;
    esp_err_t err = i2c_read_reg(ES7210_ADDR, 0x00, &val, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ES7210 not found at 0x%02X", ES7210_ADDR);
        return;
    }
    ESP_LOGI(TAG, "ES7210 found (reg00=0x%02X)", val);

    // Reset
    i2c_write_reg(ES7210_ADDR, 0x00, 0xFF);
    i2c_write_reg(ES7210_ADDR, 0x00, 0x41);

    // Clock enable
    i2c_write_reg(ES7210_ADDR, 0x01, 0x3F);

    // Timing control
    i2c_write_reg(ES7210_ADDR, 0x09, 0x30);
    i2c_write_reg(ES7210_ADDR, 0x0A, 0x30);

    // HPF (high-pass filter to remove DC offset)
    i2c_write_reg(ES7210_ADDR, 0x22, 0x0A);
    i2c_write_reg(ES7210_ADDR, 0x23, 0x2A);
    i2c_write_reg(ES7210_ADDR, 0x20, 0x0A);
    i2c_write_reg(ES7210_ADDR, 0x21, 0x2A);

    // Slave mode
    i2c_update_reg(ES7210_ADDR, 0x08, 0x01, 0x00);

    // Analog config
    i2c_write_reg(ES7210_ADDR, 0x40, 0x43);   // Analog power
    i2c_write_reg(ES7210_ADDR, 0x41, 0x70);   // MIC12 bias
    i2c_write_reg(ES7210_ADDR, 0x42, 0x70);   // MIC34 bias

    // Sample rate: 16kHz with MCLK=4.096MHz
    // coeff: {4096000, 16000, adc_div=0x00, doubler=1, dll=1, osr=0x20, lrck_h=1, lrck_l=0}
    i2c_write_reg(ES7210_ADDR, 0x02, 0xC0);   // adc_div=0 | doubler<<6 | dll<<7
    i2c_write_reg(ES7210_ADDR, 0x07, 0x20);   // OSR
    i2c_write_reg(ES7210_ADDR, 0x04, 0x01);   // LRCK divider high
    i2c_write_reg(ES7210_ADDR, 0x05, 0x00);   // LRCK divider low

    // I2S format: 16-bit word length, normal I2S, ADC12→SDOUT1
    i2c_write_reg(ES7210_ADDR, 0x11, 0x60);   // SP_WL=011 (16-bit), I2S format
    i2c_write_reg(ES7210_ADDR, 0x12, 0x00);   // SDOUT_MODE=00 (ADC12→SDOUT1)

    // Enable MIC1 + MIC2, max gain
    i2c_write_reg(ES7210_ADDR, 0x4B, 0xFF);   // MIC12 power off
    i2c_write_reg(ES7210_ADDR, 0x4C, 0xFF);   // MIC34 power off
    i2c_update_reg(ES7210_ADDR, 0x01, 0x0B, 0x00);  // Ungate ADC clocks
    i2c_write_reg(ES7210_ADDR, 0x4B, 0x00);   // MIC12 power on
    // MIC1: enable + max gain (0x0E = 37.5dB)
    i2c_update_reg(ES7210_ADDR, 0x43, 0x1F, 0x1E);
    // MIC2: enable + max gain
    i2c_update_reg(ES7210_ADDR, 0x44, 0x1F, 0x1E);

    codec_available = true;
    ESP_LOGI(TAG, "ES7210 ADC initialized (MIC1, 24dB, 16kHz)");
}

static void codec_init(void)
{
    if (!board->has_codec) return;
    // ES8311 (DAC/speaker) intentionally not initialized — not needed for
    // mic input and it may conflict on the shared I2S data line. Speaker
    // support is tracked in todo.md; the old init code is in git history.
    es7210_init();

    // Keep PA off — we only need mic input for now
    if (board->pin_codec_pa >= 0) {
        gpio_config_t pa_cfg = {
            .pin_bit_mask = (1ULL << board->pin_codec_pa),
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&pa_cfg);
        gpio_set_level(board->pin_codec_pa, 0);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Microphone (I2S) — measures loudness for pupil dilation
// ─────────────────────────────────────────────────────────────────────────────
#define MIC_SAMPLE_RATE  16000
#define MIC_BUF_SAMPLES  256

static i2s_chan_handle_t mic_handle;
static float mic_loudness = 0;        // smoothed loudness (0..1), drives pupil/blink
static float mic_floor = 0;          // adaptive noise floor (slow-tracking average)
static float mic_peak = 0;           // decaying peak above floor (for stats)
static float mic_level = 0;          // current raw mean-abs level (for stats)
static float mic_sensitivity = 3.0f; // floor multiplier for full loudness
                                      // loudness=1.0 when level = floor * sensitivity
static uint8_t mic_gain = 0x0E;      // ES7210 PGA gain (0x00-0x0E)
static uint8_t prev_mic_gain = 0x0E;

static bool mic_available = false;

static void mic_init(void)
{
    if (board->has_codec) {
        // ES7210 ADC + ES8311 DAC path (1.75" board)
        codec_init();
        if (!codec_available) return;

        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &mic_handle));

        i2s_std_config_t std_cfg = {
            .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = board->pin_codec_mclk,
                .bclk = board->pin_codec_bclk,
                .ws   = board->pin_codec_ws,
                .din  = board->pin_codec_din,
                .dout = I2S_GPIO_UNUSED,
                .invert_flags = { false, false, false },
            },
        };
        std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

        ESP_ERROR_CHECK(i2s_channel_init_std_mode(mic_handle, &std_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(mic_handle));
        mic_available = true;
        ESP_LOGI(TAG, "Microphone ready (ES8311 codec, I2S %d Hz)", MIC_SAMPLE_RATE);
        return;
    }

    // PDM mic path (1.46" board)
    if (board->pin_mic_sd < 0) {
        ESP_LOGI(TAG, "No mic on this board — mic_loudness stays 0");
        return;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &mic_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = board->pin_mic_sck,
            .ws   = board->pin_mic_ws,
            .din  = board->pin_mic_sd,
            .dout = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mic_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(mic_handle));
    mic_available = true;
    ESP_LOGI(TAG, "Microphone ready (PDM, I2S %d Hz)", MIC_SAMPLE_RATE);
}

/** Read a block of mic samples and return RMS loudness (0..1). */
static float mic_read_loudness(void)
{
    if (!mic_available) return 0;

    // Apply gain change from BLE — just update gain registers, no reset
    if (board->has_codec && mic_gain != prev_mic_gain) {
        if (mic_gain > 0x0E) mic_gain = 0x0E;
        // Write PGA gain directly (bits 3:0), keep SELMIC bit 4 set
        i2c_update_reg(ES7210_ADDR, 0x43, 0x0F, mic_gain);
        i2c_update_reg(ES7210_ADDR, 0x44, 0x0F, mic_gain);
        prev_mic_gain = mic_gain;
        uint8_t r43 = 0;
        i2c_read_reg(ES7210_ADDR, 0x43, &r43, 1);
        ESP_LOGI(TAG, "ES7210 gain=%d (reg43=0x%02X)", mic_gain, r43);
    }
    size_t bytes_read = 0;

    int64_t sum_sq = 0;
    int samples = 0;
    float rms;

    if (board->has_codec) {
        // 16-bit stereo — read interleaved L/R, use left channel
        int16_t buf16[MIC_BUF_SAMPLES * 2];  // stereo pairs
        esp_err_t err = i2s_channel_read(mic_handle, buf16, sizeof(buf16), &bytes_read, 0);
        if (err != ESP_OK || bytes_read == 0) return mic_loudness;
        int total = bytes_read / sizeof(int16_t);
        for (int i = 0; i < total; i += 2) {  // step by 2 for stereo, use left
            int32_t s = buf16[i];
            sum_sq += (int64_t)s * s;
            samples++;
        }
        rms = sqrtf((float)(sum_sq / (samples > 0 ? samples : 1)));
    } else {
        // PDM mic: 32-bit mono
        int32_t buf32[MIC_BUF_SAMPLES];
        esp_err_t err = i2s_channel_read(mic_handle, buf32, sizeof(buf32), &bytes_read, 0);
        if (err != ESP_OK || bytes_read == 0) return mic_loudness;
        samples = bytes_read / sizeof(int32_t);
        for (int i = 0; i < samples; i++) {
            int32_t s = buf32[i] >> 8;  // 24-bit data in 32-bit frame
            sum_sq += (int64_t)s * s;
        }
        rms = sqrtf((float)(sum_sq / samples));
    }

    // Compute mean absolute value (more intuitive than RMS for this purpose)
    float mean_abs = rms;  // RMS is close enough to mean-abs for our purposes

    // Update current level stat
    mic_level = mean_abs;

    // Adaptive noise floor: very slow tracking so it represents ambient level
    // Fast rise (new environment), very slow fall (don't lose floor during quiet moments)
    if (mean_abs > mic_floor)
        mic_floor = mic_floor * 0.99f + mean_abs * 0.01f;   // slow rise
    else
        mic_floor = mic_floor * 0.999f + mean_abs * 0.001f;  // very slow fall

    // Seed the floor on first samples
    if (mic_floor < 1.0f) mic_floor = mean_abs;

    // Peak tracking: fast attack, slow decay (shows recent loud events)
    if (mean_abs > mic_peak)
        mic_peak = mic_peak * 0.3f + mean_abs * 0.7f;
    else
        mic_peak = mic_peak * 0.995f + mean_abs * 0.005f;

    // Loudness = how far above the noise floor, scaled by sensitivity
    // At level == floor → loudness = 0
    // At level == floor * sensitivity → loudness = 1.0
    float excess = mean_abs - mic_floor;
    float loudness_target = 0;
    if (excess > 0 && mic_floor > 0) {
        // BLE can set sensitivity to any value; at <= 1.0 the divisor goes
        // zero/negative and loudness turns NaN/negative
        float sens = mic_sensitivity > 1.05f ? mic_sensitivity : 1.05f;
        loudness_target = excess / (mic_floor * (sens - 1.0f));
        if (loudness_target > 1.0f) loudness_target = 1.0f;
    }

    // Smooth: fast attack, moderate decay
    if (loudness_target > mic_loudness)
        mic_loudness = mic_loudness * 0.3f + loudness_target * 0.7f;
    else
        mic_loudness = mic_loudness * 0.85f + loudness_target * 0.15f;

    return mic_loudness;
}

// ─────────────────────────────────────────────────────────────────────────────
// Eyeball state & animation
// ─────────────────────────────────────────────────────────────────────────────
#define MAX_EYE_TRAVEL  80.0f   // max eye offset from display centre
#define MAX_RATTLE      80.0f   // max iris rattle offset within sclera

typedef struct {
    float px, py;           // current iris offset from display centre
    float vx, vy;           // iris velocity (px/s)
    float tx, ty;           // iris spring target
    // Pupil rattle — reacts to sudden acceleration
    float rpx, rpy;         // rattle offset from iris centre
    float rvx, rvy;         // rattle velocity
    float prev_ax, prev_ay; // previous accel for jerk detection
    bool  accel_init;
} EyeState;

static EyeState eye;

// ─────────────────────────────────────────────────────────────────────────────
// Blink / eyelid state
// ─────────────────────────────────────────────────────────────────────────────
// Eyelid position: 0.0 = fully open, 1.0 = fully closed
static float blink_pos = 0;
// Blink trigger threshold (configurable — will be used for mic, photoresistor, etc.)
static float blink_loud_threshold = 0.7f;   // mic loudness that triggers a blink
static float blink_close_speed    = 8.0f;   // how fast eyelid closes (units/sec)
static float blink_open_speed     = 3.0f;   // how fast eyelid reopens (units/sec)
static float blink_hold_time      = 0.08f;  // seconds to hold closed

typedef enum { BLINK_OPEN, BLINK_CLOSING, BLINK_HOLD, BLINK_OPENING } blink_state_t;
static blink_state_t blink_state = BLINK_OPEN;
static float blink_hold_timer = 0;

/** Call each frame to request a blink. Returns true if a blink was triggered. */
static bool blink_trigger(void)
{
    if (blink_state == BLINK_OPEN) {
        blink_state = BLINK_CLOSING;
        return true;
    }
    return false;
}

/** Update blink animation. Call once per frame with dt. */
static void blink_update(float dt)
{
    switch (blink_state) {
    case BLINK_OPEN:
        blink_pos = 0;
        break;
    case BLINK_CLOSING:
        blink_pos += blink_close_speed * dt;
        if (blink_pos >= 1.0f) {
            blink_pos = 1.0f;
            blink_state = BLINK_HOLD;
            blink_hold_timer = blink_hold_time;
        }
        break;
    case BLINK_HOLD:
        blink_hold_timer -= dt;
        if (blink_hold_timer <= 0) {
            blink_state = BLINK_OPENING;
        }
        break;
    case BLINK_OPENING:
        blink_pos -= blink_open_speed * dt;
        if (blink_pos <= 0) {
            blink_pos = 0;
            blink_state = BLINK_OPEN;
        }
        break;
    }
}

static void eye_init(void)
{
    memset(&eye, 0, sizeof(eye));
    blink_pos = 0;
    blink_state = BLINK_OPEN;
}

static int log_counter = 0;

static void eye_update(float ax, float ay, float az, float dt)
{
    // Iris stays centred — only rattle moves the pupil
    eye.px = 0;
    eye.py = 0;

    // Pupil rattle — sudden accel changes kick the pupil
    if (!eye.accel_init) {
        eye.prev_ax = ax; eye.prev_ay = ay;
        eye.accel_init = true;
    }
    float dax = ax - eye.prev_ax;
    float day = ay - eye.prev_ay;
    eye.prev_ax = ax;
    eye.prev_ay = ay;

    // Apply delta-accel as impulse (swapped axes like tilt)
    float da_mag = sqrtf(dax*dax + day*day);
    (void)da_mag;
    const float kick = 600.0f;
    eye.rvx +=  day * kick;
    eye.rvy += -dax * kick;

    // Rattle spring — slow enough to be visible at ~14fps
    const float rk = 15.0f, rc = 3.0f;
    eye.rvx += (-rk * eye.rpx - rc * eye.rvx) * dt;
    eye.rvy += (-rk * eye.rpy - rc * eye.rvy) * dt;
    eye.rpx += eye.rvx * dt;
    eye.rpy += eye.rvy * dt;

    // Clamp rattle
    float rd = sqrtf(eye.rpx * eye.rpx + eye.rpy * eye.rpy);
    if (rd > MAX_RATTLE) {
        eye.rpx = eye.rpx / rd * MAX_RATTLE;
        eye.rpy = eye.rpy / rd * MAX_RATTLE;
    }

    // Log every 10 frames
    int64_t now = esp_timer_get_time();
    if (++log_counter >= 200) {
        log_counter = 0;
        ESP_LOGI(TAG, "t=%lld dt=%.1fms | IMU: ax=%.3f ay=%.3f az=%.3f | "
                 "pos=(%.1f,%.1f) | loud=%.3f",
                 now / 1000, dt * 1000.0f, ax, ay, az,
                 eye.px, eye.py, mic_loudness);
    }
}

// Precomputed sclera x-spans per row (sclera never moves)
static int16_t sclera_x0[LCD_MAX_H];
static int16_t sclera_x1[LCD_MAX_H];

// Sphere shading LUT: pre-blended sclera colors indexed by dist_sq >> 5
#define SHADE_SHIFT    5
static uint16_t sclera_shade[SHADE_LUT_SIZE_MAX];

// Iris texture LUT: dynamically allocated, sized from board->iris_tex_r
static uint16_t *iris_tex;
static int iris_tex_size;  // 2 * board->iris_tex_r + 1

static void cat_eye_free(void)
{
    if (iris_tex) { heap_caps_free(iris_tex); iris_tex = NULL; }
    ESP_LOGI(TAG, "Cat eye resources freed");
}

static void iris_tex_init(void)
{
    if (iris_tex) return;
    const int R = board->iris_tex_r;
    iris_tex_size = 2 * R + 1;
    const int sz = iris_tex_size * iris_tex_size;
    iris_tex = heap_caps_malloc(sz * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!iris_tex) {
        ESP_LOGE(TAG, "Iris texture alloc failed");
        return;
    }

    #define FIBER_HASH(a) ((uint8_t)(((a) * 2654435761u) >> 24))

    const int rim_r = board->rim_r, iris_r = board->iris_r;
    const int rim_r2 = rim_r * rim_r;
    const int iris_r2 = iris_r * iris_r;
    const int n_fibers = 48;

    for (int dy = -R; dy <= R; dy++) {
        for (int dx = -R; dx <= R; dx++) {
            int idx = (dy + R) * iris_tex_size + (dx + R);
            int d2 = dx * dx + dy * dy;

            if (d2 > rim_r2) {
                iris_tex[idx] = 0;
                continue;
            }
            if (d2 > iris_r2) {
                iris_tex[idx] = COL_IRIS_RIM;
                continue;
            }

            float dist = sqrtf((float)d2);
            float angle = atan2f((float)dy, (float)dx);
            float t = dist / (float)iris_r;

            int sector = (int)((angle / (2.0f * M_PI) + 0.5f) * n_fibers) % n_fibers;
            float fiber_bright = (FIBER_HASH(sector) / 255.0f) * 0.3f - 0.15f;

            int sector2 = (int)((angle / (2.0f * M_PI) + 0.5f) * (n_fibers * 3)) % (n_fibers * 3);
            float fiber2 = (FIBER_HASH(sector2 + 97) / 255.0f) * 0.15f - 0.075f;

            float radial;
            if (t < 0.3f)
                radial = 0.15f * (1.0f - t / 0.3f);
            else
                radial = -0.25f * ((t - 0.3f) / 0.7f);

            float brightness = 1.0f + fiber_bright + fiber2 + radial;
            if (brightness < 0.5f) brightness = 0.5f;
            if (brightness > 1.3f) brightness = 1.3f;

            float r_base = 25.0f, g_base = 82.0f, b_base = 190.0f;
            float warm = (t < 0.4f) ? 0.3f * (1.0f - t / 0.4f) : 0.0f;

            uint8_t cr = (uint8_t)fminf(255, fmaxf(0, (r_base + warm * 40.0f) * brightness));
            uint8_t cg = (uint8_t)fminf(255, fmaxf(0, (g_base + warm * 30.0f) * brightness));
            uint8_t cb = (uint8_t)fminf(255, fmaxf(0, b_base * brightness));

            iris_tex[idx] = rgb(cr, cg, cb);
        }
    }

    #undef FIBER_HASH
    ESP_LOGI(TAG, "Iris texture ready (%d KB)", sz * 2 / 1024);
}

static void sclera_lut_init(void)
{
    const int r = board->sclera_r;
    const int r2 = r * r;
    const int H = board->lcd_h, W = board->lcd_w;
    const int CX = board->eye_cx, CY = board->eye_cy;
    int shade_lut_size = (r2 >> SHADE_SHIFT) + 2;

    for (int i = 0; i < shade_lut_size; i++) {
        float d2 = (float)(i << SHADE_SHIFT);
        float t = d2 / (float)r2;
        if (t > 1.0f) t = 1.0f;
        float shade = sqrtf(1.0f - t);
        uint8_t sr = (uint8_t)(240.0f * shade + 60.0f * (1.0f - shade));
        uint8_t sg = (uint8_t)(240.0f * shade + 55.0f * (1.0f - shade));
        uint8_t sb = (uint8_t)(246.0f * shade + 70.0f * (1.0f - shade));
        sclera_shade[i] = rgb(sr, sg, sb);
    }

    for (int y = 0; y < H; y++) {
        int dy = y - CY;
        int dy2 = dy * dy;
        if (dy2 > r2) {
            sclera_x0[y] = 0;
            sclera_x1[y] = -1;
        } else {
            int dx = (int)sqrtf((float)(r2 - dy2));
            sclera_x0[y] = CX - dx;
            sclera_x1[y] = CX + dx;
            if (sclera_x0[y] < 0) sclera_x0[y] = 0;
            if (sclera_x1[y] >= W) sclera_x1[y] = W - 1;
        }
    }
}

static void eye_draw_cat(void)
{
    const int W = board->lcd_w, H = board->lcd_h;
    const int CX = board->eye_cx, CY = board->eye_cy;
    const int SR = board->sclera_r;
    const int ITR = board->iris_tex_r;

    int ix = CX + (int)eye.px + (int)eye.rpx;
    int iy = CY + (int)eye.py + (int)eye.rpy;

    int pupil_w = 16 + (int)(mic_loudness * 70);

    for (int y = 0; y < H; y++) {
        uint16_t *row = draw_fb + y * W;
        int sx0 = sclera_x0[y];
        int sx1 = sclera_x1[y];

        for (int x = 0; x < sx0; x++) row[x] = COL_BLACK;
        for (int x = sx1 + 1; x < W; x++) row[x] = COL_BLACK;

        if (sx1 < 0) continue;

        int dy_s2 = (y - CY) * (y - CY);
        for (int x = sx0; x <= sx1; x++) {
            int dx_s = x - CX;
            int d2 = dx_s * dx_s + dy_s2;
            row[x] = sclera_shade[d2 >> SHADE_SHIFT];
        }

        int dy_i = y - iy;
        int dy_i2 = dy_i * dy_i;
        int rim_r2 = ITR * ITR;

        if (dy_i2 <= rim_r2) {
            int rim_dx = (int)sqrtf((float)(rim_r2 - dy_i2));
            int x0 = ix - rim_dx; if (x0 < sx0) x0 = sx0;
            int x1 = ix + rim_dx; if (x1 > sx1) x1 = sx1;
            int tex_row = (dy_i + ITR) * iris_tex_size;
            for (int x = x0; x <= x1; x++) {
                int dx_i = x - ix;
                row[x] = iris_tex[tex_row + dx_i + ITR];
            }
        }

        int pupil_hw = board->pupil_hw;
        if (pupil_w > 0 && dy_i >= -pupil_w && dy_i <= pupil_w) {
            float t = (float)dy_i / (float)pupil_w;
            int span = (int)(pupil_hw * sqrtf(1.0f - t * t));
            if (span < 1) span = 1;
            int x0 = ix - span; if (x0 < sx0) x0 = sx0;
            int x1 = ix + span; if (x1 > sx1) x1 = sx1;
            for (int x = x0; x <= x1; x++) row[x] = COL_PUPIL;
        }
    }

    if (blink_pos > 0.01f) {
        int lid_travel = (int)(SR * blink_pos);
        int left_edge  = CX - SR + lid_travel;
        int right_edge = CX + SR - lid_travel;

        for (int y = 0; y < H; y++) {
            int sx0 = sclera_x0[y];
            int sx1 = sclera_x1[y];
            if (sx1 < 0) continue;

            uint16_t *row = draw_fb + y * W;
            for (int x = sx0; x <= sx1; x++) {
                if (x <= left_edge || x >= right_edge) {
                    uint16_t col = COL_SKIN;
                    if (x >= left_edge - 3 && x <= left_edge)   col = COL_LASH;
                    if (x >= right_edge && x <= right_edge + 3) col = COL_LASH;
                    row[x] = col;
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Hypnotoad spiral
// ─────────────────────────────────────────────────────────────────────────────
static float spiral_phase = 0;
static float spiral_zoom  = 15.0f;  // log-spiral zoom factor
static float spiral_speed = 0.08f;  // rotation speed per frame

// Spiral colours — 4 bands, each stored as RGB bytes for BLE writeability
static uint8_t spiral_color_a[3] = {   0, 200, 180 };  // teal
static uint8_t spiral_color_b[3] = {  30, 120, 255 };  // blue
static uint8_t spiral_color_c[3] = {  80,  40, 220 };  // indigo
static uint8_t spiral_color_d[3] = { 160,  30, 200 };  // violet

// Precomputed LUTs for spiral — store angle and log-dist separately
// so zoom can be applied at draw time without trig.
// 16-bit: 8-bit LUTs made the field visibly stair-step (angle steps span ~5px
// arcs at the rim; dist steps get amplified by zoom into chunky radial bands).
static uint16_t *spiral_angle_lut = NULL;  // angle/(2*PI) * 65536
static uint16_t *spiral_dist_lut  = NULL;  // log(dist) * SPIRAL_DIST_SCALE (mod 65536)
static uint8_t  *spiral_mask      = NULL;  // 1 = inside circle, 0 = outside

// log(dist) fixed-point scale. Wrap-around past 65535 is harmless: the field
// is cyclic and the draw loop multiplies by an integer, so mod-2^16 survives.
#define SPIRAL_DIST_SCALE 12288.0f

static void spiral_lut_free(void);

static void spiral_lut_init(void)
{
    if (spiral_angle_lut) return;
    const int W = board->lcd_w, H = board->lcd_h;
    const int CX = board->eye_cx, CY = board->eye_cy;
    const float SR = (float)board->sclera_r;

    spiral_angle_lut = heap_caps_malloc(lcd_pixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    spiral_dist_lut  = heap_caps_malloc(lcd_pixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    spiral_mask      = heap_caps_malloc(lcd_pixels, MALLOC_CAP_SPIRAM);
    if (!spiral_angle_lut || !spiral_dist_lut || !spiral_mask) {
        ESP_LOGE(TAG, "Spiral LUT alloc failed");
        spiral_lut_free();   // don't leak the allocations that succeeded
        return;
    }
    float sr2 = SR * SR;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            float dx = x - CX;
            float dy = y - CY;
            float dist_sq = dx * dx + dy * dy;
            if (dist_sq > sr2) {
                spiral_mask[i] = 0;
                spiral_angle_lut[i] = 0;
                spiral_dist_lut[i] = 0;
                continue;
            }
            spiral_mask[i] = 1;
            float dist = sqrtf(dist_sq);
            float angle = atan2f(dy, dx);
            spiral_angle_lut[i] = (uint16_t)((int)(angle / (2.0f * M_PI) * 65536.0f) & 0xFFFF);
            float log_d = (dist > 1.0f) ? logf(dist) : 0;
            spiral_dist_lut[i] = (uint16_t)((int)(log_d * SPIRAL_DIST_SCALE) & 0xFFFF);
        }
    }
    ESP_LOGI(TAG, "Spiral LUT ready (split angle/dist)");
}

static void spiral_lut_free(void)
{
    if (spiral_angle_lut) { heap_caps_free(spiral_angle_lut); spiral_angle_lut = NULL; }
    if (spiral_dist_lut)  { heap_caps_free(spiral_dist_lut);  spiral_dist_lut = NULL; }
    if (spiral_mask)      { heap_caps_free(spiral_mask);      spiral_mask = NULL; }
    ESP_LOGI(TAG, "Spiral LUT freed");
}

// 256-entry palette indexed by the top 8 bits of the 16-bit spiral field:
// 4 bands of 64 entries, with a short linear blend across each band boundary
// so edges anti-alias instead of hard-switching colour.
#define SPIRAL_BLEND_W 3  // blend half-width in palette entries

static uint16_t spiral_pal256[256];

static void spiral_palette_build(void)
{
    const uint8_t *cols[4] = { spiral_color_a, spiral_color_b, spiral_color_c, spiral_color_d };
    for (int j = 0; j < 256; j++) {
        int b = j >> 6, p = j & 63;
        const uint8_t *c0 = cols[b], *c1 = cols[b];
        int t = 0;  // blend position, 0..2W over the boundary
        if (p < SPIRAL_BLEND_W) {
            c0 = cols[(b + 3) & 3];
            c1 = cols[b];
            t = p + SPIRAL_BLEND_W;
        } else if (p >= 64 - SPIRAL_BLEND_W) {
            c0 = cols[b];
            c1 = cols[(b + 1) & 3];
            t = p - (64 - SPIRAL_BLEND_W);
        }
        uint8_t r = c0[0] + (c1[0] - c0[0]) * t / (2 * SPIRAL_BLEND_W);
        uint8_t g = c0[1] + (c1[1] - c0[1]) * t / (2 * SPIRAL_BLEND_W);
        uint8_t bl = c0[2] + (c1[2] - c0[2]) * t / (2 * SPIRAL_BLEND_W);
        spiral_pal256[j] = rgb(r, g, bl);
    }
}

static void eye_draw_hypnotoad(void)
{
    if (!spiral_angle_lut) return;

    spiral_palette_build();

    uint16_t phase_offset = (uint16_t)((int)(spiral_phase * 65536.0f) & 0xFFFF);
    // dist_lut stores log(dist)*SPIRAL_DIST_SCALE. We want:
    //   dist_component = log(dist)*zoom*65536/(2*PI)  (mod 65536)
    // so multiply by zoom*65536/(2*PI*SPIRAL_DIST_SCALE) in Q16.
    // The 32-bit product wraps mod 2^32, which preserves the result mod 2^16
    // after the >>16 — no widening needed.
    uint32_t zoom_mult = (uint32_t)(spiral_zoom * 65536.0f * 65536.0f
                                    / (SPIRAL_DIST_SCALE * 2.0f * (float)M_PI));

    uint16_t black = COL_BLACK;
    for (int i = 0; i < lcd_pixels; i++) {
        if (!spiral_mask[i]) {
            draw_fb[i] = black;
        } else {
            uint16_t ang = spiral_angle_lut[i];
            uint16_t dist_val = (uint16_t)(((uint32_t)spiral_dist_lut[i] * zoom_mult) >> 16);
            uint16_t combined = (uint16_t)(ang + dist_val - phase_offset);
            draw_fb[i] = spiral_pal256[combined >> 8];
        }
    }

    draw_circle(board->eye_cx, board->eye_cy, 20, COL_PUPIL);
}

// ─────────────────────────────────────────────────────────────────────────────
// Generic pre-rendered animation loader (shared by sauron, spiral, heart)
// Reads from the "eyedata" flash partition which contains a TOC + multiple
// animations packed by pack_eyedata.py.
// ─────────────────────────────────────────────────────────────────────────────
#include "esp_partition.h"

static uint16_t anim_palette_rgb565[64];
static uint8_t *anim_comp_data = NULL;    // all compressed frames (PSRAM)
static uint32_t *anim_offsets = NULL;     // per-frame offsets into anim_comp_data
static uint32_t anim_comp_size = 0;
static uint8_t *anim_frame_buf = NULL;    // one decoded frame (PSRAM)
static int anim_frame_w, anim_frame_h, anim_num_frames;
static int anim_frame_idx = 0;
static int anim_playback = 0;   // 0=ping-pong, 1=loop
static int64_t anim_start_us = 0;
static bool anim_inited = false;
// Dirty-band bookkeeping: content row band last drawn into each framebuffer.
// The first draw into each buffer paints the full screen (static border).
static int anim_prev_band[2][2];
static int anim_full_draws = 0;

// Decode a 6-bit RLE bitstream into pixel buffer. Reports the first/last
// pixel holding content (non-zero palette index) so callers can skip
// unchanged background rows.
static void anim_decode_rle(const uint8_t *comp, int comp_size, uint8_t *out, int num_pixels,
                            int *min_px_out, int *max_px_out)
{
    int bit_pos = 0;
    int px = 0;
    int min_px = num_pixels, max_px = -1;

    #define READ6() ({ \
        int _byte = bit_pos >> 3; \
        int _bit  = bit_pos & 7; \
        bit_pos += 6; \
        ((comp[_byte] << _bit) | (comp[_byte + 1] >> (8 - _bit))) >> 2 & 0x3F; \
    })

    while (px < num_pixels) {
        int v = READ6();
        if (v != 0) {
            if (px < min_px) min_px = px;
            if (px > max_px) max_px = px;
            out[px++] = v;
        } else {
            int color = READ6();
            int count = READ6();
            if (count == 0) count = 1;
            int end = px + count;
            if (end > num_pixels) end = num_pixels;
            if (color != 0) {
                if (px < min_px) min_px = px;
                if (end - 1 > max_px) max_px = end - 1;
            }
            memset(out + px, color, end - px);
            px = end;
        }
    }
    #undef READ6
    *min_px_out = min_px;
    *max_px_out = max_px;
}

// Find animation in the eyedata partition TOC, return its offset and size
static bool anim_find_in_partition(const esp_partition_t *part, const char *name,
                                   uint32_t *out_offset, uint32_t *out_size)
{
    uint8_t header[8];
    esp_partition_read(part, 0, header, 8);
    if (memcmp(header, "EYES", 4) != 0) {
        ESP_LOGE(TAG, "eyedata partition has bad magic");
        return false;
    }
    uint16_t num_anims = header[4] | (header[5] << 8);

    for (int i = 0; i < num_anims; i++) {
        uint8_t entry[24];  // 16-byte name + 4-byte offset + 4-byte size
        esp_partition_read(part, 8 + i * 24, entry, 24);
        if (strncmp((char *)entry, name, 16) == 0) {
            *out_offset = entry[16] | (entry[17] << 8) | (entry[18] << 16) | (entry[19] << 24);
            *out_size   = entry[20] | (entry[21] << 8) | (entry[22] << 16) | (entry[23] << 24);
            return true;
        }
    }
    ESP_LOGE(TAG, "Animation '%s' not found in eyedata", name);
    return false;
}

static void anim_free(void);

static void anim_init(const char *name)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, "eyedata");
    if (!part) {
        ESP_LOGE(TAG, "eyedata partition not found!");
        return;
    }

    uint32_t anim_offset, anim_size;
    if (!anim_find_in_partition(part, name, &anim_offset, &anim_size))
        return;

    // Read animation header (16 bytes)
    uint8_t hdr[16];
    esp_partition_read(part, anim_offset, hdr, 16);
    anim_frame_w    = hdr[0] | (hdr[1] << 8);
    anim_frame_h    = hdr[2] | (hdr[3] << 8);
    anim_num_frames = hdr[4] | (hdr[5] << 8);
    int pal_size    = hdr[6] | (hdr[7] << 8);
    anim_playback   = hdr[9];  // 0=ping-pong, 1=loop

    if (pal_size <= 0 || pal_size > 64) {
        ESP_LOGE(TAG, "Anim '%s': bad palette size %d", name, pal_size);
        return;
    }

    // Read palette
    uint8_t pal_rgb[64 * 3];
    esp_partition_read(part, anim_offset + 16, pal_rgb, pal_size * 3);
    for (int i = 0; i < pal_size; i++)
        anim_palette_rgb565[i] = rgb(pal_rgb[i * 3], pal_rgb[i * 3 + 1], pal_rgb[i * 3 + 2]);

    // Read frame offset table (offsets are relative to start of file, not partition)
    int offset_table_pos = anim_offset + 16 + pal_size * 3;
    uint32_t *offsets = heap_caps_malloc(anim_num_frames * 4, MALLOC_CAP_SPIRAM);
    if (!offsets) {
        ESP_LOGE(TAG, "Anim offset table alloc failed");
        return;
    }
    esp_partition_read(part, offset_table_pos, offsets, anim_num_frames * 4);

    // Keep frames compressed in PSRAM and decode one per draw call — a full
    // decode of long animations (e.g. spiral: 360 frames = 15 MB) would not
    // fit in the 8 MB PSRAM.
    int frame_pixels = anim_frame_w * anim_frame_h;
    uint32_t data_start = offsets[0];
    anim_comp_size = anim_size - data_start;
    anim_comp_data = heap_caps_malloc(anim_comp_size + 2, MALLOC_CAP_SPIRAM);
    anim_frame_buf = heap_caps_malloc(frame_pixels, MALLOC_CAP_SPIRAM);
    if (!anim_comp_data || !anim_frame_buf) {
        ESP_LOGE(TAG, "Anim PSRAM alloc failed (%lu bytes)",
                 (unsigned long)(anim_comp_size + frame_pixels));
        heap_caps_free(offsets);
        anim_free();
        return;
    }
    esp_partition_read(part, anim_offset + data_start, anim_comp_data, anim_comp_size);
    // RLE decoder reads one byte past the current position; keep it in bounds
    anim_comp_data[anim_comp_size]     = 0;
    anim_comp_data[anim_comp_size + 1] = 0;

    // Rebase offsets so they index into anim_comp_data
    for (int f = 0; f < anim_num_frames; f++)
        offsets[f] -= data_start;
    anim_offsets = offsets;

    anim_frame_idx = 0;
    anim_start_us = esp_timer_get_time();
    anim_full_draws = 0;
    anim_inited = true;
    ESP_LOGI(TAG, "Anim '%s': %d×%d, %d frames, %.1f MB compressed in PSRAM",
             name, anim_frame_w, anim_frame_h, anim_num_frames,
             anim_comp_size / (1024.0f * 1024.0f));
}

static void anim_free(void)
{
    if (anim_comp_data) { heap_caps_free(anim_comp_data); anim_comp_data = NULL; }
    if (anim_offsets)   { heap_caps_free(anim_offsets);   anim_offsets = NULL; }
    if (anim_frame_buf) { heap_caps_free(anim_frame_buf); anim_frame_buf = NULL; }
    anim_comp_size = 0;
    anim_inited = false;
    ESP_LOGI(TAG, "Anim frames freed");
}

static void eye_draw_anim(void)
{
    if (!anim_comp_data || !anim_frame_buf) return;

    const int W = board->lcd_w, H = board->lcd_h;
    const int CX = board->eye_cx, CY = board->eye_cy;
    const int SR = board->sclera_r;

    // Wall-clock frame selection at the nominal 30 fps: display FPS only
    // affects smoothness, never the animation's speed
    int64_t pos = (esp_timer_get_time() - anim_start_us) * 30 / 1000000;
    if (anim_playback == 1) {
        anim_frame_idx = (int)(pos % anim_num_frames);
    } else if (anim_num_frames > 1) {
        int cycle = 2 * (anim_num_frames - 1);
        int p = (int)(pos % cycle);
        anim_frame_idx = p < anim_num_frames ? p : cycle - p;
    } else {
        anim_frame_idx = 0;
    }

    // Decode the current frame from the compressed data in PSRAM, learning
    // which pixel range holds content (non-background)
    int frame_pixels = anim_frame_w * anim_frame_h;
    uint32_t comp_off = anim_offsets[anim_frame_idx];
    uint32_t comp_len = (anim_frame_idx + 1 < anim_num_frames
                         ? anim_offsets[anim_frame_idx + 1] : anim_comp_size) - comp_off;
    int min_px, max_px;
    anim_decode_rle(anim_comp_data + comp_off, comp_len, anim_frame_buf, frame_pixels,
                    &min_px, &max_px);
    const uint8_t *frame = anim_frame_buf;
    uint16_t bg = anim_palette_rgb565[0];

    // Palette entry expanded to the identical 2-pixel pair it becomes at 2x
    uint32_t pal32[64];
    for (int i = 0; i < 64; i++)
        pal32[i] = (uint32_t)anim_palette_rgb565[i] * 0x00010001u;

    int scaled_w = anim_frame_w * 2;
    int scaled_h = anim_frame_h * 2;
    int ox = (W - scaled_w) / 2;
    int oy = (H - scaled_h) / 2;
    int eye_r2 = SR * SR;

    // Screen-row band holding this frame's content
    int cur_y0 = H, cur_y1 = -1;
    if (max_px >= 0) {
        cur_y0 = oy + (min_px / anim_frame_w) * 2;
        cur_y1 = oy + (max_px / anim_frame_w) * 2 + 1;
        if (cur_y0 < 0) cur_y0 = 0;
        if (cur_y1 > H - 1) cur_y1 = H - 1;
        if (cur_y1 < cur_y0) { cur_y0 = H; cur_y1 = -1; }
    }

    // Blit rows that differ from what THIS buffer still holds (frame N-2);
    // flush rows that differ from what the PANEL shows (frame N-1). The
    // first draw into each buffer paints everything, including the static
    // black corners and background ring outside the animation frame.
    // (Note: overlays drawn on top of the animation — the Sauron slit —
    // rely on the fire content spanning the full frame every frame.)
    int buf = (draw_fb == framebuf[1]) ? 1 : 0;
    int blit_y0, blit_y1;
    if (anim_full_draws < 2) {
        blit_y0 = 0;  blit_y1 = H - 1;
        flush_band_y0[buf] = 0;  flush_band_y1[buf] = H - 1;
        anim_full_draws++;
    } else {
        blit_y0 = cur_y0 < anim_prev_band[buf][0] ? cur_y0 : anim_prev_band[buf][0];
        blit_y1 = cur_y1 > anim_prev_band[buf][1] ? cur_y1 : anim_prev_band[buf][1];
        flush_band_y0[buf] = cur_y0 < anim_prev_band[buf ^ 1][0] ? cur_y0 : anim_prev_band[buf ^ 1][0];
        flush_band_y1[buf] = cur_y1 > anim_prev_band[buf ^ 1][1] ? cur_y1 : anim_prev_band[buf ^ 1][1];
    }
    anim_prev_band[buf][0] = cur_y0;
    anim_prev_band[buf][1] = cur_y1;

    int prev_fy = -1, prev_cx0 = 0, prev_cx1 = -1;
    for (int y = blit_y0; y <= blit_y1; y++) {
        uint16_t *out = draw_fb + y * W;
        int dy = y - CY;
        int dy2 = dy * dy;
        if (dy2 > eye_r2) {           // row entirely outside the eye circle
            memset(out, 0, W * sizeof(uint16_t));   // COL_BLACK == 0x0000
            prev_fy = -1;
            continue;
        }

        // Circle x-span for this row: one sqrt instead of a per-pixel test
        int half = (int)sqrtf((float)(eye_r2 - dy2));
        int cx0 = CX - half;  if (cx0 < 0) cx0 = 0;
        int cx1 = CX + half;  if (cx1 >= W) cx1 = W - 1;

        memset(out, 0, cx0 * sizeof(uint16_t));
        memset(out + cx1 + 1, 0, (W - 1 - cx1) * sizeof(uint16_t));

        int sy = y - oy;
        int fy = sy >> 1;
        if (sy < 0 || sy >= scaled_h || fy >= anim_frame_h) {
            for (int x = cx0; x <= cx1; x++) out[x] = bg;
            prev_fy = -1;
            continue;
        }

        // Identical sibling row (2x vertical scale): copy the row above
        if (fy == prev_fy && cx0 == prev_cx0 && cx1 == prev_cx1) {
            memcpy(out + cx0, out - W + cx0, (cx1 - cx0 + 1) * sizeof(uint16_t));
            continue;
        }
        prev_fy = fy;  prev_cx0 = cx0;  prev_cx1 = cx1;

        const uint8_t *frame_row = frame + fy * anim_frame_w;

        // Circle regions left/right of the frame get the background colour
        int xa = ox > cx0 ? ox : cx0;
        int xb = ox + scaled_w - 1 < cx1 ? ox + scaled_w - 1 : cx1;
        for (int x = cx0; x < xa; x++) out[x] = bg;
        for (int x = xb + 1; x <= cx1; x++) out[x] = bg;

        // Frame pixels: one aligned 32-bit store per output pixel pair
        int x = xa;
        if ((x & 1) && x <= xb) {                 // reach 4-byte alignment
            out[x] = anim_palette_rgb565[frame_row[(x - ox) >> 1]];
            x++;
        }
        if (((x - ox) & 1) == 0) {
            // pair maps to one frame pixel doubled
            for (; x + 1 <= xb; x += 2)
                *(uint32_t *)(out + x) = pal32[frame_row[(x - ox) >> 1]];
        } else {
            // pair straddles two adjacent frame pixels
            for (; x + 1 <= xb; x += 2) {
                int fx = (x - ox) >> 1;
                *(uint32_t *)(out + x) =
                    (uint32_t)anim_palette_rgb565[frame_row[fx]] |
                    ((uint32_t)anim_palette_rgb565[frame_row[fx + 1]] << 16);
            }
        }
        if (x <= xb)
            out[x] = anim_palette_rgb565[frame_row[(x - ox) >> 1]];
    }

}

// ─────────────────────────────────────────────────────────────────────────────
// Sauron eye — fire animation + firmware-drawn slit pupil
// ─────────────────────────────────────────────────────────────────────────────
// Sauron blink state — periodic narrowing to a thin vertical slit
static float sauron_blink_timer = 0;
static float sauron_blink_pos = 0;     // 0=open, 1=fully narrowed
static int   sauron_blink_state = 0;   // 0=idle, 1=closing, 2=hold, 3=opening
static float sauron_blink_hold = 0;

static void eye_draw_sauron(void)
{
    const int W = board->lcd_w, H = board->lcd_h;
    const int CX = board->eye_cx, CY = board->eye_cy;

    eye_draw_anim();

    int base_w = board->sauron_base_w + (int)(mic_loudness * 24);
    int slit_half_h = board->slit_half_h;

    float open = 1.0f - render_params.sauron_blink_pos;
    int min_w = (int)(base_w * 0.2f);
    if (min_w < 1) min_w = 1;
    int slit_half_w = min_w + (int)((base_w - min_w) * open);

    int cx = CX, cy = CY;
    if (slit_half_w < 1) slit_half_w = 1;
    if (slit_half_h < 1) slit_half_h = 1;
    int sw2 = slit_half_w * slit_half_w;
    int sh2 = slit_half_h * slit_half_h;

    int y0 = cy - slit_half_h - 2; if (y0 < 0) y0 = 0;
    int y1 = cy + slit_half_h + 2; if (y1 >= H) y1 = H - 1;
    int x0 = cx - slit_half_w - 2; if (x0 < 0) x0 = 0;
    int x1 = cx + slit_half_w + 2; if (x1 >= W) x1 = W - 1;

    for (int y = y0; y <= y1; y++) {
        int dy = y - cy;
        int dy2_sh2 = dy * dy * sw2;
        int row = y * W;
        for (int x = x0; x <= x1; x++) {
            int dxx = x - cx;
            if (dxx * dxx * sh2 + dy2_sh2 <= sw2 * sh2) {
                draw_fb[row + x] = COL_BLACK;
            }
        }
    }
}

static int mode_log_counter = 0;
static const char *mode_name(display_mode_t m) {
    switch (m) {
        case MODE_CAT_EYE:      return "CAT_EYE";
        case MODE_HYPNOTOAD:    return "HYPNOTOAD";
        case MODE_SAURON:       return "SAURON";
        case MODE_SPIRAL_RINGS: return "SPIRAL_RINGS";
        case MODE_HEART:        return "HEART";
        default:             return "UNKNOWN";
    }
}

static display_mode_t active_mode = MODE_CAT_EYE;

static void eye_mode_switch(display_mode_t new_mode)
{
    if (new_mode == active_mode) return;

    // Free old mode resources
    switch (active_mode) {
        case MODE_CAT_EYE:      cat_eye_free(); break;
        case MODE_HYPNOTOAD:    spiral_lut_free(); break;
        case MODE_SAURON:
        case MODE_SPIRAL_RINGS:
        case MODE_HEART:        anim_free(); break;
    }

    // Init new mode resources
    switch (new_mode) {
        case MODE_CAT_EYE:      iris_tex_init(); sclera_lut_init(); break;
        case MODE_HYPNOTOAD:    spiral_lut_init(); break;
        case MODE_SAURON:       anim_init("sauron"); break;
        case MODE_SPIRAL_RINGS: anim_init("spiral"); break;
        case MODE_HEART:        anim_init("heart"); break;
    }

    active_mode = new_mode;
    ESP_LOGI(TAG, "Mode switch: %s → resources loaded", mode_name(new_mode));
}

static void eye_draw(void)
{
    // Default to flushing the whole frame; eye_draw_anim narrows this to
    // the dirty row band when it can
    int fb_idx = (draw_fb == framebuf[1]) ? 1 : 0;
    flush_band_y0[fb_idx] = 0;
    flush_band_y1[fb_idx] = board->lcd_h - 1;

    if (display_mode != active_mode) {
        // Clear screen — this frame gets flushed as black while resources load
        for (int i = 0; i < lcd_pixels; i++)
            draw_fb[i] = COL_BLACK;
        eye_mode_switch(display_mode);
        return;  // show black frame; next call draws the new mode
    }

    if (++mode_log_counter >= 200) {
        mode_log_counter = 0;
        ESP_LOGI(TAG, "mode=%s", mode_name(display_mode));
    }
    if (display_mode == MODE_HYPNOTOAD)
        eye_draw_hypnotoad();
    else if (display_mode == MODE_SAURON)
        eye_draw_sauron();
    else if (display_mode == MODE_SPIRAL_RINGS ||
             display_mode == MODE_HEART)
        eye_draw_anim();
    else
        eye_draw_cat();
}

// ─────────────────────────────────────────────────────────────────────────────
// Render task — runs on Core 1, draws into the back buffer
// ─────────────────────────────────────────────────────────────────────────────
static void render_task(void *arg)
{
    while (1) {
        xSemaphoreTake(flush_done_sem, portMAX_DELAY);

        // Copy snapshot into globals so existing draw functions work unchanged
        draw_fb       = render_params.target_fb;
        eye.px        = render_params.px;
        eye.py        = render_params.py;
        eye.rpx       = render_params.rpx;
        eye.rpy       = render_params.rpy;
        blink_pos     = render_params.blink_pos;
        mic_loudness  = render_params.mic_loudness;
        display_mode  = render_params.display_mode;
        spiral_phase  = render_params.spiral_phase;
        spiral_zoom   = render_params.spiral_zoom;
        memcpy(spiral_color_a, render_params.spiral_color_a, 3);
        memcpy(spiral_color_b, render_params.spiral_color_b, 3);
        memcpy(spiral_color_c, render_params.spiral_color_c, 3);
        memcpy(spiral_color_d, render_params.spiral_color_d, 3);

        eye_draw();

        xSemaphoreGive(render_done_sem);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Battery state (shared by ADC and PMIC paths)
// ─────────────────────────────────────────────────────────────────────────────
static float battery_voltage = 0;
static float battery_percent = 0;
static float bat_adc_raw = 0;

// ─────────────────────────────────────────────────────────────────────────────
// AXP2101 PMIC (1.75" board) — battery monitoring + power control
// ─────────────────────────────────────────────────────────────────────────────
// Register map from the AXP2101 datasheet (X-power AXP2101 SWcharge V1.0)
#define AXP2101_ADDR        0x34

#define AXP2101_STATUS1     0x00   // bit5: VBUS good, bit3: battery present
#define AXP2101_STATUS2     0x01   // bits[6:5]: 00 standby, 01 charging, 10 discharging
#define AXP2101_COMMON_CFG  0x10   // bit0: soft power off (write 1)
#define AXP2101_PWROFF_EN   0x22   // bit1: PWRON long-press is a power-off source
                                   // bit0: 0 = power off, 1 = restart on long-press
#define AXP2101_LEVEL_CFG   0x27   // [5:4] IRQLEVEL, [3:2] OFFLEVEL (00=4s..11=10s),
                                   // [1:0] ONLEVEL (00=128ms, 01=512ms, 10=1s, 11=2s)
#define AXP2101_ADC_CTRL    0x30   // ADC channel enables: bit0 VBAT, bit2 VBUS, bit3 VSYS
#define AXP2101_VBAT_H      0x34   // [5:0] = vbat[13:8]; read H before L
#define AXP2101_VBAT_L      0x35   // vbat[7:0], 1 mV/LSB
#define AXP2101_VBUS_H      0x38   // [5:0] = vbus[13:8]
#define AXP2101_VBUS_L      0x39   // vbus[7:0], 1 mV/LSB
#define AXP2101_BAT_PERCENT 0xA4   // E-gauge battery percentage, 0-100

static bool pmic_available = false;
static bool battery_present = false;
static bool battery_charging = false;

static uint16_t axp2101_read_adc14(uint8_t reg_h, uint8_t reg_l)
{
    uint8_t hi = 0, lo = 0;
    i2c_read_reg(AXP2101_ADDR, reg_h, &hi, 1);   // datasheet: high 6 bits first
    i2c_read_reg(AXP2101_ADDR, reg_l, &lo, 1);
    return ((uint16_t)(hi & 0x3F) << 8) | lo;    // 1 mV/LSB
}

static void axp2101_init(void)
{
    if (!board->has_pmic) return;

    // Verify chip is present
    uint8_t val = 0;
    esp_err_t err = i2c_read_reg(AXP2101_ADDR, AXP2101_STATUS1, &val, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not found at 0x%02X", AXP2101_ADDR);
        return;
    }
    battery_present = (val >> 3) & 1;
    bool vbus_good  = (val >> 5) & 1;

    // Enable VBAT, VBUS, and VSYS ADC channels
    i2c_read_reg(AXP2101_ADDR, AXP2101_ADC_CTRL, &val, 1);
    val |= (1 << 0) | (1 << 2) | (1 << 3);
    i2c_write_reg(AXP2101_ADDR, AXP2101_ADC_CTRL, val);

    // Power button behaviour to match the 1.46": short press (512 ms)
    // powers on (handled by PMIC hardware from battery or USB), 4 s hold
    // powers off. OFFLEVEL=00 (4s), ONLEVEL=01 (512ms), keep IRQLEVEL.
    i2c_read_reg(AXP2101_ADDR, AXP2101_LEVEL_CFG, &val, 1);
    val = (val & ~0x0F) | 0x01;
    i2c_write_reg(AXP2101_ADDR, AXP2101_LEVEL_CFG, val);

    // Make the long-press a power-off (not restart) source
    i2c_read_reg(AXP2101_ADDR, AXP2101_PWROFF_EN, &val, 1);
    val = (uint8_t)((val | 0x02) & ~0x01);
    i2c_write_reg(AXP2101_ADDR, AXP2101_PWROFF_EN, val);

    pmic_available = true;
    ESP_LOGI(TAG, "AXP2101 PMIC initialized (battery %s, VBUS %s, VBUS=%umV)",
             battery_present ? "present" : "absent",
             vbus_good ? "good" : "absent",
             axp2101_read_adc14(AXP2101_VBUS_H, AXP2101_VBUS_L));
}

static void axp2101_read_battery(void)
{
    if (!pmic_available) return;

    uint8_t status = 0;
    i2c_read_reg(AXP2101_ADDR, AXP2101_STATUS1, &status, 1);
    battery_present = (status >> 3) & 1;
    if (!battery_present) {
        battery_voltage = 0;
        battery_percent = 0;
        battery_charging = false;
        return;
    }

    i2c_read_reg(AXP2101_ADDR, AXP2101_STATUS2, &status, 1);
    battery_charging = ((status >> 5) & 0x03) == 0x01;

    battery_voltage = axp2101_read_adc14(AXP2101_VBAT_H, AXP2101_VBAT_L) / 1000.0f;

    // E-gauge fuel gauge reports calibrated percentage directly
    uint8_t pct = 0;
    i2c_read_reg(AXP2101_ADDR, AXP2101_BAT_PERCENT, &pct, 1);
    battery_percent = pct > 100 ? 100 : pct;
}

static void axp2101_power_off(void)
{
    if (!pmic_available) return;
    ESP_LOGW(TAG, "AXP2101 power off!");
    uint8_t val = 0;
    i2c_read_reg(AXP2101_ADDR, AXP2101_COMMON_CFG, &val, 1);
    val |= 0x01;   // soft PWROFF
    i2c_write_reg(AXP2101_ADDR, AXP2101_COMMON_CFG, val);
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// ─────────────────────────────────────────────────────────────────────────────
// Battery power latch (1.46" board) + unified battery interface
// ─────────────────────────────────────────────────────────────────────────────
static void power_latch_init(void)
{
    if (board->pin_bat_control < 0) {
        ESP_LOGI(TAG, "No power latch on this board (PMIC manages power)");
        return;
    }

    gpio_config_t pwr_cfg = {
        .pin_bit_mask = (1ULL << board->pin_bat_control),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr_cfg);
    gpio_set_level(board->pin_bat_control, 1);

    if (board->pin_key_bat >= 0) {
        gpio_config_t key_cfg = {
            .pin_bit_mask = (1ULL << board->pin_key_bat),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&key_cfg);
    }

    ESP_LOGI(TAG, "Power latch engaged (GPIO%d)", board->pin_bat_control);
}

static void power_off(void)
{
    if (board->has_pmic) {
        axp2101_power_off();
        return;
    }
    if (board->pin_bat_control < 0) return;
    ESP_LOGW(TAG, "Powering off!");
    gpio_set_level(board->pin_bat_control, 0);
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// Battery ADC — GPIO8 has a ÷3 voltage divider, so battery_V = adc_V * 3
static adc_oneshot_unit_handle_t bat_adc_handle;

static void battery_init(void)
{
    if (board->has_pmic) {
        axp2101_init();
        return;
    }

    if (board->pin_bat_adc < 0) return;

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    adc_oneshot_new_unit(&unit_cfg, &bat_adc_handle);

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    // GPIO8 = ADC1_CHANNEL_7 on ESP32-S3
    adc_oneshot_config_channel(bat_adc_handle, ADC_CHANNEL_7, &chan_cfg);
}

static void battery_read_voltage(void)
{
    if (board->has_pmic) {
        axp2101_read_battery();
        return;
    }

    if (board->pin_bat_adc < 0) return;
    int raw_int = 0;
    adc_oneshot_read(bat_adc_handle, ADC_CHANNEL_7, &raw_int);
    bat_adc_raw = (float)raw_int;
    // 12-bit ADC with 12dB attenuation: ~0–3.1V range
    // Calibrated: charge LED off (full=4.2V) when raw≈1520, so multiplier=3.6
    float adc_v = raw_int * 3.1f / 4095.0f;
    battery_voltage = adc_v * 3.6f;

    // Li-ion approximate charge curve (3.3V=0%, 4.2V=100%)
    float pct = (battery_voltage - 3.3f) / (4.2f - 3.3f) * 100.0f;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    battery_percent = pct;
}

#define PWR_BUTTON_SHUTDOWN_MS  2000  // Hold 2s to power off

void app_main(void)
{
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    // Detect which board we're on BEFORE any hardware init
    board = detect_board();
    lcd_pixels = board->lcd_w * board->lcd_h;
    ESP_LOGI(TAG, "Eyeball starting up — Board: %s", board->name);

    power_latch_init();

    // Double framebuffers in PSRAM
    framebuf[0] = heap_caps_malloc(lcd_pixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    framebuf[1] = heap_caps_malloc(lcd_pixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    draw_fb = framebuf[0];

    if (!framebuf[0] || !framebuf[1]) {
        ESP_LOGE(TAG, "PSRAM alloc failed! Enable CONFIG_SPIRAM in menuconfig.");
        return;
    }
    ESP_LOGI(TAG, "Double framebuffers: %.1f KB in PSRAM",
             lcd_pixels * 2.0f * 2.0f / 1024.0f);

    // Create dual-core sync semaphores
    render_done_sem = xSemaphoreCreateBinary();
    flush_done_sem  = xSemaphoreCreateBinary();

    i2c_init();
    battery_init();
    tca9554_init();
    lcd_init();
    te_init();
    touch_init();
    imu_init();
    mic_init();
    eye_init();
    // Mode resources are loaded on demand by eye_mode_switch() in eye_draw()
    // Init cat eye as the default starting mode
    iris_tex_init();
    sclera_lut_init();

    // BLE param registry
    ble_display_mode = (uint8_t)display_mode;
    // Deliberately minimal — params get re-added as they prove useful.
    // The internal tuning variables (blink, spiral colors, mic gain, ...)
    // still exist; they're just not exposed over BLE.
    static const ble_param_t ble_params[] = {
        { 0x0010, "Display Mode",  BLE_PARAM_RWN,  &ble_display_mode,  1 },
        { 0x0022, "Battery V",     BLE_PARAM_STAT, &battery_voltage,   4 },
        { 0x0023, "Battery %",     BLE_PARAM_STAT, &battery_percent,   4 },
    };
    ble_init(ble_params, sizeof(ble_params) / sizeof(ble_params[0]));

    // Launch render task on Core 1
    xTaskCreatePinnedToCore(render_task, "render", 8192, NULL, 5, NULL, 1);

    // Bootstrap first frame: snapshot params and signal render task
    render_params.target_fb    = framebuf[back_idx];
    render_params.display_mode = display_mode;
    render_params.spiral_phase = spiral_phase;
    render_params.spiral_zoom  = spiral_zoom;
    memcpy(render_params.spiral_color_a, spiral_color_a, 3);
    memcpy(render_params.spiral_color_b, spiral_color_b, 3);
    memcpy(render_params.spiral_color_c, spiral_color_c, 3);
    memcpy(render_params.spiral_color_d, spiral_color_d, 3);
    render_params.sauron_blink_pos = sauron_blink_pos;
    xSemaphoreGive(flush_done_sem);

    int64_t prev_us = esp_timer_get_time();
    int64_t pwr_btn_down_since = 0;

    int frame_count = 0;
    int64_t fps_timer_us = esp_timer_get_time();
    int64_t perf_wait_us = 0, perf_flush_us = 0, perf_sensor_us = 0;

    // Render first frame synchronously so we have something to flush
    xSemaphoreTake(render_done_sem, portMAX_DELAY);

    while (1) {
        int64_t now_us = esp_timer_get_time();
        float dt = (now_us - prev_us) * 1e-6f;
        prev_us = now_us;
        if (dt > 0.1f) dt = 0.1f;

        int64_t t0 = esp_timer_get_time();

        // --- Swap and flush the just-rendered frame (Core 1 is idle here) ---
        front_idx = back_idx;
        back_idx  = 1 - front_idx;

        // Snapshot params and kick off Core 1 BEFORE flushing
        // so rendering overlaps with the DMA flush
        float ax, ay, az;
        imu_accel(&ax, &ay, &az);
        mic_read_loudness();

        if (touch_check()) {
            display_mode = (display_mode_t)((display_mode + 1) % NUM_MODES);
            ble_display_mode = (uint8_t)display_mode;
            ESP_LOGI(TAG, "Touch! Switching to %s", mode_name(display_mode));
        }

        if (mic_loudness > blink_loud_threshold) {
            blink_trigger();
        }
        blink_update(dt);
        eye_update(ax, ay, az, dt);

        spiral_phase += spiral_speed;
        if (spiral_phase > 1.0f) spiral_phase -= 1.0f;
        if (spiral_phase < 0.0f) spiral_phase += 1.0f;
        // Sauron periodic blink — narrow to thin slit every ~4 seconds
        // Sauron periodic blink: close → hold → reopen
        switch (sauron_blink_state) {
        case 0:  // idle — wait for timer
            sauron_blink_timer += dt;
            if (sauron_blink_timer > 4.0f) {
                sauron_blink_state = 1;
                sauron_blink_timer = 0;
                ESP_LOGI(TAG, "Sauron blink!");
            }
            break;
        case 1:  // closing
            sauron_blink_pos += dt * 4.0f;  // close over ~0.25s
            if (sauron_blink_pos >= 1.0f) {
                sauron_blink_pos = 1.0f;
                sauron_blink_state = 2;
                sauron_blink_hold = 0;
            }
            break;
        case 2:  // hold closed
            sauron_blink_hold += dt;
            if (sauron_blink_hold > 0.15f) {
                sauron_blink_state = 3;
            }
            break;
        case 3:  // opening
            sauron_blink_pos -= dt * 2.0f;  // reopen over ~0.5s
            if (sauron_blink_pos <= 0) {
                sauron_blink_pos = 0;
                sauron_blink_state = 0;
            }
            break;
        }

        if (ble_display_mode >= NUM_MODES) ble_display_mode = 0;
        display_mode = (display_mode_t)ble_display_mode;

        render_params.px           = eye.px;
        render_params.py           = eye.py;
        render_params.rpx          = eye.rpx;
        render_params.rpy          = eye.rpy;
        render_params.blink_pos    = blink_pos;
        render_params.mic_loudness = mic_loudness;
        render_params.display_mode = display_mode;
        render_params.spiral_phase = spiral_phase;
        render_params.spiral_zoom  = spiral_zoom;
        memcpy(render_params.spiral_color_a, spiral_color_a, 3);
        memcpy(render_params.spiral_color_b, spiral_color_b, 3);
        memcpy(render_params.spiral_color_c, spiral_color_c, 3);
        memcpy(render_params.spiral_color_d, spiral_color_d, 3);
        render_params.sauron_blink_pos = sauron_blink_pos;
        render_params.target_fb    = framebuf[back_idx];

        // Start Core 1 rendering into back buffer
        xSemaphoreGive(flush_done_sem);

        // Apply brightness if changed via BLE (CO5300 only)
        if (display_brightness != prev_brightness && !board->has_backlight_gpio) {
            esp_lcd_panel_co5300_set_brightness(panel, display_brightness);
            prev_brightness = display_brightness;
        }

        int64_t t1 = esp_timer_get_time();

        // --- Wait for Core 1 to finish rendering BEFORE flushing ---
        // Flushing while Core 1 renders halves PSRAM bandwidth for the DMA,
        // stretching the GRAM write past one panel refresh period — the
        // scan-out then laps the write and tears even with TE sync.
        xSemaphoreTake(render_done_sem, portMAX_DELAY);

        int64_t t2 = esp_timer_get_time();

        // Flush front buffer — Core 1 idle, DMA gets full PSRAM bandwidth
        lcd_flush();

        int64_t t3 = esp_timer_get_time();

        // --- Perf tracking ---
        perf_sensor_us += (t1 - t0);
        perf_wait_us   += (t2 - t1);
        perf_flush_us  += (t3 - t2);

        frame_count++;
        int64_t elapsed = now_us - fps_timer_us;
        if (elapsed >= 5000000) {
            float fc = (float)frame_count;
            ESP_LOGI(TAG, "FPS: %.1f | sensor:%.1fms flush:%.1fms wait:%.1fms",
                     frame_count * 1e6f / elapsed,
                     perf_sensor_us / fc / 1000.0f,
                     perf_flush_us / fc / 1000.0f,
                     perf_wait_us / fc / 1000.0f);
            ESP_LOGI(TAG, "  I2C tca:%lu qmi:%lu tp:%lu | IMU_ERR: %lu | TE:%lu TP:%lu(s:%lu x:%lu)",
                     (unsigned long)i2c_txn_tca, (unsigned long)i2c_txn_qmi,
                     (unsigned long)i2c_txn_tp, (unsigned long)imu_err_count,
                     (unsigned long)te_isr_count, (unsigned long)tp_isr_count,
                     (unsigned long)tp_serviced_count, (unsigned long)tp_spurious_count);
            current_fps = frame_count * 1e6f / elapsed;
            frame_count = 0;
            fps_timer_us = now_us;
            perf_sensor_us = perf_flush_us = perf_wait_us = 0;

            battery_read_voltage();
            if (board->has_pmic) {
                ESP_LOGI(TAG, "PWR: PMIC BAT=%.2fV (%.0f%%) %s",
                         battery_voltage, battery_percent,
                         !battery_present ? "no battery"
                         : battery_charging ? "charging" : "discharging");
            } else if (board->pin_bat_control >= 0) {
                ESP_LOGI(TAG, "PWR: BAT_CTRL(IO%d)=%d KEY_BAT(IO%d)=%d BAT=%.2fV (raw=%.0f adc=%.3fV)",
                         board->pin_bat_control, gpio_get_level(board->pin_bat_control),
                         board->pin_key_bat, gpio_get_level(board->pin_key_bat),
                         battery_voltage, bat_adc_raw, bat_adc_raw * 3.1f / 4095.0f);
            }

            ble_notify_all();
        }

        // PWR button long-press → power off (only on boards with hardware button)
        if (board->pin_key_bat >= 0) {
            if (gpio_get_level(board->pin_key_bat) == 0) {
                if (pwr_btn_down_since == 0) pwr_btn_down_since = now_us;
                else if ((now_us - pwr_btn_down_since) > PWR_BUTTON_SHUTDOWN_MS * 1000LL)
                    power_off();
            } else {
                pwr_btn_down_since = 0;
            }
        }
    }
}
