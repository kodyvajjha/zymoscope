/*
 * MQTT client for Zymoscope telemetry
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include "sensor/ds18b20.h"

typedef struct {
    float temp_c[DS18B20_MAX_SENSORS];
    int   temp_count;
    float humidity;
    float pressure_hpa;
    float weight_g;
    float gravity_est;
    int   relay1;
    int   relay2;
    bool  wifi_connected;
} telemetry_t;

/**
 * Start the MQTT client and connect to the broker.
 */
esp_err_t mqtt_client_init(const char *broker_uri);

/**
 * Publish a telemetry JSON payload.
 */
esp_err_t mqtt_publish_telemetry(const telemetry_t *data);

/**
 * Returns true when the MQTT client is connected.
 */
bool mqtt_is_connected(void);

/**
 * Returns the latest setpoint received from the command topic.
 * Returns NAN if none has been received.
 */
float mqtt_get_cmd_setpoint(void);
