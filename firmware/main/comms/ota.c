/*
 * Over-the-air firmware updates — see ota.h
 */

#include "comms/ota.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "comms/mqtt_client.h"

static const char *TAG = "ota";

#define OTA_URL_MAX  256

static volatile bool          busy = false;
static char                   pending_url[OTA_URL_MAX];
static ota_pre_restart_cb_t   pre_restart_cb = NULL;
static esp_timer_handle_t     verify_timer = NULL;

/* ------------------------------------------------------------------ */
/*  Status reporting (zymoscope/<mac>/status)                         */
/* ------------------------------------------------------------------ */

static void report(const char *state, int progress, const char *detail)
{
    char json[256];
    snprintf(json, sizeof(json),
             "{\"event\":\"ota\",\"state\":\"%s\",\"progress\":%d,"
             "\"detail\":\"%s\"}",
             state, progress, detail ? detail : "");
    mqtt_publish_status(json, false);
    ESP_LOGI(TAG, "%s (%d%%) %s", state, progress, detail ? detail : "");
}

/* ------------------------------------------------------------------ */
/*  Rollback watchdog                                                 */
/* ------------------------------------------------------------------ */

static void verify_timeout_cb(void *arg)
{
    (void)arg;
    ESP_LOGE(TAG, "New image not verified within %d s — rebooting to roll back",
             OTA_VERIFY_TIMEOUT_S);
    if (pre_restart_cb) pre_restart_cb();
    /* Rebooting while PENDING_VERIFY makes the bootloader mark this image
       ABORTED and boot the previous one. */
    esp_restart();
}

static esp_ota_img_states_t running_state(void)
{
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(esp_ota_get_running_partition(), &st);
    return st;
}

const char *ota_state_str(void)
{
    switch (running_state()) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    default:                         return "undefined";  /* e.g. USB-flashed */
    }
}

void ota_init(ota_pre_restart_cb_t pre_restart)
{
    pre_restart_cb = pre_restart;

    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_app_desc_t  *app = esp_app_get_description();
    ESP_LOGI(TAG, "Running %s from %s (state: %s)",
             app->version, run ? run->label : "?", ota_state_str());

    if (running_state() == ESP_OTA_IMG_PENDING_VERIFY) {
        const esp_timer_create_args_t args = {
            .callback = verify_timeout_cb,
            .name     = "ota_verify",
        };
        if (esp_timer_create(&args, &verify_timer) == ESP_OK) {
            esp_timer_start_once(verify_timer,
                                 (uint64_t)OTA_VERIFY_TIMEOUT_S * 1000000ULL);
            ESP_LOGW(TAG, "Image pending verification — must run %d s and "
                     "publish telemetry within %d s or it will roll back",
                     OTA_VERIFY_MIN_UPTIME_S, OTA_VERIFY_TIMEOUT_S);
        }
    }
}

bool ota_pending_verify(void)
{
    return running_state() == ESP_OTA_IMG_PENDING_VERIFY;
}

bool ota_mark_valid(void)
{
    if (running_state() != ESP_OTA_IMG_PENDING_VERIFY) return false;

    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "Image marked valid — rollback cancelled");
        if (verify_timer) {
            esp_timer_stop(verify_timer);
            esp_timer_delete(verify_timer);
            verify_timer = NULL;
        }
        return true;
    }
    ESP_LOGE(TAG, "Failed to mark image valid");
    return false;
}

bool ota_in_progress(void)
{
    return busy;
}

/* ------------------------------------------------------------------ */
/*  Update task                                                       */
/* ------------------------------------------------------------------ */

static void ota_task(void *arg)
{
    (void)arg;
    char detail[96];

    report("starting", 0, pending_url);

    esp_http_client_config_t http_cfg = {
        .url               = pending_url,
        .timeout_ms        = 15000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &h);
    if (err != ESP_OK) {
        snprintf(detail, sizeof(detail), "begin: %s", esp_err_to_name(err));
        report("failed", 0, detail);
        goto out;
    }

    /* Refuse anything that isn't a Zymoscope image (wrong .bin, etc.) */
    esp_app_desc_t new_app;
    err = esp_https_ota_get_img_desc(h, &new_app);
    if (err != ESP_OK) {
        snprintf(detail, sizeof(detail), "img_desc: %s", esp_err_to_name(err));
        report("failed", 0, detail);
        esp_https_ota_abort(h);
        goto out;
    }
    const esp_app_desc_t *cur = esp_app_get_description();
    if (strncmp(new_app.project_name, cur->project_name,
                sizeof(new_app.project_name)) != 0) {
        snprintf(detail, sizeof(detail), "wrong project '%.32s'",
                 new_app.project_name);
        report("failed", 0, detail);
        esp_https_ota_abort(h);
        goto out;
    }
    snprintf(detail, sizeof(detail), "%.32s -> %.32s", cur->version, new_app.version);
    report("downloading", 0, detail);

    int total = esp_https_ota_get_image_size(h);
    int last_pct = 0;
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (total > 0) {
            int pct = (int)(100LL * esp_https_ota_get_image_len_read(h) / total);
            if (pct >= last_pct + 10) {
                last_pct = pct - (pct % 10);
                report("downloading", last_pct, NULL);
            }
        }
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        snprintf(detail, sizeof(detail), "download: %s",
                 err != ESP_OK ? esp_err_to_name(err) : "incomplete");
        report("failed", last_pct, detail);
        esp_https_ota_abort(h);
        goto out;
    }

    /* Validates the image and sets it as the next boot partition */
    err = esp_https_ota_finish(h);
    if (err != ESP_OK) {
        snprintf(detail, sizeof(detail), "finish: %s", esp_err_to_name(err));
        report("failed", 100, detail);
        goto out;
    }

    report("rebooting", 100, new_app.version);
    vTaskDelay(pdMS_TO_TICKS(1000));   /* let the status message go out */
    if (pre_restart_cb) pre_restart_cb();
    esp_restart();

out:
    busy = false;
    vTaskDelete(NULL);
}

esp_err_t ota_start(const char *url)
{
    if (busy) {
        ESP_LOGW(TAG, "Update already in progress — ignoring");
        return ESP_ERR_INVALID_STATE;
    }
    if (!url || strlen(url) >= sizeof(pending_url)) {
        return ESP_ERR_INVALID_ARG;
    }
    busy = true;
    strcpy(pending_url, url);

    if (xTaskCreate(ota_task, "ota", 8192, NULL, 3, NULL) != pdPASS) {
        busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
