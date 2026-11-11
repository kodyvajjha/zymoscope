/*
 * MQTT client for Zymoscope telemetry
 *
 * Publishes JSON telemetry to  zymoscope/{device_id}/telemetry
 * Publishes status/events to   zymoscope/{device_id}/status
 *   (retained "online" on connect, retained "offline" as last will)
 * Subscribes to                zymoscope/{device_id}/cmd
 *   {"setpoint": <float>}  — change PID target
 *   {"ota_url": "<url>"}   — download + boot new firmware (see ota.h)
 *
 * Uses the ESP-MQTT component shipped with ESP-IDF v5.x.
 */

#include "comms/mqtt_client.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <mqtt_client.h>       /* ESP-IDF mqtt component header */
#include "freertos/FreeRTOS.h" /* portMUX_TYPE / critical sections */
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"

#include "comms/ota.h"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t client = NULL;
static bool connected = false;

/* Pending setpoint command (NAN = none). Written by the MQTT task,
   consumed by control_task via mqtt_get_cmd_setpoint(). */
static float cmd_setpoint = NAN;
static portMUX_TYPE cmd_mux = portMUX_INITIALIZER_UNLOCKED;

/* Reject obviously bogus setpoints (typos, unit mix-ups) */
#define SETPOINT_MIN_C  0.0f
#define SETPOINT_MAX_C  60.0f

/* Topic buffers (filled at init with device_id) */
static char topic_telemetry[64];
static char topic_cmd[64];
static char topic_status[64];

static const char LWT_OFFLINE[] = "{\"event\":\"offline\"}";

/* Copy the JSON string value of "key" from src into out. Returns true if
   found. Minimal: no escape handling (URLs/versions don't need it). */
static bool json_get_string(const char *src, const char *key,
                            char *out, size_t out_len)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(src, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p = strchr(p, '"');
    if (!p) return false;
    p++;
    const char *end = strchr(p, '"');
    if (!end || (size_t)(end - p) >= out_len) return false;
    memcpy(out, p, end - p);
    out[end - p] = '\0';
    return true;
}

void mqtt_publish_online(void)
{
    const esp_app_desc_t  *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    char json[200];
    snprintf(json, sizeof(json),
             "{\"event\":\"online\",\"fw\":\"%s\",\"partition\":\"%s\","
             "\"ota_state\":\"%s\",\"idf\":\"%s\"}",
             app->version, run ? run->label : "?", ota_state_str(),
             app->idf_ver);
    mqtt_publish_status(json, true);
}

