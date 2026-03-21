/*
 * Zymoscope v2 — Main application
 *
 * Wires together all subsystems:
 *   - DS18B20 temperature sensors (1-Wire, GPIO 4)
 *   - HX711 load cell ADC (GPIO 18/19)
 *   - BME280 ambient temp/humidity/pressure (I2C 0x76)
 *   - SSD1306 OLED display (I2C 0x3C)
 *   - PID temperature controller driving relays (GPIO 25/26)
 *   - Wi-Fi STA with MQTT telemetry
 *
 * Three FreeRTOS tasks:
 *   sensor_task     — reads all sensors every 5 s, updates OLED
 *   control_task    — runs PID every 5 s, drives heat/cool relays
 *   telemetry_task  — publishes MQTT JSON every 30 s
 */

#include <math.h>
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "sensor/ds18b20.h"
#include "sensor/hx711.h"
#include "sensor/bme280_i2c.h"
#include "display/oled.h"
#include "control/pid.h"
#include "comms/wifi_sta.h"
#include "comms/mqtt_client.h"

static const char *TAG = "zymoscope";

/* ------------------------------------------------------------------ */
/*  Relay GPIOs                                                       */
/* ------------------------------------------------------------------ */
#define RELAY_HEAT_GPIO  25
#define RELAY_COOL_GPIO  26

/* ------------------------------------------------------------------ */
/*  Shared state (protected by mutex)                                 */
/* ------------------------------------------------------------------ */
static SemaphoreHandle_t state_mutex;

static struct {
    float temps[DS18B20_MAX_SENSORS];
    int   temp_count;
    float bme_temp;
    float bme_humidity;
    float bme_pressure;
    float weight_g;
    float gravity_est;
    int   relay_heat;
    int   relay_cool;
} shared;

/* ------------------------------------------------------------------ */
/*  MQTT broker URI (compile-time default)                            */
/* ------------------------------------------------------------------ */
#ifndef MQTT_BROKER_URI
#define MQTT_BROKER_URI "mqtt://192.168.1.100:1883"
#endif

/* ------------------------------------------------------------------ */
/*  PID defaults                                                      */
/* ------------------------------------------------------------------ */
#define PID_KP  2.0f
#define PID_KI  0.1f
#define PID_KD  0.5f
#define PID_DEFAULT_SETPOINT  20.0f   /* degrees C */

/* ------------------------------------------------------------------ */
/*  Crude specific-gravity estimator from weight loss                 */
/*  Uses the idea that CO2 mass loss tracks sugar consumption.        */
/*  OG is assumed 1.050; calibrate for your wort.                     */
/* ------------------------------------------------------------------ */
static float initial_weight = 0.0f;
#define ASSUMED_OG  1.050f

static float estimate_gravity(float current_weight)
{
    if (initial_weight <= 0.0f) return ASSUMED_OG;
    float loss = initial_weight - current_weight;
    if (loss < 0.0f) loss = 0.0f;
    /* Rough: 1 g CO2 lost ≈ 0.001 SG drop per litre.
       Adjust divisor for your batch volume. */
    float sg = ASSUMED_OG - (loss / 1000.0f) * 0.001f;
    if (sg < 0.990f) sg = 0.990f;
    return sg;
}

/* ------------------------------------------------------------------ */
/*  Relay helpers                                                     */
/* ------------------------------------------------------------------ */

static void relay_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << RELAY_HEAT_GPIO) | (1ULL << RELAY_COOL_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(RELAY_HEAT_GPIO, 0);
    gpio_set_level(RELAY_COOL_GPIO, 0);
}

/* ------------------------------------------------------------------ */
/*  sensor_task — read all sensors every 5 s                          */
/* ------------------------------------------------------------------ */

static void sensor_task(void *arg)
{
    (void)arg;
    bool first_weight = true;

    for (;;) {
        float temps[DS18B20_MAX_SENSORS];
        int   temp_count = 0;
        float bme_t = 0, bme_h = 0, bme_p = 0;
        float weight = 0;

        /* DS18B20 */
        if (ds18b20_read_temps(temps, &temp_count) == ESP_OK) {
            for (int i = 0; i < temp_count; i++) {
                ESP_LOGI(TAG, "DS18B20[%d]: %.2f C", i, temps[i]);
            }
        }

        /* BME280 */
        if (bme280_read(&bme_t, &bme_h, &bme_p) == ESP_OK) {
            ESP_LOGI(TAG, "BME280: %.1f C, %.1f%% RH, %.1f hPa",
                     bme_t, bme_h, bme_p);
        }

        /* HX711 */
        if (hx711_get_grams(&weight) == ESP_OK) {
            ESP_LOGI(TAG, "Weight: %.1f g", weight);
            if (first_weight && weight > 100.0f) {
                initial_weight = weight;
                first_weight = false;
                ESP_LOGI(TAG, "Initial weight captured: %.1f g", initial_weight);
            }
        }

        float sg = estimate_gravity(weight);

        /* Update shared state */
        if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100))) {
            memcpy(shared.temps, temps, sizeof(temps));
            shared.temp_count   = temp_count;
            shared.bme_temp     = bme_t;
            shared.bme_humidity = bme_h;
            shared.bme_pressure = bme_p;
            shared.weight_g     = weight;
            shared.gravity_est  = sg;
            xSemaphoreGive(state_mutex);
        }

        /* Update OLED */
        float display_temp = (temp_count > 0) ? temps[0] : bme_t;
        oled_show_status(display_temp, sg,
                         shared.relay_heat, shared.relay_cool,
                         wifi_sta_is_connected());

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ------------------------------------------------------------------ */
/*  control_task — PID temperature control every 5 s                  */
/* ------------------------------------------------------------------ */

