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
#include "driver/gpio.h"
#include "driver/i2c.h"
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

// TCA9554 GPIO expander
#define TCA9554_ADDR   0x20
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
// ─────────────────────────────────────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────────────────────
// TCA9554 GPIO expander
// ─────────────────────────────────────────────────────────────────────────────
static uint8_t tca_output = 0xFF;   // cached output register

static void tca9554_init(void)
{
    // Configure all pins as outputs (0 = output in TCA9554 config register)
    i2c_write_reg(TCA9554_ADDR, TCA9554_CFG, 0x00);
    // Drive all high initially
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
        .pclk_hz            = 40 * 1000 * 1000,
        .lcd_cmd_bits       = 32,
        .lcd_param_bits     = 8,
        .spi_mode           = 0,
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
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,   // reset handled via TCA9554 above
        .color_space    = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_spd2010(io_handle, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    // Backlight on
    gpio_set_level(PIN_LCD_BL, 1);
    ESP_LOGI(TAG, "Display ready (SPD2010 QSPI 412x412)");
}

/** Push the full draw_fb to the display via the esp_lcd DMA path. */
static void lcd_flush(void)
{
    // Top half
    memcpy(fb[0], draw_fb,                   LCD_W * HALF_H * sizeof(uint16_t));
    // Bottom half
    memcpy(fb[1], draw_fb + LCD_W * HALF_H,  LCD_W * HALF_H * sizeof(uint16_t));

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
#define QMI_AX_L     0x35

static uint8_t qmi_addr = 0x6B;

static void imu_init(void)
{
    uint8_t who = 0;
    if (i2c_read_reg(qmi_addr, QMI_WHO_AM_I, &who, 1) != ESP_OK || who != 0x05) {
        qmi_addr = 0x6A;
        i2c_read_reg(qmi_addr, QMI_WHO_AM_I, &who, 1);
    }
    ESP_LOGI(TAG, "QMI8658 WHO_AM_I=0x%02X @ 0x%02X", who, qmi_addr);

    i2c_write_reg(qmi_addr, QMI_CTRL1, 0x20);   // little-endian, addr auto-inc
    i2c_write_reg(qmi_addr, QMI_CTRL2, 0x16);   // accel ±4 g, 58.75 Hz
    i2c_write_reg(qmi_addr, QMI_CTRL7, 0x03);   // enable accel + gyro
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "IMU ready");
}

static void imu_accel(float *ax, float *ay, float *az)
{
    uint8_t raw[6];
    if (i2c_read_reg(qmi_addr, QMI_AX_L, raw, 6) != ESP_OK) {
        *ax = *ay = *az = 0; return;
    }
    // ±4 g range: 1 g = 8192 LSB
    *ax = (int16_t)((raw[1] << 8) | raw[0]) / 8192.0f;
    *ay = (int16_t)((raw[3] << 8) | raw[2]) / 8192.0f;
    *az = (int16_t)((raw[5] << 8) | raw[4]) / 8192.0f;
}

// ─────────────────────────────────────────────────────────────────────────────
// Eyeball state & animation
// ─────────────────────────────────────────────────────────────────────────────
#define MAX_IRIS_TRAVEL  65.0f   // max pupil offset from centre — scaled for 412px

typedef struct {
    float px, py;           // current iris offset
    float vx, vy;           // velocity (px/s)
    float tx, ty;           // spring target
    float blink;            // 0=open .. 1=closed
    bool  blinking;
    float blink_t;
    float prev_az;
    bool  az_init;
    int64_t next_saccade_us;
    float sacc_tx, sacc_ty;
    bool  in_saccade;
    float sacc_t;
} EyeState;

static EyeState eye;

static void eye_init(void)
{
    memset(&eye, 0, sizeof(eye));
    eye.next_saccade_us = esp_timer_get_time() + (2000 + esp_random() % 2000) * 1000LL;
}

static void eye_update(float ax, float ay, float az, float dt)
{
    // 1. Tilt target
    float tx = -ax * 85.0f;
    float ty =  ay * 85.0f;
    float dist = sqrtf(tx*tx + ty*ty);
    if (dist > MAX_IRIS_TRAVEL) { tx = tx/dist*MAX_IRIS_TRAVEL; ty = ty/dist*MAX_IRIS_TRAVEL; }
    eye.tx = tx;
    eye.ty = ty;

    // 2. Idle saccade
    int64_t now = esp_timer_get_time();
    if (!eye.in_saccade && now >= eye.next_saccade_us) {
        float angle = (float)(esp_random() % 628) / 100.0f;
        float r     = (float)(esp_random() % (int)MAX_IRIS_TRAVEL);
        eye.sacc_tx = cosf(angle) * r;
        eye.sacc_ty = sinf(angle) * r;
        eye.in_saccade = true;
        eye.sacc_t = 0;
    }
    if (eye.in_saccade) {
        eye.sacc_t += dt;
        if (eye.sacc_t < 0.12f) { eye.tx = eye.sacc_tx; eye.ty = eye.sacc_ty; }
        else if (eye.sacc_t > 0.4f) {
            eye.in_saccade = false;
            eye.next_saccade_us = now + (2000 + esp_random() % 3000) * 1000LL;
        }
    }

    // 3. Spring-damper
    const float k = 14.0f, c = 6.0f;
    eye.vx += (-k * (eye.px - eye.tx) - c * eye.vx) * dt;
    eye.vy += (-k * (eye.py - eye.ty) - c * eye.vy) * dt;
    eye.px += eye.vx * dt;
    eye.py += eye.vy * dt;

    // 4. Jump / blink detection  (sharp ΔZ)
    if (!eye.az_init) { eye.prev_az = az; eye.az_init = true; }
    float daz = az - eye.prev_az;
    eye.prev_az = az;
    if (!eye.blinking && fabsf(daz) > 1.4f) {
        eye.blinking = true;
        eye.blink_t  = 0;
        ESP_LOGI(TAG, "Blink! daz=%.2f", daz);
    }

    // 5. Blink animation (triangle wave over 350 ms)
    if (eye.blinking) {
        eye.blink_t += dt;
        const float HALF = 0.175f;
        float t = eye.blink_t / HALF;
        eye.blink = (t < 1.0f) ? t : (2.0f - t);
        if (eye.blink < 0) eye.blink = 0;
        if (eye.blink > 1) eye.blink = 1;
        if (eye.blink_t >= HALF * 2.0f) { eye.blinking = false; eye.blink = 0; }
    }
}

static void eye_draw(void)
{
    int px = EYE_CX + (int)eye.px;
    int py = EYE_CY + (int)eye.py;

    // ── Background ─────────────────────────────────────────────────────────
    memset(draw_fb, 0, LCD_PIXELS * sizeof(uint16_t));

    // ── Sclera ─────────────────────────────────────────────────────────────
    draw_circle(EYE_CX, EYE_CY, 195, COL_SCLERA);

    // Subtle blood vessels near the corners
    draw_circle(EYE_CX - 130, EYE_CY + 35, 10, COL_VESSEL);
    draw_circle(EYE_CX + 122, EYE_CY - 25,  8, COL_VESSEL);
    draw_circle(EYE_CX - 130, EYE_CY + 35,  6, COL_SCLERA);
    draw_circle(EYE_CX + 122, EYE_CY - 25,  5, COL_SCLERA);

    // ── Iris ───────────────────────────────────────────────────────────────
    draw_circle(px, py, 110, COL_IRIS_RIM);   // limbal ring
    draw_circle(px, py, 102, COL_IRIS);        // main iris
    draw_circle(px, py,  76, COL_IRIS_RIM);   // darker inner zone
    draw_circle(px, py,  70, COL_IRIS_IN);    // inner iris

    // ── Pupil ──────────────────────────────────────────────────────────────
    draw_circle(px, py, 47, COL_PUPIL);

    // ── Corneal glints ─────────────────────────────────────────────────────
    draw_circle(px + 27, py - 27, 18, COL_GLINT1);  // main specular
    draw_circle(px - 21, py + 30,  8, COL_GLINT2);  // secondary glint

    // ── Eyelids ────────────────────────────────────────────────────────────
    // upper_y sweeps DOWN as blink→1 (lid closes from top)
    // lower_y sweeps UP  as blink→1 (lid closes from bottom)
    int upper_y = (int)(-6   + eye.blink * (EYE_CY + 16));
    int lower_y = (int)(LCD_H + 6 - eye.blink * (LCD_H - EYE_CY + 16));

    if (upper_y > 0) {
        draw_hband(0,           upper_y - 8, COL_SKIN);
        draw_hband(upper_y - 7, upper_y,     COL_LASH);
    }
    if (lower_y < LCD_H) {
        draw_hband(lower_y,     lower_y + 7, COL_LASH);
        draw_hband(lower_y + 8, LCD_H - 1,   COL_SKIN);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Entry point
// ─────────────────────────────────────────────────────────────────────────────
void app_main(void)
{
    ESP_LOGI(TAG, "Eyeball starting up (1.46\" SPD2010)");

    // Full-screen draw buffer in PSRAM (~330 KB)
    draw_fb = heap_caps_malloc(LCD_PIXELS * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    // Two half-height DMA buffers
    fb[0] = heap_caps_malloc(LCD_W * HALF_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    fb[1] = heap_caps_malloc(LCD_W * HALF_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);

    if (!draw_fb || !fb[0] || !fb[1]) {
        ESP_LOGE(TAG, "PSRAM alloc failed! Enable CONFIG_SPIRAM in menuconfig.");
        return;
    }
    ESP_LOGI(TAG, "Framebuffers: %.1f KB in PSRAM",
             (float)(LCD_PIXELS + LCD_W * LCD_H) * 2.0f / 1024.0f);

    i2c_init();
    tca9554_init();
    lcd_init();
    imu_init();
    eye_init();

    int64_t prev_us = esp_timer_get_time();

    while (1) {
        int64_t now_us = esp_timer_get_time();
        float dt = (now_us - prev_us) * 1e-6f;
        prev_us = now_us;
        if (dt > 0.1f) dt = 0.1f;

        float ax, ay, az;
        imu_accel(&ax, &ay, &az);

        eye_update(ax, ay, az, dt);
        eye_draw();
        lcd_flush();

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
