/*
 * SSD1306 128x64 OLED I2C driver. Drawing lives in gfx.c / ui.c; this
 * file only talks to the panel.
 *
 * Assumes the I2C master driver is already installed (by bme280_init).
 * Shares I2C port 0 with the BME280.
 */

#include "oled.h"

#include <string.h>
#include <stdio.h>
#include "driver/i2c.h"
#include "esp_log.h"
#include "sensor/bme280_i2c.h"   /* for BME280_I2C_PORT */

static const char *TAG = "oled";

/* ------------------------------------------------------------------ */
/*  SSD1306 I2C helpers                                               */
/* ------------------------------------------------------------------ */

static esp_err_t oled_send_cmd(uint8_t cmd)
{
    uint8_t buf[2] = { 0x00, cmd };   /* Co=0, D/C#=0 (command) */
    return i2c_master_write_to_device(BME280_I2C_PORT, OLED_I2C_ADDR,
                                      buf, sizeof(buf),
                                      pdMS_TO_TICKS(100));
}

static esp_err_t oled_send_data(const uint8_t *data, size_t len)
{
    /* Control byte (0x40 = Co=0, D/C#=1) + one page. Static: only the
       display task sends data, and it avoids a malloc per page. */
    static uint8_t buf[OLED_WIDTH + 1];
    if (len > OLED_WIDTH) return ESP_ERR_INVALID_SIZE;
    buf[0] = 0x40;
    memcpy(buf + 1, data, len);
    return i2c_master_write_to_device(BME280_I2C_PORT, OLED_I2C_ADDR,
                                      buf, len + 1, pdMS_TO_TICKS(200));
}

esp_err_t oled_flush(const uint8_t *fb)
{
    /* Column 0..127, page 0..7 */
    static const uint8_t window[] = { 0x21, 0x00, 0x7F, 0x22, 0x00, 0x07 };
    for (size_t i = 0; i < sizeof(window); i++) {
        esp_err_t err = oled_send_cmd(window[i]);
        if (err != ESP_OK) return err;
    }
    /* One page (128 bytes) per transfer */
    for (int page = 0; page < OLED_HEIGHT / 8; page++) {
        esp_err_t err = oled_send_data(&fb[page * OLED_WIDTH], OLED_WIDTH);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

esp_err_t oled_init(void)
{
    /* SSD1306 initialisation sequence for 128x64 */
    static const uint8_t init_cmds[] = {
        0xAE,       /* Display OFF */
        0xD5, 0x80, /* Set display clock div ratio */
        0xA8, 0x3F, /* Set multiplex ratio (64-1) */
        0xD3, 0x00, /* Set display offset = 0 */
        0x40,       /* Set start line = 0 */
        0x8D, 0x14, /* Enable charge pump */
        0x20, 0x00, /* Memory addressing mode = horizontal */
        0xA1,       /* Segment re-map (col 127 = SEG0) */
        0xC8,       /* COM output scan direction (remapped) */
        0xDA, 0x12, /* COM pins hardware config */
        0x81, 0xCF, /* Contrast */
        0xD9, 0xF1, /* Pre-charge period */
        0xDB, 0x40, /* VCOMH deselect level */
        0xA4,       /* Entire display ON (follow RAM) */
        0xA6,       /* Normal display (not inverted) */
        0xAF,       /* Display ON */
    };

    for (size_t i = 0; i < sizeof(init_cmds); i++) {
        esp_err_t err = oled_send_cmd(init_cmds[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OLED init cmd 0x%02X failed", init_cmds[i]);
            return err;
        }
    }

    oled_clear();
    ESP_LOGI(TAG, "SSD1306 128x64 OLED initialised (addr=0x%02X)", OLED_I2C_ADDR);
    return ESP_OK;
}

void oled_clear(void)
{
    static const uint8_t blank[OLED_WIDTH * OLED_HEIGHT / 8];
    oled_flush(blank);
}

void oled_set_contrast(uint8_t level)
{
    oled_send_cmd(0x81);
    oled_send_cmd(level);
}
