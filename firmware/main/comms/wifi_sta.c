/*
 * Wi-Fi Station mode with automatic reconnect
 *
 * Reads SSID/password from NVS namespace "wifi" (keys "ssid", "pass").
 * Falls back to compile-time WIFI_SSID / WIFI_PASS defaults.
 */

#include "wifi_sta.h"

#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi_sta";

#define WIFI_CONNECTED_BIT  BIT0

static EventGroupHandle_t s_wifi_events;

/* ------------------------------------------------------------------ */
/*  Event handler                                                     */
/* ------------------------------------------------------------------ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "Connecting...");
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected — retrying in 5 s");
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
            vTaskDelay(pdMS_TO_TICKS(5000));
            esp_wifi_connect();
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

esp_err_t wifi_sta_init(void)
{
    s_wifi_events = xEventGroupCreate();

    /* NVS is required by Wi-Fi driver */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Network interface + default event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /* Wi-Fi init */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    /* Determine SSID and password */
    char ssid[33] = {0};
    char pass[65] = {0};

    /* Try NVS first */
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(ssid);
        if (nvs_get_str(nvs, "ssid", ssid, &len) != ESP_OK) ssid[0] = 0;
        len = sizeof(pass);
        if (nvs_get_str(nvs, "pass", pass, &len) != ESP_OK) pass[0] = 0;
        nvs_close(nvs);
    }

    /* Fall back to compile-time defaults */
    if (ssid[0] == 0) {
        strncpy(ssid, WIFI_SSID, sizeof(ssid) - 1);
        strncpy(pass, WIFI_PASS, sizeof(pass) - 1);
        ESP_LOGI(TAG, "Using compile-time SSID: %s", ssid);
    } else {
        ESP_LOGI(TAG, "Using NVS SSID: %s", ssid);
    }

    /* Configure and start */
    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    memcpy(wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    memcpy(wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi STA initialised");
    return ESP_OK;
}

bool wifi_sta_is_connected(void)
{
    if (!s_wifi_events) return false;
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}