/* ------------------------------------------------------------------ */
/*  MQTT event handler                                                */
/* ------------------------------------------------------------------ */

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t evt = (esp_mqtt_event_handle_t)event_data;

    switch (evt->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to broker");
        connected = true;
        esp_mqtt_client_subscribe(client, topic_cmd, 1);
        ESP_LOGI(TAG, "Subscribed to %s", topic_cmd);
        /* (A freshly-updated image is marked valid later, from
           telemetry_task, once it has proven itself — see ota.h.) */
        mqtt_publish_online();
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from broker");
        connected = false;
        break;

    case MQTT_EVENT_DATA:
        /* Incoming command — {"setpoint": <float>} and/or {"ota_url": "..."} */
        if (evt->topic_len > 0 && evt->data_len > 0) {
            /* evt->data is NOT NUL-terminated; copy before using str*() */
            /* static: only ever runs on the MQTT task; keeps its stack small */
            static char buf[512];
            int n = evt->data_len;
            if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
            memcpy(buf, evt->data, n);
            buf[n] = '\0';

            static char url[256];
            if (json_get_string(buf, "ota_url", url, sizeof(url))) {
                ESP_LOGI(TAG, "OTA command: %s", url);
                esp_err_t e = ota_start(url);
                if (e != ESP_OK) {
                    char json[128];
                    snprintf(json, sizeof(json),
                             "{\"event\":\"ota\",\"state\":\"rejected\","
                             "\"detail\":\"%s\"}", esp_err_to_name(e));
                    mqtt_publish_status(json, false);
                }
            }

            /* Simple parser — no JSON lib dependency */
            const char *sp = strstr(buf, "\"setpoint\"");
            if (sp) {
                sp = strchr(sp, ':');
                if (sp) {
                    float val;
                    if (sscanf(sp + 1, "%f", &val) == 1 &&
                        val >= SETPOINT_MIN_C && val <= SETPOINT_MAX_C) {
                        portENTER_CRITICAL(&cmd_mux);
                        cmd_setpoint = val;
                        portEXIT_CRITICAL(&cmd_mux);
                        ESP_LOGI(TAG, "Setpoint command: %.1f C", val);
                    } else {
                        ESP_LOGW(TAG, "Ignoring invalid setpoint command: %s", buf);
                    }
                }
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error type=%d", evt->error_handle->error_type);
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Build the device_id from the MAC address                          */
/* ------------------------------------------------------------------ */

static void get_device_id(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, len, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

esp_err_t mqtt_client_init(const char *broker_uri)
{
    char device_id[16];
    get_device_id(device_id, sizeof(device_id));

    snprintf(topic_telemetry, sizeof(topic_telemetry),
             "zymoscope/%s/telemetry", device_id);
    snprintf(topic_cmd, sizeof(topic_cmd),
             "zymoscope/%s/cmd", device_id);
    snprintf(topic_status, sizeof(topic_status),
             "zymoscope/%s/status", device_id);

    ESP_LOGI(TAG, "Device ID: %s", device_id);
    ESP_LOGI(TAG, "Telemetry topic: %s", topic_telemetry);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
        .session.last_will = {
            .topic  = topic_status,
            .msg    = LWT_OFFLINE,
            .qos    = 1,
            .retain = 1,
        },
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client");
        return err;
    }

    return ESP_OK;
}

esp_err_t mqtt_publish_telemetry(const telemetry_t *data)
{
    if (!connected || !client) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Build JSON payload */
    char json[512];
    int pos = 0;

    pos += snprintf(json + pos, sizeof(json) - pos, "{\"temp_c\":[");
    for (int i = 0; i < data->temp_count && i < DS18B20_MAX_SENSORS; i++) {
        if (i > 0) pos += snprintf(json + pos, sizeof(json) - pos, ",");
        pos += snprintf(json + pos, sizeof(json) - pos, "%.2f", data->temp_c[i]);
    }
    pos += snprintf(json + pos, sizeof(json) - pos, "],");

    pos += snprintf(json + pos, sizeof(json) - pos,
                    "\"humidity\":%.1f,"
                    "\"pressure_hpa\":%.1f,"
                    "\"weight_g\":%.1f,"
                    "\"gravity_est\":%.4f,"
                    "\"relay1\":%d,"
                    "\"relay2\":%d,"
                    "\"wifi\":%s,"
                    "\"fw\":\"%s\"}",
                    data->humidity,
                    data->pressure_hpa,
                    data->weight_g,
                    data->gravity_est,
                    data->relay1,
                    data->relay2,
                    data->wifi_connected ? "true" : "false",
                    esp_app_get_description()->version);

    int msg_id = esp_mqtt_client_publish(client, topic_telemetry,
                                         json, 0, 1, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Publish failed");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Published (%d bytes) msg_id=%d", pos, msg_id);
    return ESP_OK;
}

bool mqtt_is_connected(void)
{
    return connected;
}

float mqtt_get_cmd_setpoint(void)
{
    /* Consume-once: hand the pending value over and clear it, so the
       caller only sees each command a single time. */
    portENTER_CRITICAL(&cmd_mux);
    float val = cmd_setpoint;
    cmd_setpoint = NAN;
    portEXIT_CRITICAL(&cmd_mux);
    return val;
}

esp_err_t mqtt_publish_status(const char *json, bool retain)
{
    if (!connected || !client) {
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(client, topic_status, json, 0, 1,
                                         retain ? 1 : 0);
    return msg_id < 0 ? ESP_FAIL : ESP_OK;
}
