/**
 * @file board_config.h
 * @brief Runtime board detection and per-board configuration
 *
 * Supports two Waveshare ESP32-S3 round AMOLED dev boards:
 *   - ESP32-S3-Touch-LCD-1.46   (412×412, SPD2010)
 *   - ESP32-S3-Touch-AMOLED-1.75 (466×466, CO5300)
 *
 * CRITICAL GPIO OVERLAP — pins serve different functions on each board:
 *   GPIO4:  1.46" Touch INT    / 1.75" QSPI DATA0
 *   GPIO5:  1.46" Backlight    / 1.75" QSPI DATA1
 *   GPIO6:  1.46" PWR Button   / 1.75" QSPI DATA2
 *   GPIO7:  1.46" Power Latch  / 1.75" QSPI DATA3
 *   GPIO11: 1.46" I2C SDA      / 1.75" Touch INT
 *   GPIO15: 1.46" Mic SCK      / 1.75" I2C SDA
 *   GPIO39: 1.46" Mic SD       / 1.75" LCD RST
 *   GPIO46: 1.46" QSPI DATA0   / 1.75" Speaker PA
 *
 * Every peripheral init MUST use board-> fields, never raw GPIO numbers.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c.h"
#include "esp_log.h"

// Maximum dimensions across all boards — used for static array sizing only
#define LCD_MAX_W          466
#define LCD_MAX_H          466
#define SCLERA_R_MAX       220
#define SHADE_LUT_SIZE_MAX ((SCLERA_R_MAX * SCLERA_R_MAX >> 5) + 2)

typedef enum { BOARD_146 = 0, BOARD_175 = 1 } board_id_t;

typedef struct {
    board_id_t id;
    const char *name;

    // Display geometry
    int lcd_w, lcd_h;
    int eye_cx, eye_cy;

    // Eye scaling (proportional to display size)
    int sclera_r;
    int iris_tex_r;
    int iris_r, inner_r, rim_r;
    int pupil_hw;
    int slit_half_h;
    int sauron_base_w;

    // QSPI display pins
    int pin_lcd_sda0, pin_lcd_sda1, pin_lcd_sda2, pin_lcd_sda3;
    int pin_lcd_sck, pin_lcd_cs;
    int pin_lcd_te;    // -1 if not present
    int pin_lcd_bl;    // -1 if brightness via display command
    int pin_lcd_rst;   // -1 if reset via TCA9554

    // I2C bus
    int pin_i2c_sda, pin_i2c_scl;

    // Touch
    int pin_tp_int;
    int pin_tp_rst;    // -1 if reset via TCA9554
    uint8_t tp_addr;

    // Mic / audio (PDM mic on 1.46", ES8311 codec on 1.75")
    int pin_mic_ws;    // -1 if no PDM mic
    int pin_mic_sck;
    int pin_mic_sd;

    // ES8311 codec (1.75" only, -1 if not present)
    int pin_codec_mclk;
    int pin_codec_bclk;
    int pin_codec_ws;
    int pin_codec_din;   // codec → ESP32 (mic data)
    int pin_codec_dout;  // ESP32 → codec (speaker data)
    int pin_codec_pa;    // speaker PA enable
    bool has_codec;

    // Power management
    int pin_bat_control;  // -1 if using PMIC
    int pin_key_bat;      // -1 if no hardware button
    int pin_bat_adc;      // -1 if using PMIC

    // Display driver config
    int spi_mode;         // 3 for SPD2010, 0 for CO5300
    int x_gap, y_gap;     // CO5300 needs x_gap=6

    // Feature flags
    bool has_backlight_gpio;  // false on 1.75" (uses cmd 0x51)
    bool has_pmic;            // AXP2101 on 1.75"
    bool lcd_rst_via_tca;     // true on 1.46"
    bool tp_rst_via_tca;      // true on 1.46"
    bool use_spd2010;         // true=SPD2010, false=CO5300
} board_config_t;

// ─────────────────────────────────────────────────────────────────────────────
// Board A: ESP32-S3-Touch-LCD-1.46 (412×412, SPD2010)
// ─────────────────────────────────────────────────────────────────────────────
static const board_config_t board_config_146 = {
    .id   = BOARD_146,
    .name = "1.46in (412x412, SPD2010)",

    .lcd_w = 412, .lcd_h = 412,
    .eye_cx = 206, .eye_cy = 206,

    .sclera_r    = 195,
    .iris_tex_r  = 110,
    .iris_r      = 102,
    .inner_r     = 50,
    .rim_r       = 110,
    .pupil_hw    = 90,
    .slit_half_h = 70,
    .sauron_base_w = 14,

    .pin_lcd_sda0 = 46, .pin_lcd_sda1 = 45,
    .pin_lcd_sda2 = 42, .pin_lcd_sda3 = 41,
    .pin_lcd_sck  = 40, .pin_lcd_cs = 21,
    .pin_lcd_te   = 18,
    .pin_lcd_bl   = 5,
    .pin_lcd_rst  = -1,  // via TCA9554 EXIO2

    .pin_i2c_sda = 11, .pin_i2c_scl = 10,

    .pin_tp_int = 4,
    .pin_tp_rst = -1,    // via TCA9554 EXIO1
    .tp_addr    = 0x53,

    .pin_mic_ws  = 2,
    .pin_mic_sck = 15,
    .pin_mic_sd  = 39,

    .pin_codec_mclk = -1,
    .pin_codec_bclk = -1,
    .pin_codec_ws   = -1,
    .pin_codec_din  = -1,
    .pin_codec_dout = -1,
    .pin_codec_pa   = -1,
    .has_codec      = false,

    .pin_bat_control = 7,
    .pin_key_bat     = 6,
    .pin_bat_adc     = 8,

    .spi_mode = 3,
    .x_gap = 0, .y_gap = 0,

    .has_backlight_gpio = true,
    .has_pmic           = false,
    .lcd_rst_via_tca    = true,
    .tp_rst_via_tca     = true,
    .use_spd2010        = true,
};

// ─────────────────────────────────────────────────────────────────────────────
// Board B: ESP32-S3-Touch-AMOLED-1.75 (466×466, CO5300)
// ─────────────────────────────────────────────────────────────────────────────
static const board_config_t board_config_175 = {
    .id   = BOARD_175,
    .name = "1.75in (466x466, CO5300)",

    .lcd_w = 466, .lcd_h = 466,
    .eye_cx = 233, .eye_cy = 233,

    // Scaled from 1.46" by 466/412 = 1.131
    .sclera_r    = 220,
    .iris_tex_r  = 124,
    .iris_r      = 115,
    .inner_r     = 57,
    .rim_r       = 124,
    .pupil_hw    = 102,
    .slit_half_h = 79,
    .sauron_base_w = 16,

    .pin_lcd_sda0 = 4, .pin_lcd_sda1 = 5,
    .pin_lcd_sda2 = 6, .pin_lcd_sda3 = 7,
    .pin_lcd_sck  = 38, .pin_lcd_cs = 12,
    .pin_lcd_te   = 13,   // LCD_TE per schematic; CO5300 TE enabled via cmd 0x35
    .pin_lcd_bl   = -1,   // brightness via cmd 0x51
    .pin_lcd_rst  = 39,   // direct GPIO

    .pin_i2c_sda = 15, .pin_i2c_scl = 14,

    .pin_tp_int = 11,
    .pin_tp_rst = 40,     // direct GPIO
    .tp_addr    = 0x5A,   // CST9217

    .pin_mic_ws  = -1,    // no PDM mic
    .pin_mic_sck = -1,
    .pin_mic_sd  = -1,

    .pin_codec_mclk = 42,
    .pin_codec_bclk = 9,
    .pin_codec_ws   = 45,
    .pin_codec_din  = 10,   // ES7210 ASDOUT → ESP32 (mic data)
    .pin_codec_dout = 8,    // ESP32 DSDIN → ES8311 (speaker data)
    .pin_codec_pa   = 46,   // speaker PA enable
    .has_codec      = true,

    .pin_bat_control = -1,  // AXP2101 PMIC
    .pin_key_bat     = -1,
    .pin_bat_adc     = -1,

    .spi_mode = 0,
    .x_gap = 6, .y_gap = 0,

    .has_backlight_gpio = false,
    .has_pmic           = true,
    .lcd_rst_via_tca    = false,
    .tp_rst_via_tca     = false,
    .use_spd2010        = false,
};

// ─────────────────────────────────────────────────────────────────────────────
// Board detection — probe I2C for TCA9554 on each board's pin set
// ─────────────────────────────────────────────────────────────────────────────
#define DETECT_TCA9554_ADDR 0x20

static inline bool i2c_probe_addr(uint8_t addr)
{
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(h);
    esp_err_t e = i2c_master_cmd_begin(I2C_NUM_0, h, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(h);
    return (e == ESP_OK);
}

static inline bool try_i2c_pins(int sda, int scl)
{
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = sda,
        .scl_io_num       = scl,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,  // slow for detection reliability
    };
    if (i2c_param_config(I2C_NUM_0, &cfg) != ESP_OK) return false;
    if (i2c_driver_install(I2C_NUM_0, cfg.mode, 0, 0, 0) != ESP_OK) return false;

    bool found = i2c_probe_addr(DETECT_TCA9554_ADDR);

    i2c_driver_delete(I2C_NUM_0);
    return found;
}

/**
 * Detect which board we're running on by probing I2C.
 * Must be called before any other hardware init.
 */
