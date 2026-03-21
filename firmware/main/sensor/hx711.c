/*
 * HX711 24-bit ADC bit-bang driver for ESP32
 *
 * The HX711 uses a simple synchronous serial protocol:
 *   - DOUT goes low when data is ready
 *   - Host clocks SCK 25 times (24 data + 1 for channel/gain selection)
 *   - Data is MSB first, 24-bit two's complement
 */

#include "hx711.h"

#include <math.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

static const char *TAG = "hx711";

static int32_t tare_offset = 0;
static float   scale_factor = 420.0f;   /* raw counts per gram — calibrate! */

/* ------------------------------------------------------------------ */
/*  Low-level helpers                                                 */
/* ------------------------------------------------------------------ */

esp_err_t hx711_init(void)
{
    gpio_config_t sck_cfg = {
        .pin_bit_mask = 1ULL << HX711_SCK_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&sck_cfg);

    gpio_config_t dout_cfg = {
        .pin_bit_mask = 1ULL << HX711_DOUT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&dout_cfg);

    /* Power on the HX711 — SCK low means "active" */
    gpio_set_level(HX711_SCK_GPIO, 0);

    /* Give the HX711 time to stabilise */
    vTaskDelay(pdMS_TO_TICKS(400));

    ESP_LOGI(TAG, "HX711 initialised (SCK=%d, DOUT=%d)",
             HX711_SCK_GPIO, HX711_DOUT_GPIO);
    return ESP_OK;
}

/**
 * Wait for DOUT to go low (data ready), with timeout.
 */
static esp_err_t hx711_wait_ready(uint32_t timeout_ms)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(HX711_DOUT_GPIO) != 0) {
        if ((esp_timer_get_time() - start) > (int64_t)timeout_ms * 1000) {
            ESP_LOGE(TAG, "HX711 not ready (timeout %lu ms)", (unsigned long)timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);  /* yield briefly */
    }
    return ESP_OK;
}

esp_err_t hx711_read_raw(int32_t *raw)
{
    esp_err_t err = hx711_wait_ready(1000);
    if (err != ESP_OK) return err;

    /*
     * Clock out 24 data bits + 1 extra pulse = 25 pulses total.
     * 25 pulses selects channel A, gain 128 for the NEXT conversion.
     *
     * Enter a critical section so timing is not disrupted by interrupts.
     */
    portDISABLE_INTERRUPTS();

    uint32_t value = 0;
    for (int i = 0; i < 24; i++) {
        gpio_set_level(HX711_SCK_GPIO, 1);
        ets_delay_us(1);
        value <<= 1;
        if (gpio_get_level(HX711_DOUT_GPIO)) {
            value |= 1;
        }
        gpio_set_level(HX711_SCK_GPIO, 0);
        ets_delay_us(1);
    }

    /* 25th pulse — sets gain 128, channel A for next read */
    gpio_set_level(HX711_SCK_GPIO, 1);
    ets_delay_us(1);
    gpio_set_level(HX711_SCK_GPIO, 0);
    ets_delay_us(1);

    portENABLE_INTERRUPTS();

    /* Convert 24-bit two's complement to signed 32-bit */
    if (value & 0x800000) {
        value |= 0xFF000000;  /* sign-extend */
    }
    *raw = (int32_t)value;

    return ESP_OK;
}

esp_err_t hx711_set_tare(void)
{
    int64_t sum = 0;
    int good = 0;

    for (int i = 0; i < 10; i++) {
        int32_t v;
        if (hx711_read_raw(&v) == ESP_OK) {
            sum += v;
            good++;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (good == 0) {
        ESP_LOGE(TAG, "Tare failed — no valid readings");
        return ESP_FAIL;
    }

    tare_offset = (int32_t)(sum / good);
    ESP_LOGI(TAG, "Tare set to %ld (from %d readings)", (long)tare_offset, good);
    return ESP_OK;
}

esp_err_t hx711_get_grams(float *grams)
{
    int32_t raw;
    esp_err_t err = hx711_read_raw(&raw);
    if (err != ESP_OK) return err;

    *grams = (float)(raw - tare_offset) / scale_factor;
    return ESP_OK;
}

void hx711_set_scale(float scale)
{
    if (fabsf(scale) > 0.001f) {
        scale_factor = scale;
        ESP_LOGI(TAG, "Scale factor set to %.2f", scale_factor);
    }
}
