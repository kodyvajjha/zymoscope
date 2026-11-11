/*
 * OLED screen manager: owns the display task, the 4 h chart history and
 * the current page. Other tasks only hand it data.
 *
 * Pages: main → chart → info (cycled by the "next" command from the
 * dashboard), plus two easter eggs shown for a few seconds.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/**
 * Start the display task. quiet_mutex is held by the sensor task while it
 * bit-bangs the DS18B20; the display waits for it before each I2C frame so
 * OLED traffic never overlaps a 1-Wire read. oled_init() must have
 * succeeded first.
 */
esp_err_t display_start(SemaphoreHandle_t quiet_mutex);

/** Latest control-loop values (call every control period). temp = NAN
 *  when the probe has no reading; duty > 0 heat demand, < 0 cool. */
void display_set_readings(float temp, float setpoint, float duty,
                          bool heat, bool cool);

/** Batch shown in the status strip. Empty name clears it. elapsed_s is
 *  the time since the batch started; the display keeps counting. */
void display_set_batch(const char *name, uint32_t elapsed_s);

/** "next", "main", "chart", "info", or "pet" (alternates cat / yeast). */
void display_command(const char *cmd);