static void control_task(void *arg)
{
    (void)arg;

    pid_ctrl_t pid;
    pid_init(&pid, PID_KP, PID_KI, PID_KD, PID_DEFAULT_SETPOINT);

    for (;;) {
        /* Check for setpoint commands from MQTT */
        float new_sp = mqtt_get_cmd_setpoint();
        if (!isnan(new_sp)) {
            pid_set_setpoint(&pid, new_sp);
            ESP_LOGI(TAG, "PID setpoint changed to %.1f C", new_sp);
        }

        /* Get current fermentation temperature */
        float temp = 0;
        if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100))) {
            if (shared.temp_count > 0) {
                temp = shared.temps[0];
            } else {
                temp = shared.bme_temp;
            }
            xSemaphoreGive(state_mutex);
        }

        /* Run PID */
        float output = pid_update(&pid, temp);

        /* Drive relays:
         *   output > 0.1  → heat ON,  cool OFF
         *   output < -0.1 → heat OFF, cool ON
         *   otherwise     → both OFF (deadband)
         */
        int heat = 0, cool = 0;
        if (output > 0.1f) {
            heat = 1;
        } else if (output < -0.1f) {
            cool = 1;
        }

        gpio_set_level(RELAY_HEAT_GPIO, heat);
        gpio_set_level(RELAY_COOL_GPIO, cool);

        if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100))) {
            shared.relay_heat = heat;
            shared.relay_cool = cool;
            xSemaphoreGive(state_mutex);
        }

        ESP_LOGD(TAG, "PID: temp=%.1f sp=%.1f out=%.2f heat=%d cool=%d",
                 temp, pid.setpoint, output, heat, cool);

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ------------------------------------------------------------------ */
/*  telemetry_task — MQTT publish every 30 s                          */
/* ------------------------------------------------------------------ */

static void telemetry_task(void *arg)
{
    (void)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));

        if (!mqtt_is_connected()) {
            ESP_LOGW(TAG, "MQTT not connected, skipping telemetry");
            continue;
        }

        telemetry_t telem = {0};

        if (xSemaphoreTake(state_mutex, pdMS_TO_TICKS(100))) {
            memcpy(telem.temp_c, shared.temps, sizeof(shared.temps));
            telem.temp_count    = shared.temp_count;
            telem.humidity      = shared.bme_humidity;
            telem.pressure_hpa  = shared.bme_pressure;
            telem.weight_g      = shared.weight_g;
            telem.gravity_est   = shared.gravity_est;
            telem.relay1        = shared.relay_heat;
            telem.relay2        = shared.relay_cool;
            telem.wifi_connected = wifi_sta_is_connected();
            xSemaphoreGive(state_mutex);
        }

        esp_err_t err = mqtt_publish_telemetry(&telem);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Telemetry published");
        } else {
            ESP_LOGW(TAG, "Telemetry publish failed: %s", esp_err_to_name(err));
        }
    }
}

/* ------------------------------------------------------------------ */
/*  app_main                                                          */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "Zymoscope v2 starting...");

    /* Print device ID (MAC-based) */
    {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        ESP_LOGI(TAG, "Device MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    /* Create shared-state mutex */
    state_mutex = xSemaphoreCreateMutex();
    configASSERT(state_mutex);
    memset(&shared, 0, sizeof(shared));
    shared.gravity_est = ASSUMED_OG;

    /* ---- Init subsystems ---- */

    /* Wi-Fi (also initialises NVS) */
    ESP_ERROR_CHECK(wifi_sta_init());

    /* I2C sensors — BME280 installs the I2C driver */
    if (bme280_init() != ESP_OK) {
        ESP_LOGW(TAG, "BME280 init failed — continuing without ambient sensor");
    }

    /* OLED (uses I2C bus set up by bme280_init) */
    if (oled_init() != ESP_OK) {
        ESP_LOGW(TAG, "OLED init failed — continuing without display");
    }

    /* DS18B20 temperature probes */
    if (ds18b20_init() != ESP_OK) {
        ESP_LOGW(TAG, "DS18B20 init failed — continuing without probes");
    }

    /* HX711 load cell */
    if (hx711_init() != ESP_OK) {
        ESP_LOGW(TAG, "HX711 init failed — continuing without scale");
    } else {
        ESP_LOGI(TAG, "Taring scale...");
        hx711_set_tare();
    }

    /* Relays */
    relay_init();

    /* MQTT (wait briefly for Wi-Fi) */
    ESP_LOGI(TAG, "Waiting for Wi-Fi before starting MQTT...");
    for (int i = 0; i < 20 && !wifi_sta_is_connected(); i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (wifi_sta_is_connected()) {
        ESP_ERROR_CHECK(mqtt_client_init(MQTT_BROKER_URI));
    } else {
        ESP_LOGW(TAG, "Wi-Fi not connected — MQTT deferred");
        /* Could retry in telemetry_task; for now we start MQTT anyway
           and it will connect once Wi-Fi comes up. */
        mqtt_client_init(MQTT_BROKER_URI);
    }

    /* Show initial screen */
    oled_show_status(0, ASSUMED_OG, 0, 0, wifi_sta_is_connected());

    /* ---- Launch tasks ---- */
    xTaskCreate(sensor_task,    "sensor",    4096, NULL, 5, NULL);
    xTaskCreate(control_task,   "control",   4096, NULL, 5, NULL);
    xTaskCreate(telemetry_task, "telemetry", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "All tasks started. Zymoscope v2 running.");
}
