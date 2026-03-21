/*
 * MQTT client for FermentaBot telemetry
 *
 * Publishes JSON telemetry to  fermentabot/{device_id}/telemetry
 * Subscribes to                fermentabot/{device_id}/cmd
 *
 * Uses the ESP-MQTT component shipped with ESP-IDF v5.x.
 */

#include "comms/mqtt_client.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mqtt_client.h"       /* ESP-IDF mqtt component header */
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t client = NULL;
static bool connected = false;
static float cmd_setpoint = NAN;

/* Topic buffers (filled at init with device_id) */
static char topic_telemetry[64];
static char topic_cmd[64];

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
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected from broker");
        connected = false;
        break;

    case MQTT_EVENT_DATA:
        /* Incoming command — look for {"setpoint": <float>} */
        if (evt->topic_len > 0 && evt->data_len > 0) {
            /* Simple parser — no JSON lib dependency */
            const char *sp = strstr(evt->data, "\"setpoint\"");
            if (sp) {
                sp = strchr(sp, ':');
                if (sp) {
                    float val;
                    if (sscanf(sp + 1, "%f", &val) == 1) {
                        cmd_setpoint = val;
                        ESP_LOGI(TAG, "Setpoint command: %.1f C", val);
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
             "fermentabot/%s/telemetry", device_id);
    snprintf(topic_cmd, sizeof(topic_cmd),
             "fermentabot/%s/cmd", device_id);

    ESP_LOGI(TAG, "Device ID: %s", device_id);
    ESP_LOGI(TAG, "Telemetry topic: %s", topic_telemetry);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
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

    pos += snprintf(json + pos, sizeof(json) - pos, "{\"temps\":[");
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
                    "\"wifi\":%s}",
                    data->humidity,
                    data->pressure_hpa,
                    data->weight_g,
                    data->gravity_est,
                    data->relay1,
                    data->relay2,
                    data->wifi_connected ? "true" : "false");

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
    return cmd_setpoint;
}
