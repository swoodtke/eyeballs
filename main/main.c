/**
 * @file main.c
 * @brief Animated eyeball – Waveshare ESP32-S3-Touch-LCD-1.46
 *
 * Hardware summary
 * ─────────────────────────────────────────────────────────────
 * Display  : SPD2010, 412×412 round AMOLED, QSPI (4 data lines)
 * IMU      : QMI8658, I2C address 0x6B (or 0x6A)
 * IO expdr : TCA9554, I2C address 0x20 — controls LCD_RST & TP_RST
 *
 * Pin map (from Waveshare schematic / Spotpear wiki)
 * ─────────────────────────────────────────────────────────────
 *  QSPI  LCD_SDA0 → GPIO46   LCD_SDA1 → GPIO45
 *        LCD_SDA2 → GPIO42   LCD_SDA3 → GPIO41
 *        LCD_SCK  → GPIO40   LCD_CS   → GPIO21
 *        LCD_TE   → GPIO18   LCD_BL   → GPIO5
 *        LCD_RST  → TCA9554 EXIO2 (via I2C)
 *
 *  I2C   SDA → GPIO11   SCL → GPIO10   (shared: IMU, touch, RTC, expander)
 *
 * Behaviour
 * ─────────────────────────────────────────────────────────────
 *  Tilt board       → pupil follows gravity (spring-damper physics)
 *  Jump / tap       → blink (sharp ΔZ from accelerometer)
 *  Idle > 2-5 s     → random saccade (quick flick to a new position)
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
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "eye";

// ─────────────────────────────────────────────────────────────────────────────
// Pin definitions
// ─────────────────────────────────────────────────────────────────────────────
#define PIN_LCD_SDA0   46
#define PIN_LCD_SDA1   45
#define PIN_LCD_SDA2   42
#define PIN_LCD_SDA3   41
#define PIN_LCD_SCK    40
#define PIN_LCD_CS     21
#define PIN_LCD_TE     18
#define PIN_LCD_BL      5

#define PIN_I2C_SDA    11
#define PIN_I2C_SCL    10

#define PIN_MIC_WS      2
#define PIN_MIC_SCK    15
#define PIN_MIC_SD     39

#define PIN_TP_INT      4

// I2C device addresses
#define TCA9554_ADDR   0x20
#define TP_ADDR        0x53
#define TCA9554_INPUT  0x00
#define TCA9554_OUTPUT 0x01
#define TCA9554_CFG    0x03
// EXIO bit positions on TCA9554 (from schematic)
#define EXIO_TP_RST    (1 << 1)   // EXIO1
#define EXIO_LCD_RST   (1 << 2)   // EXIO2

// ─────────────────────────────────────────────────────────────────────────────
// Display geometry
// ─────────────────────────────────────────────────────────────────────────────
#define LCD_W     412
#define LCD_H     412
#define LCD_PIXELS (LCD_W * LCD_H)
#define EYE_CX   (LCD_W / 2)
#define EYE_CY   (LCD_H / 2)

// ─────────────────────────────────────────────────────────────────────────────
// Framebuffer  (412×412×2 = ~330 KB → PSRAM)
// We split into two half-height buffers so esp_lcd can DMA one while we draw
// into the other (double-buffer approach).
// ─────────────────────────────────────────────────────────────────────────────
#define HALF_H   (LCD_H / 2)
static uint16_t *fb[2];     // fb[0] = top half, fb[1] = bottom half
static uint16_t *draw_fb;   // flat full-screen buffer we draw into
static esp_lcd_panel_handle_t panel;

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
    if ((unsigned)x < LCD_W && (unsigned)y < LCD_H)
        draw_fb[y * LCD_W + x] = col;
}

static inline void hline(int x0, int x1, int y, uint16_t col)
{
    if ((unsigned)y >= LCD_H) return;
    if (x0 < 0)        x0 = 0;
    if (x1 >= LCD_W)   x1 = LCD_W - 1;
    uint16_t *row = draw_fb + y * LCD_W;
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
    if (y0 < 0)        y0 = 0;
    if (y1 >= LCD_H)   y1 = LCD_H - 1;
    if (y0 > y1) return;
    uint16_t *row = draw_fb + y0 * LCD_W;
    for (int r = y0; r <= y1; r++, row += LCD_W)
        for (int x = 0; x < LCD_W; x++)
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
    if (addr == TCA9554_ADDR)  i2c_txn_tca++;
    else if (addr == qmi_addr) i2c_txn_qmi++;
    else if (addr == TP_ADDR)  i2c_txn_tp++;
}

#ifdef USE_NEW_I2C_API

static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t i2c_devs[128];  // handle cache by 7-bit addr

static void i2c_init(void)
{
    memset(i2c_devs, 0, sizeof(i2c_devs));
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
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
        .sda_io_num       = PIN_I2C_SDA,
        .scl_io_num       = PIN_I2C_SCL,
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
// LCD init using esp_lcd + espressif/esp_lcd_spd2010 component
// ─────────────────────────────────────────────────────────────────────────────
static void lcd_init(void)
{
    // Backlight off during init
    gpio_config_t bl_cfg = {
        .pin_bit_mask = (1ULL << PIN_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(PIN_LCD_BL, 0);

    // Hardware reset via TCA9554 expander
    tca9554_set(EXIO_LCD_RST, false);
    vTaskDelay(pdMS_TO_TICKS(20));
    tca9554_set(EXIO_LCD_RST, true);
    vTaskDelay(pdMS_TO_TICKS(120));

    // Create QSPI panel IO
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num        = -1,    // QSPI has no D/C line; cmd embedded in transfer
        .cs_gpio_num        = PIN_LCD_CS,
        .pclk_hz            = 80 * 1000 * 1000,
        .lcd_cmd_bits       = 32,
        .lcd_param_bits     = 8,
        .spi_mode           = 3,
        .trans_queue_depth  = 10,
        .flags = {
            .quad_mode = true,
        },
    };

    // SPI bus config for QSPI
    spi_bus_config_t bus_cfg = {
        .data0_io_num   = PIN_LCD_SDA0,
        .data1_io_num   = PIN_LCD_SDA1,
        .data2_io_num   = PIN_LCD_SDA2,
        .data3_io_num   = PIN_LCD_SDA3,
        .sclk_io_num    = PIN_LCD_SCK,
        .max_transfer_sz = LCD_W * HALF_H * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                              &io_config, &io_handle));

    // Create SPD2010 panel
    spd2010_vendor_config_t vendor_cfg = {
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,   // reset handled via TCA9554 above
        .data_endian    = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
        .vendor_config  = &vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_spd2010(io_handle, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    // Backlight on
    gpio_set_level(PIN_LCD_BL, 1);
    ESP_LOGI(TAG, "Display ready (SPD2010 QSPI 412x412)");
}

// ─────────────────────────────────────────────────────────────────────────────
// Touch — direct I2C to SPD2010 integrated touch controller (addr 0x53)
// ─────────────────────────────────────────────────────────────────────────────

typedef enum { MODE_CAT_EYE, MODE_HYPNOTOAD } display_mode_t;
static display_mode_t display_mode = MODE_CAT_EYE;
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
    i2c_txn_tp++;
#ifdef USE_NEW_I2C_API
    return i2c_master_transmit(i2c_get_dev(TP_ADDR), data, len, 50);
#else
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (TP_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(h, data, len, true);
    i2c_master_stop(h);
    esp_err_t e = i2c_master_cmd_begin(I2C_NUM_0, h, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(h);
    return e;
#endif
}

static esp_err_t tp_i2c_write_read(const uint8_t *cmd, size_t cmd_len, uint8_t *out, size_t out_len)
{
    i2c_txn_tp++;
#ifdef USE_NEW_I2C_API
    return i2c_master_transmit_receive(i2c_get_dev(TP_ADDR), cmd, cmd_len, out, out_len, 100);
#else
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (TP_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(h, cmd, cmd_len, true);
    i2c_master_start(h);
    i2c_master_write_byte(h, (TP_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(h, out, out_len, I2C_MASTER_LAST_NACK);
    i2c_master_stop(h);
    esp_err_t e = i2c_master_cmd_begin(I2C_NUM_0, h, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(h);
    return e;
#endif
}

static void touch_init(void)
{

    // Read status to see if touch CPU needs starting
    uint8_t cmd[2] = {0x20, 0x00};
    uint8_t status[4] = {0};
    tp_i2c_write_read(cmd, 2, status, 4);

    bool in_bios = (status[1] >> 6) & 1;
    bool in_cpu  = (status[1] >> 5) & 1;

    if (in_bios) {
        // Clear INT + start CPU
        uint8_t clr[] = {0x02, 0x00, 0x01, 0x00};
        tp_i2c_write(clr, 4); esp_rom_delay_us(200);
        uint8_t cpu[] = {0x04, 0x00, 0x01, 0x00};
        tp_i2c_write(cpu, 4); esp_rom_delay_us(200);
        ESP_LOGI(TAG, "Touch: started CPU from BIOS");
        vTaskDelay(pdMS_TO_TICKS(100));
    } else if (in_cpu) {
        // Set point mode + start + clear INT
        uint8_t pm[] = {0x50, 0x00, 0x00, 0x00};
        tp_i2c_write(pm, 4); esp_rom_delay_us(200);
        uint8_t st[] = {0x46, 0x00, 0x00, 0x00};
        tp_i2c_write(st, 4); esp_rom_delay_us(200);
        uint8_t clr[] = {0x02, 0x00, 0x01, 0x00};
        tp_i2c_write(clr, 4); esp_rom_delay_us(200);
        ESP_LOGI(TAG, "Touch: configured point mode");
    }

    // Set up interrupt on TP_INT (active low)
    tp_sem = xSemaphoreCreateBinary();
    gpio_config_t tp_cfg = {
        .pin_bit_mask = (1ULL << PIN_TP_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&tp_cfg);
    gpio_isr_handler_add(PIN_TP_INT, tp_isr, NULL);

    ESP_LOGI(TAG, "Touch ready (tap to toggle mode, INT on GPIO%d)", PIN_TP_INT);
}

static uint32_t tp_serviced_count = 0;
static uint32_t tp_spurious_count = 0;

/** Check touch — only reads I2C when TP_INT fires. */
static bool touch_check(void)
{
    // Only proceed if the touch controller signalled an interrupt
    if (xSemaphoreTake(tp_sem, 0) != pdTRUE) return false;

    // Read status + length
    uint8_t cmd[2] = {0x20, 0x00};
    uint8_t status[4] = {0};
    if (tp_i2c_write_read(cmd, 2, status, 4) != ESP_OK) return false;

    bool pt_exist = status[0] & 0x01;
    uint16_t read_len = (status[3] << 8) | status[2];

    if (pt_exist && read_len > 0) {
        tp_serviced_count++;
        // Read touch data to clear the controller's interrupt
        uint8_t hdr[2] = {0x00, 0x03};
        uint8_t data[64] = {0};
        int rlen = read_len > sizeof(data) ? sizeof(data) : read_len;
        tp_i2c_write_read(hdr, 2, data, rlen);

        // Clear INT flag
        uint8_t clr[] = {0x02, 0x00, 0x01, 0x00};
        tp_i2c_write(clr, 4);

        // Debounce
        int64_t now = esp_timer_get_time();
        if (now - last_touch_us > 500000) {
            last_touch_us = now;
            return true;
        }
    } else {
        tp_spurious_count++;
        // INT fired but no point data — just clear the flag
        uint8_t clr[] = {0x02, 0x00, 0x01, 0x00};
        tp_i2c_write(clr, 4);
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
    te_sem = xSemaphoreCreateBinary();
    gpio_config_t te_cfg = {
        .pin_bit_mask = (1ULL << PIN_LCD_TE),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&te_cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_LCD_TE, te_isr, NULL);
}

/** Push draw_fb to the display. fb[0]/fb[1] alias draw_fb, so no memcpy needed. */
static void lcd_flush(void)
{
    esp_lcd_panel_draw_bitmap(panel, 0, 0,        LCD_W, HALF_H, fb[0]);
    esp_lcd_panel_draw_bitmap(panel, 0, HALF_H,   LCD_W, LCD_H,  fb[1]);
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
    if (++imu_log_counter >= 10) {
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

static void mic_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &mic_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_MIC_SCK,
            .ws   = PIN_MIC_WS,
            .din  = PIN_MIC_SD,
            .dout = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mic_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(mic_handle));
    ESP_LOGI(TAG, "Microphone ready (I2S %d Hz)", MIC_SAMPLE_RATE);
}

/** Read a block of mic samples and return RMS loudness (0..1). */
static float mic_read_loudness(void)
{
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

static void eye_init(void)
{
    memset(&eye, 0, sizeof(eye));
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
    if (da_mag > 0.05f) {
        ESP_LOGI(TAG, "RATTLE: dax=%.3f day=%.3f mag=%.3f rpx=%.1f rpy=%.1f rvx=%.1f rvy=%.1f",
                 dax, day, da_mag, eye.rpx, eye.rpy, eye.rvx, eye.rvy);
    }
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
    if (++log_counter >= 10) {
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
static int16_t sclera_x0[LCD_H];  // first sclera pixel on row
static int16_t sclera_x1[LCD_H];  // last sclera pixel on row (inclusive), -1 if none

static void sclera_lut_init(void)
{
    const int r = 195;
    const int r2 = r * r;
    for (int y = 0; y < LCD_H; y++) {
        int dy = y - EYE_CY;
        int dy2 = dy * dy;
        if (dy2 > r2) {
            sclera_x0[y] = 0;
            sclera_x1[y] = -1;
        } else {
            int dx = (int)sqrtf((float)(r2 - dy2));
            sclera_x0[y] = EYE_CX - dx;
            sclera_x1[y] = EYE_CX + dx;
            if (sclera_x0[y] < 0) sclera_x0[y] = 0;
            if (sclera_x1[y] >= LCD_W) sclera_x1[y] = LCD_W - 1;
        }
    }
}

static void eye_draw_cat(void)
{
    int ix = EYE_CX + (int)eye.px + (int)eye.rpx;
    int iy = EYE_CY + (int)eye.py + (int)eye.rpy;

    int pupil_w = 16 + (int)(mic_loudness * 70);

    for (int y = 0; y < LCD_H; y++) {
        uint16_t *row = draw_fb + y * LCD_W;
        int sx0 = sclera_x0[y];
        int sx1 = sclera_x1[y];

        // Black outside sclera
        for (int x = 0; x < sx0; x++) row[x] = COL_BLACK;
        for (int x = sx1 + 1; x < LCD_W; x++) row[x] = COL_BLACK;

        if (sx1 < 0) continue;  // entire row is black

        // Sclera fill
        for (int x = sx0; x <= sx1; x++) row[x] = COL_SCLERA;

        // Iris rim + iris (smaller, layered on top)
        int dy_i = y - iy;
        int dy_i2 = dy_i * dy_i;
        int rim_r2 = 110 * 110;
        int iris_r2 = 102 * 102;

        if (dy_i2 <= rim_r2) {
            int rim_dx = (int)sqrtf((float)(rim_r2 - dy_i2));
            int x0 = ix - rim_dx; if (x0 < sx0) x0 = sx0;
            int x1 = ix + rim_dx; if (x1 > sx1) x1 = sx1;
            for (int x = x0; x <= x1; x++) row[x] = COL_IRIS_RIM;

            if (dy_i2 <= iris_r2) {
                int iris_dx = (int)sqrtf((float)(iris_r2 - dy_i2));
                x0 = ix - iris_dx; if (x0 < sx0) x0 = sx0;
                x1 = ix + iris_dx; if (x1 > sx1) x1 = sx1;
                for (int x = x0; x <= x1; x++) row[x] = COL_IRIS;
            }
        }

        // Cat pupil slit
        int pupil_h = 90;
        if (dy_i >= -pupil_h && dy_i <= pupil_h) {
            float t = (float)dy_i / (float)pupil_h;
            int hw = (int)(pupil_w * sqrtf(1.0f - t * t));
            if (hw < 1) hw = 1;
            int x0 = ix - hw; if (x0 < sx0) x0 = sx0;
            int x1 = ix + hw; if (x1 > sx1) x1 = sx1;
            for (int x = x0; x <= x1; x++) row[x] = COL_PUPIL;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Hypnotoad spiral
// ─────────────────────────────────────────────────────────────────────────────
static float spiral_phase = 0;

// Spiral colour palette — alternating bands
#define COL_SPIRAL_A  rgb(255,  50,   0)   // red-orange
#define COL_SPIRAL_B  rgb(255, 220,   0)   // yellow
#define COL_SPIRAL_C  rgb( 20, 180,  20)   // green
#define COL_SPIRAL_D  rgb(255, 120,   0)   // orange

// Precomputed lookup tables for spiral (avoid per-pixel atan2f/sqrtf)
static uint8_t *spiral_lut = NULL;  // angle*256/(2*PI) + dist_scaled per pixel

static void spiral_lut_init(void)
{
    // Store (angle_component + dist_component) * 4 as uint8_t for each pixel
    // We only need it mod 4, so store as fixed-point and use at draw time
    spiral_lut = heap_caps_malloc(LCD_PIXELS, MALLOC_CAP_SPIRAM);
    if (!spiral_lut) {
        ESP_LOGE(TAG, "Spiral LUT alloc failed");
        return;
    }
    const float band_width = 40.0f;
    for (int y = 0; y < LCD_H; y++) {
        for (int x = 0; x < LCD_W; x++) {
            float dx = x - EYE_CX;
            float dy = y - EYE_CY;
            float dist_sq = dx * dx + dy * dy;
            if (dist_sq > 195.0f * 195.0f) {
                spiral_lut[y * LCD_W + x] = 0xFF; // outside circle marker
                continue;
            }
            float dist = sqrtf(dist_sq);
            float angle = atan2f(dy, dx);
            // Encode as fixed-point: (angle/(2*PI) + dist/band_width) * 256
            float val = (angle / (2.0f * M_PI) + dist / band_width) * 256.0f;
            // Store lower 8 bits (wraps naturally)
            spiral_lut[y * LCD_W + x] = (uint8_t)((int)val & 0xFF);
        }
    }
    ESP_LOGI(TAG, "Spiral LUT ready");
}

static void eye_draw_hypnotoad(void)
{
    if (!spiral_lut) return;

    const uint16_t palette[] = { COL_SPIRAL_A, COL_SPIRAL_B, COL_SPIRAL_C, COL_SPIRAL_D };
    // Phase offset as fixed-point matching LUT encoding
    uint8_t phase_offset = (uint8_t)((int)(spiral_phase * 256.0f) & 0xFF);

    uint16_t black = COL_BLACK;
    for (int i = 0; i < LCD_PIXELS; i++) {
        uint8_t lut_val = spiral_lut[i];
        if (lut_val == 0xFF) {
            draw_fb[i] = black;
        } else {
            // Subtract phase to make spiral move outward, divide by 64 for 4 bands
            uint8_t band = ((lut_val - phase_offset) >> 6) & 0x03;
            draw_fb[i] = palette[band];
        }
    }

    // Dark pupil in centre
    draw_circle(EYE_CX, EYE_CY, 30, COL_PUPIL);

    spiral_phase += 0.08f;
    if (spiral_phase > 1.0f) spiral_phase -= 1.0f;
}

static int mode_log_counter = 0;

static void eye_draw(void)
{
    if (++mode_log_counter >= 10) {
        mode_log_counter = 0;
        ESP_LOGI(TAG, "mode=%s", display_mode == MODE_HYPNOTOAD ? "HYPNOTOAD" : "CAT_EYE");
    }
    if (display_mode == MODE_HYPNOTOAD)
        eye_draw_hypnotoad();
    else
        eye_draw_cat();
}

// ─────────────────────────────────────────────────────────────────────────────
// Entry point
// ─────────────────────────────────────────────────────────────────────────────
void app_main(void)
{
    ESP_LOGI(TAG, "Eyeball starting up (1.46\" SPD2010)");

    // Single contiguous framebuffer in PSRAM (~330 KB)
    // draw_fb is the full screen; fb[0]/fb[1] point into top/bottom halves
    draw_fb = heap_caps_malloc(LCD_PIXELS * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (draw_fb) {
        fb[0] = draw_fb;
        fb[1] = draw_fb + LCD_W * HALF_H;
    }

    if (!draw_fb) {
        ESP_LOGE(TAG, "PSRAM alloc failed! Enable CONFIG_SPIRAM in menuconfig.");
        return;
    }
    ESP_LOGI(TAG, "Framebuffers: %.1f KB in PSRAM",
             (float)(LCD_PIXELS + LCD_W * LCD_H) * 2.0f / 1024.0f);

    i2c_init();
    tca9554_init();
    lcd_init();
    te_init();
    touch_init();
    imu_init();
    mic_init();
    eye_init();
    sclera_lut_init();
    spiral_lut_init();

    int64_t prev_us = esp_timer_get_time();

    int frame_count = 0;
    int64_t fps_timer_us = esp_timer_get_time();
    int64_t perf_sensor_us = 0, perf_draw_us = 0, perf_flush_us = 0;

    while (1) {
        int64_t now_us = esp_timer_get_time();
        float dt = (now_us - prev_us) * 1e-6f;
        prev_us = now_us;
        if (dt > 0.1f) dt = 0.1f;

        int64_t t0 = esp_timer_get_time();

        float ax, ay, az;
        imu_accel(&ax, &ay, &az);
        mic_read_loudness();

        if (touch_check()) {
            display_mode = (display_mode == MODE_CAT_EYE) ? MODE_HYPNOTOAD : MODE_CAT_EYE;
            ESP_LOGI(TAG, "Touch! Switching to %s",
                     display_mode == MODE_HYPNOTOAD ? "HYPNOTOAD" : "CAT_EYE");
        }

        int64_t t1 = esp_timer_get_time();

        eye_update(ax, ay, az, dt);
        eye_draw();

        int64_t t2 = esp_timer_get_time();

        lcd_flush();

        int64_t t3 = esp_timer_get_time();

        perf_sensor_us += (t1 - t0);
        perf_draw_us   += (t2 - t1);
        perf_flush_us  += (t3 - t2);

        // FPS counter — log every second
        frame_count++;
        int64_t elapsed = now_us - fps_timer_us;
        if (elapsed >= 1000000) {
            float fc = (float)frame_count;
            ESP_LOGI(TAG, "FPS: %.1f | sensor:%.1fms draw:%.1fms flush:%.1fms",
                     frame_count * 1e6f / elapsed,
                     perf_sensor_us / fc / 1000.0f,
                     perf_draw_us / fc / 1000.0f,
                     perf_flush_us / fc / 1000.0f);
            ESP_LOGI(TAG, "  I2C tca:%lu qmi:%lu tp:%lu | IMU_ERR: %lu | TE:%lu TP:%lu(s:%lu x:%lu)",
                     (unsigned long)i2c_txn_tca, (unsigned long)i2c_txn_qmi,
                     (unsigned long)i2c_txn_tp, (unsigned long)imu_err_count,
                     (unsigned long)te_isr_count, (unsigned long)tp_isr_count,
                     (unsigned long)tp_serviced_count, (unsigned long)tp_spurious_count);
            frame_count = 0;
            fps_timer_us = now_us;
            perf_sensor_us = perf_draw_us = perf_flush_us = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
