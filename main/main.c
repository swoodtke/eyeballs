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

typedef enum { MODE_CAT_EYE, MODE_HYPNOTOAD, MODE_BLOB_EYE, MODE_SAURON } display_mode_t;
#define NUM_MODES 4
static display_mode_t display_mode = MODE_CAT_EYE;
static float current_fps = 0;

// ─────────────────────────────────────────────────────────────────────────────
// Framebuffer  (double-buffered in PSRAM, sized at runtime from board config)
// Core 1 renders into the back buffer while Core 0 DMA-flushes the front.
// ─────────────────────────────────────────────────────────────────────────────
static uint16_t *framebuf[2];   // two full-screen buffers in PSRAM
static uint16_t *draw_fb;       // points to whichever buffer the render task writes
static int front_idx = 0;
static int back_idx  = 1;
static esp_lcd_panel_handle_t panel;

// Dual-core synchronisation
static SemaphoreHandle_t render_done_sem;  // Core 1 → Core 0: frame rendered
static SemaphoreHandle_t flush_done_sem;   // Core 0 → Core 1: back buffer safe

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
    float blob_pulse;
    float sauron_blink_pos;
    uint8_t blob_color_a[3];
    uint8_t blob_color_b[3];
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

static void draw_hband(int y0, int y1, uint16_t col)
{
    const int W = board->lcd_w, H = board->lcd_h;
    if (y0 < 0)   y0 = 0;
    if (y1 >= H)   y1 = H - 1;
    if (y0 > y1) return;
    uint16_t *row = draw_fb + y0 * W;
    for (int r = y0; r <= y1; r++, row += W)
        for (int x = 0; x < W; x++)
            row[x] = col;
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

    int half_h = board->lcd_h / 2;

    // Create QSPI panel IO
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num        = -1,
        .cs_gpio_num        = board->pin_lcd_cs,
        .pclk_hz            = 80 * 1000 * 1000,
        .lcd_cmd_bits       = 32,
        .lcd_param_bits     = 8,
        .spi_mode           = board->spi_mode,
        .trans_queue_depth  = 10,
        .flags = {
            .quad_mode = true,
        },
    };

    spi_bus_config_t bus_cfg = {
        .data0_io_num   = board->pin_lcd_sda0,
        .data1_io_num   = board->pin_lcd_sda1,
        .data2_io_num   = board->pin_lcd_sda2,
        .data3_io_num   = board->pin_lcd_sda3,
        .sclk_io_num    = board->pin_lcd_sck,
        .max_transfer_sz = board->lcd_w * (board->lcd_h / 4) * sizeof(uint16_t),
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

/** Push front buffer to display in 4 strips.
 *  Each strip must fit in the SPI DMA internal bounce buffer. */
#define FLUSH_STRIPS 4

static void lcd_flush(void)
{
    const int W = board->lcd_w, H = board->lcd_h;
    uint16_t *base = framebuf[front_idx];
    int strip_h = H / FLUSH_STRIPS;
    for (int s = 0; s < FLUSH_STRIPS; s++) {
        int y0 = s * strip_h;
        int y1 = (s == FLUSH_STRIPS - 1) ? H : y0 + strip_h;
        esp_lcd_panel_draw_bitmap(panel, 0, y0, W, y1, base + y0 * W);
    }
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
// Microphone (I2S) — measures loudness for pupil dilation
// ─────────────────────────────────────────────────────────────────────────────
#define MIC_SAMPLE_RATE  16000
#define MIC_BUF_SAMPLES  256

static i2s_chan_handle_t mic_handle;
static float mic_loudness = 0;   // smoothed loudness (0..1)

static bool mic_available = false;

static void mic_init(void)
{
    if (board->pin_mic_sd < 0) {
        ESP_LOGI(TAG, "No PDM mic on this board — mic_loudness stays 0");
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
    ESP_LOGI(TAG, "Microphone ready (I2S %d Hz)", MIC_SAMPLE_RATE);
}

/** Read a block of mic samples and return RMS loudness (0..1). */
static float mic_read_loudness(void)
{
    if (!mic_available) return 0;
    int32_t buf[MIC_BUF_SAMPLES];
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(mic_handle, buf, sizeof(buf), &bytes_read, 0);
    if (err != ESP_OK || bytes_read == 0) return mic_loudness;

    int samples = bytes_read / sizeof(int32_t);
    int64_t sum_sq = 0;
    for (int i = 0; i < samples; i++) {
        int32_t s = buf[i] >> 8;  // 24-bit data in 32-bit frame
        sum_sq += (int64_t)s * s;
    }
    float rms = sqrtf((float)(sum_sq / samples));

    // Normalize — typical I2S mic range, adjust if needed
    float level = rms / 100000.0f;
    if (level > 1.0f) level = 1.0f;

    // Smooth: fast attack, slow decay
    if (level > mic_loudness)
        mic_loudness = mic_loudness * 0.3f + level * 0.7f;
    else
        mic_loudness = mic_loudness * 0.9f + level * 0.1f;

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

/** Draw a filled vertical slit (cat pupil) centred at (cx, cy).
 *  h = half-height, w = half-width at the widest point (middle). */
static void draw_cat_pupil(int cx, int cy, int h, int w, uint16_t col)
{
    for (int dy = -h; dy <= h; dy++) {
        // Elliptical profile: wider in the middle, pointed at top/bottom
        float t = (float)dy / (float)h;          // -1..1
        int half_w = (int)(w * sqrtf(1.0f - t * t));  // ellipse width
        if (half_w < 1) half_w = 1;
        hline(cx - half_w, cx + half_w, cy + dy, col);
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
// so zoom can be applied at draw time without trig
static uint8_t *spiral_angle_lut = NULL;  // angle_component * 256
static uint8_t *spiral_dist_lut  = NULL;  // log(dist) * 256 (pre-scaled by 1.0)
static uint8_t *spiral_mask      = NULL;  // 1 = inside circle, 0 = outside

static void spiral_lut_init(void)
{
    if (spiral_angle_lut) return;
    const int W = board->lcd_w, H = board->lcd_h;
    const int CX = board->eye_cx, CY = board->eye_cy;
    const float SR = (float)board->sclera_r;

    spiral_angle_lut = heap_caps_malloc(lcd_pixels, MALLOC_CAP_SPIRAM);
    spiral_dist_lut  = heap_caps_malloc(lcd_pixels, MALLOC_CAP_SPIRAM);
    spiral_mask      = heap_caps_malloc(lcd_pixels, MALLOC_CAP_SPIRAM);
    if (!spiral_angle_lut || !spiral_dist_lut || !spiral_mask) {
        ESP_LOGE(TAG, "Spiral LUT alloc failed");
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
            spiral_angle_lut[i] = (uint8_t)((int)(angle / (2.0f * M_PI) * 256.0f) & 0xFF);
            float log_d = (dist > 1.0f) ? logf(dist) : 0;
            spiral_dist_lut[i] = (uint8_t)((int)(log_d * 48.0f) & 0xFF);
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

static void eye_draw_hypnotoad(void)
{
    if (!spiral_angle_lut) return;

    uint16_t palette[4] = {
        rgb(spiral_color_a[0], spiral_color_a[1], spiral_color_a[2]),
        rgb(spiral_color_b[0], spiral_color_b[1], spiral_color_b[2]),
        rgb(spiral_color_c[0], spiral_color_c[1], spiral_color_c[2]),
        rgb(spiral_color_d[0], spiral_color_d[1], spiral_color_d[2]),
    };

    uint8_t phase_offset = (uint8_t)((int)(spiral_phase * 256.0f) & 0xFF);
    // dist_lut stores log(dist)*48. We want: dist_component = log(dist)*zoom*256/(2*PI)
    // = dist_lut * zoom * 256 / (48 * 2 * PI)
    // ≈ dist_lut * zoom * 0.8488
    uint16_t zoom_mult = (uint16_t)(spiral_zoom * 256.0f * 256.0f / (48.0f * 2.0f * 3.14159f));

    uint16_t black = COL_BLACK;
    for (int i = 0; i < lcd_pixels; i++) {
        if (!spiral_mask[i]) {
            draw_fb[i] = black;
        } else {
            uint8_t ang = spiral_angle_lut[i];
            uint8_t dist_val = (uint8_t)((spiral_dist_lut[i] * zoom_mult) >> 8);
            uint8_t combined = ang + dist_val - phase_offset;
            uint8_t band = (combined >> 6) & 0x03;
            draw_fb[i] = palette[band];
        }
    }

    draw_circle(board->eye_cx, board->eye_cy, 20, COL_PUPIL);
}

// ─────────────────────────────────────────────────────────────────────────────
// Blob eye — pre-rendered animation playback
// ─────────────────────────────────────────────────────────────────────────────
static float blob_speed = 1.0f;
static float blob_pulse_threshold = 0.7f;
static float blob_pulse = 0;
static uint8_t blob_color_a[3] = { 255, 120,   0 };  // tint color (unused for now)
static uint8_t blob_color_b[3] = {  12,   2,   1 };  // background tint (unused for now)

// ─────────────────────────────────────────────────────────────────────────────
// Generic pre-rendered animation loader (shared by blob eye, sauron, etc.)
// Reads from the "eyedata" flash partition which contains a TOC + multiple
// animations packed by pack_eyedata.py.
// ─────────────────────────────────────────────────────────────────────────────
#include "esp_partition.h"

static uint16_t anim_palette_rgb565[64];
static uint8_t *anim_all_frames = NULL;
static int anim_frame_w, anim_frame_h, anim_num_frames;
static int anim_frame_idx = 0;
static int anim_frame_dir = 1;  // ping-pong direction
static int anim_playback = 0;   // 0=ping-pong, 1=loop
static bool anim_inited = false;

// Decode a 6-bit RLE bitstream into pixel buffer
static void anim_decode_rle(const uint8_t *comp, int comp_size, uint8_t *out, int num_pixels)
{
    int bit_pos = 0;
    int px = 0;

    #define READ6() ({ \
        int _byte = bit_pos >> 3; \
        int _bit  = bit_pos & 7; \
        bit_pos += 6; \
        ((comp[_byte] << _bit) | (comp[_byte + 1] >> (8 - _bit))) >> 2 & 0x3F; \
    })

    while (px < num_pixels) {
        int v = READ6();
        if (v != 0) {
            out[px++] = v;
        } else {
            int color = READ6();
            int count = READ6();
            if (count == 0) count = 1;
            int end = px + count;
            if (end > num_pixels) end = num_pixels;
            memset(out + px, color, end - px);
            px = end;
        }
    }
    #undef READ6
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

    // Allocate all decoded frames in PSRAM
    int frame_pixels = anim_frame_w * anim_frame_h;
    int total = frame_pixels * anim_num_frames;
    anim_all_frames = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!anim_all_frames) {
        ESP_LOGE(TAG, "Anim PSRAM alloc failed (%d bytes)", total);
        heap_caps_free(offsets);
        return;
    }

    // Temp buffer for compressed data
    int max_comp = frame_pixels;
    uint8_t *comp_buf = heap_caps_malloc(max_comp + 2, MALLOC_CAP_SPIRAM);
    if (!comp_buf) {
        ESP_LOGE(TAG, "Anim comp buffer alloc failed");
        heap_caps_free(offsets);
        heap_caps_free(anim_all_frames);
        anim_all_frames = NULL;
        return;
    }
    memset(comp_buf, 0, max_comp + 2);

    // Decode all frames — offsets in the file are relative to file start,
    // but we need partition-relative, so add anim_offset
    for (int f = 0; f < anim_num_frames; f++) {
        int comp_size;
        if (f + 1 < anim_num_frames)
            comp_size = offsets[f + 1] - offsets[f];
        else
            comp_size = max_comp;
        if (comp_size > max_comp) comp_size = max_comp;

        esp_partition_read(part, anim_offset + offsets[f], comp_buf, comp_size);
        anim_decode_rle(comp_buf, comp_size,
                        anim_all_frames + f * frame_pixels, frame_pixels);
    }

    heap_caps_free(comp_buf);
    heap_caps_free(offsets);

    anim_frame_idx = 0;
    anim_frame_dir = 1;
    anim_inited = true;
    ESP_LOGI(TAG, "Anim '%s': %d×%d, %d frames decoded (%.1f MB PSRAM)",
             name, anim_frame_w, anim_frame_h, anim_num_frames,
             total / (1024.0f * 1024.0f));
}

static void anim_free(void)
{
    if (anim_all_frames) { heap_caps_free(anim_all_frames); anim_all_frames = NULL; }
    anim_inited = false;
    ESP_LOGI(TAG, "Anim frames freed");
}

static void eye_draw_anim(void)
{
    if (!anim_all_frames) return;

    const int W = board->lcd_w, H = board->lcd_h;
    const int CX = board->eye_cx, CY = board->eye_cy;
    const int SR = board->sclera_r;

    const uint8_t *frame = anim_all_frames +
        anim_frame_idx * anim_frame_w * anim_frame_h;
    uint16_t bg = anim_palette_rgb565[0];
    uint16_t black = COL_BLACK;

    int scaled_w = anim_frame_w * 2;
    int scaled_h = anim_frame_h * 2;
    int ox = (W - scaled_w) / 2;
    int oy = (H - scaled_h) / 2;
    int eye_r2 = SR * SR;

    for (int y = 0; y < H; y++) {
        int row = y * W;
        int dy = y - CY;
        int dy2 = dy * dy;
        int sy = y - oy;
        int fy = sy >> 1;
        int in_frame_y = (sy >= 0 && sy < scaled_h && fy < anim_frame_h);
        const uint8_t *frame_row = in_frame_y ? frame + fy * anim_frame_w : NULL;

        for (int x = 0; x < W; x++) {
            int i = row + x;
            int dxx = x - CX;
            if (dxx * dxx + dy2 > eye_r2) {
                draw_fb[i] = black;
            } else if (frame_row) {
                int sx = x - ox;
                int fx = sx >> 1;
                if (sx >= 0 && sx < scaled_w && fx < anim_frame_w)
                    draw_fb[i] = anim_palette_rgb565[frame_row[fx]];
                else
                    draw_fb[i] = bg;
            } else {
                draw_fb[i] = bg;
            }
        }
    }

    if (anim_playback == 1) {
        anim_frame_idx = (anim_frame_idx + 1) % anim_num_frames;
    } else {
        anim_frame_idx += anim_frame_dir;
        if (anim_frame_idx >= anim_num_frames - 1)
            anim_frame_dir = -1;
        else if (anim_frame_idx <= 0)
            anim_frame_dir = 1;
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
        case MODE_CAT_EYE:   return "CAT_EYE";
        case MODE_HYPNOTOAD: return "HYPNOTOAD";
        case MODE_BLOB_EYE:  return "BLOB_EYE";
        case MODE_SAURON:    return "SAURON";
        default:             return "UNKNOWN";
    }
}

static display_mode_t active_mode = MODE_CAT_EYE;

static void eye_mode_switch(display_mode_t new_mode)
{
    if (new_mode == active_mode) return;

    // Free old mode resources
    switch (active_mode) {
        case MODE_CAT_EYE:   cat_eye_free(); break;
        case MODE_HYPNOTOAD: spiral_lut_free(); break;
        case MODE_BLOB_EYE:
        case MODE_SAURON:    anim_free(); break;
    }

    // Init new mode resources
    switch (new_mode) {
        case MODE_CAT_EYE:   iris_tex_init(); sclera_lut_init(); break;
        case MODE_HYPNOTOAD: spiral_lut_init(); break;
        case MODE_BLOB_EYE:  anim_init("blob"); break;
        case MODE_SAURON:    anim_init("sauron"); break;
    }

    active_mode = new_mode;
    ESP_LOGI(TAG, "Mode switch: %s → resources loaded", mode_name(new_mode));
}

static void eye_draw(void)
{
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
    else if (display_mode == MODE_BLOB_EYE)
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
        blob_pulse        = render_params.blob_pulse;
        memcpy(blob_color_a, render_params.blob_color_a, 3);
        memcpy(blob_color_b, render_params.blob_color_b, 3);

        eye_draw();

        xSemaphoreGive(render_done_sem);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Battery power latch
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
    if (board->pin_bat_control < 0) return;
    ESP_LOGW(TAG, "Powering off!");
    gpio_set_level(board->pin_bat_control, 0);
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// Battery ADC — GPIO8 has a ÷3 voltage divider, so battery_V = adc_V * 3
static adc_oneshot_unit_handle_t bat_adc_handle;
static float battery_voltage = 0;
static float battery_percent = 0;

static void battery_adc_init(void)
{
    if (board->pin_bat_adc < 0) {
        ESP_LOGI(TAG, "No battery ADC on this board (PMIC manages battery)");
        return;
    }

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

static float bat_adc_raw = 0;

static float battery_read_voltage(void)
{
    if (board->pin_bat_adc < 0) return 0;
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

    return battery_voltage;
}

#define PWR_BUTTON_SHUTDOWN_MS  2000  // Hold 2s to power off

void app_main(void)
{
    // Detect which board we're on BEFORE any hardware init
    board = detect_board();
    lcd_pixels = board->lcd_w * board->lcd_h;
    ESP_LOGI(TAG, "Eyeball starting up — Board: %s", board->name);

    power_latch_init();
    battery_adc_init();

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
    static const ble_param_t ble_params[] = {
        { 0x0010, "Display Mode",        BLE_PARAM_RWN,  &ble_display_mode,      1 },
        { 0x0011, "Blink Threshold",     BLE_PARAM_RW,   &blink_loud_threshold,  4 },
        { 0x0012, "Blink Close Speed",   BLE_PARAM_RW,   &blink_close_speed,     4 },
        { 0x0013, "Blink Open Speed",    BLE_PARAM_RW,   &blink_open_speed,      4 },
        { 0x0014, "Blink Hold Time",     BLE_PARAM_RW,   &blink_hold_time,       4 },
        { 0x0015, "Spiral Zoom",        BLE_PARAM_RW,   &spiral_zoom,           4 },
        { 0x0016, "Spiral Speed",       BLE_PARAM_RW,   &spiral_speed,          4 },
        { 0x0017, "Spiral Color A",     BLE_PARAM_RW,   &spiral_color_a,        3 },
        { 0x0018, "Spiral Color B",     BLE_PARAM_RW,   &spiral_color_b,        3 },
        { 0x0019, "Spiral Color C",     BLE_PARAM_RW,   &spiral_color_c,        3 },
        { 0x001A, "Spiral Color D",     BLE_PARAM_RW,   &spiral_color_d,        3 },
        { 0x001B, "Blob Speed",        BLE_PARAM_RW,   &blob_speed,            4 },
        { 0x001E, "Blob Pulse Thresh",BLE_PARAM_RW,   &blob_pulse_threshold,  4 },
        { 0x001C, "Blob Color A",      BLE_PARAM_RW,   &blob_color_a,          3 },
        { 0x001D, "Blob Color B",      BLE_PARAM_RW,   &blob_color_b,          3 },
        { 0x0020, "Mic Loudness",        BLE_PARAM_STAT, &mic_loudness,          4 },
        { 0x0021, "FPS",                 BLE_PARAM_STAT, &current_fps,           4 },
        { 0x0022, "Battery V",          BLE_PARAM_STAT, &battery_voltage,       4 },
        { 0x0023, "Battery %",          BLE_PARAM_STAT, &battery_percent,       4 },
        { 0x0024, "BAT ADC Raw",       BLE_PARAM_STAT, &bat_adc_raw,           4 },
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
    render_params.blob_pulse       = blob_pulse;
    render_params.sauron_blink_pos = sauron_blink_pos;
    memcpy(render_params.blob_color_a, blob_color_a, 3);
    memcpy(render_params.blob_color_b, blob_color_b, 3);
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

        // Blob pulse — trigger on loud noise, decay quickly
        if (mic_loudness > blob_pulse_threshold && blob_pulse < 0.1f) {
            blob_pulse = 1.0f;
            ESP_LOGI(TAG, "Blob pulse! loud=%.3f thresh=%.3f", mic_loudness, blob_pulse_threshold);
        }
        if (blob_pulse > 0) {
            blob_pulse -= dt * 2.0f;  // decay over ~0.5s
            if (blob_pulse < 0) blob_pulse = 0;
        }

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
        render_params.blob_pulse       = blob_pulse;
        render_params.sauron_blink_pos = sauron_blink_pos;
        memcpy(render_params.blob_color_a, blob_color_a, 3);
        memcpy(render_params.blob_color_b, blob_color_b, 3);
        render_params.target_fb    = framebuf[back_idx];

        // Start Core 1 rendering into back buffer
        xSemaphoreGive(flush_done_sem);

        int64_t t1 = esp_timer_get_time();

        // Flush front buffer — Core 1 is rendering in parallel
        lcd_flush();

        int64_t t2 = esp_timer_get_time();

        // --- Wait for Core 1 to finish rendering ---
        xSemaphoreTake(render_done_sem, portMAX_DELAY);

        int64_t t3 = esp_timer_get_time();

        // --- Perf tracking ---
        perf_sensor_us += (t1 - t0);
        perf_flush_us  += (t2 - t1);
        perf_wait_us   += (t3 - t2);

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
            if (board->pin_bat_control >= 0) {
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
