/*
 * BME280 I2C driver for ESP-IDF v5.x
 * Address 0x76, SDA = GPIO 21, SCL = GPIO 22
 */
#pragma once

#include "esp_err.h"

#define BME280_I2C_ADDR   0x76
#define BME280_SDA_GPIO   21
#define BME280_SCL_GPIO   22
#define BME280_I2C_PORT   0

/**
 * Install the I2C master driver (shared bus) and configure the BME280.
 */
esp_err_t bme280_init(void);

/**
 * Trigger a forced measurement and read compensated values.
 */
esp_err_t bme280_read(float *temp_c, float *humidity, float *pressure_hpa);
