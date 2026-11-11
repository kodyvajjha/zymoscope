/*
 * SSD1306 128x64 OLED I2C driver
 * Address 0x3C, shares I2C bus with BME280 (must init bme280 first)
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define OLED_I2C_ADDR  0x3C
#define OLED_WIDTH     128
#define OLED_HEIGHT    64

/**
 * Initialise the SSD1306 display.
 * Assumes the I2C master is already installed (bme280_init does this).
 */
esp_err_t oled_init(void);

/** Blank the panel. */
void oled_clear(void);

/** Push a 1024-byte framebuffer (gfx.h layout) to the panel. ~95 ms at
 *  100 kHz I2C. */
esp_err_t oled_flush(const uint8_t *fb);

/** 0x00..0xFF; lower is dimmer and slows burn-in. */
void oled_set_contrast(uint8_t level);
