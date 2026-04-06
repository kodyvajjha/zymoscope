/*
 * SSD1306 128x64 OLED I2C driver
 * Address 0x3C, shares I2C bus with BME280 (must init bme280 first)
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#define OLED_I2C_ADDR  0x3C
#define OLED_WIDTH     128
#define OLED_HEIGHT    64

/**
 * Initialise the SSD1306 display.
 * Assumes the I2C master is already installed (bme280_init does this).
 */
esp_err_t oled_init(void);

/**
 * Clear the entire framebuffer and push to display.
 */
void oled_clear(void);

/**
 * Draw a status screen with the main fermentation telemetry.
 */
void oled_show_status(float temp, float ambient_temp, float setpoint,
                      int heater, int cooler, bool wifi_connected);
