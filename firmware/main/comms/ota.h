/*
 * Over-the-air firmware updates
 *
 * Triggered by an MQTT command on zymoscope/<mac>/cmd:
 *     {"ota_url": "http://<host>:<port>/zymoscope.bin"}
 *
 * The image is downloaded into the inactive OTA slot (ota_0 / ota_1),
 * checked to be a "zymoscope" image, and booted. With bootloader rollback
 * enabled the new image starts in PENDING_VERIFY; it is marked valid only
 * after it has run for OTA_VERIFY_MIN_UPTIME_S and published telemetry
 * (sensors, control and MQTT all working — so it can receive the *next*
 * OTA). If it crashes, reboots, or hasn't verified within
 * OTA_VERIFY_TIMEOUT_S, the bootloader reverts to the previous image.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/** Seconds a freshly-updated image has to verify before it is rolled back. */
#define OTA_VERIFY_TIMEOUT_S  300

/** Minimum uptime before a new image may be marked valid. */
#define OTA_VERIFY_MIN_UPTIME_S  120

/** Called just before esp_restart() after a successful update. */
typedef void (*ota_pre_restart_cb_t)(void);

/**
 * Log the running image and, if it is pending verification, arm the
 * rollback timer. Call early in app_main.
 */
void ota_init(ota_pre_restart_cb_t pre_restart);

/**
 * Mark the running image as good (cancels rollback). Safe to call
 * repeatedly; only acts while the image is pending verification.
 * Returns true if this call changed the state to valid.
 */
bool ota_mark_valid(void);

/** True while the running image still awaits verification. */
bool ota_pending_verify(void);

/**
 * Start an update from an http(s) URL in a background task.
 * Returns ESP_ERR_INVALID_STATE if an update is already running.
 */
esp_err_t ota_start(const char *url);

/** True while an update is downloading / being written. */
bool ota_in_progress(void);

/** Download progress 0..100 while an update runs, else -1. */
int ota_progress(void);

/** Short label for the running image's OTA state ("valid", "pending_verify", ...). */
const char *ota_state_str(void);
