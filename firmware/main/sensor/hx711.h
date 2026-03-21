/*
 * HX711 24-bit ADC driver (bit-bang)
 * SCK = GPIO 18, DOUT = GPIO 19
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

#define HX711_SCK_GPIO   18
#define HX711_DOUT_GPIO  19

/**
 * Configure GPIOs and power-cycle the HX711.
 */
esp_err_t hx711_init(void);

/**
 * Read a single 24-bit signed value from the HX711 (channel A, gain 128).
 */
esp_err_t hx711_read_raw(int32_t *raw);

/**
 * Capture current reading as the tare (zero) offset.
 * Averages 10 readings.
 */
esp_err_t hx711_set_tare(void);

/**
 * Return the weight in grams, compensated for tare.
 * Requires hx711_set_tare() or manual offset before use.
 */
esp_err_t hx711_get_grams(float *grams);

/**
 * Set the calibration scale factor (raw units per gram).
 * Default is 420.0 -- adjust after calibration with a known weight.
 */
void hx711_set_scale(float scale);