static inline const board_config_t *detect_board(void)
{
    static const char *TAG_DET = "board_detect";

    // Try Board A pins first (SDA=11, SCL=10)
    ESP_LOGI(TAG_DET, "Probing I2C SDA=%d SCL=%d (1.46\" board)...",
             board_config_146.pin_i2c_sda, board_config_146.pin_i2c_scl);
    if (try_i2c_pins(board_config_146.pin_i2c_sda, board_config_146.pin_i2c_scl)) {
        ESP_LOGI(TAG_DET, "TCA9554 found — Board: %s", board_config_146.name);
        return &board_config_146;
    }

    // Try Board B pins (SDA=15, SCL=14)
    ESP_LOGI(TAG_DET, "Probing I2C SDA=%d SCL=%d (1.75\" board)...",
             board_config_175.pin_i2c_sda, board_config_175.pin_i2c_scl);
    if (try_i2c_pins(board_config_175.pin_i2c_sda, board_config_175.pin_i2c_scl)) {
        ESP_LOGI(TAG_DET, "TCA9554 found — Board: %s", board_config_175.name);
        return &board_config_175;
    }

    // Fallback — default to 1.46"
    ESP_LOGW(TAG_DET, "No board detected! Defaulting to %s", board_config_146.name);
    return &board_config_146;
}
