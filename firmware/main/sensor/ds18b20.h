/*
 * DS18B20 1-Wire Temperature Sensor Driver
 * Bit-bang implementation for ESP32 on GPIO 4
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#define DS18B20_MAX_SENSORS  4
#define DS18B20_GPIO         4

/**
 * Initialise the 1-Wire bus on DS18B20_GPIO.
 * Returns ESP_OK if at least one device responds to a reset pulse.
 */
esp_err_t ds18b20_init(void);

/**
 * Issue a Convert-T to all devices, wait for conversion, then read
 * scratchpads one by one.
 *
 * @param temps  Output array (must hold DS18B20_MAX_SENSORS floats)
 * @param count  Number of sensors actually read
 * @return ESP_OK on success
 */
esp_err_t ds18b20_read_temps(float *temps, int *count);
