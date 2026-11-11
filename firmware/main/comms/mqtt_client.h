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
 * Publish a JSON event to zymoscope/<id>/status (OTA progress, online, ...).
 */
esp_err_t mqtt_publish_status(const char *json, bool retain);

/**
 * Publish the retained {"event":"online", fw, partition, ota_state} status.
 * Sent on every connect, and again when an OTA image is marked valid.
 */
void mqtt_publish_online(void);

/**
 * Returns a newly received setpoint from the command topic, then clears it
 * (each command is returned exactly once). Returns NAN if no new command
 * has arrived since the last call.
 */
float mqtt_get_cmd_setpoint(void);
