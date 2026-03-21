/*
 * Wi-Fi Station mode driver with automatic reconnect
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifndef WIFI_SSID
#define WIFI_SSID "ZymoNet"
#endif

#ifndef WIFI_PASS
#define WIFI_PASS "changeme123"
#endif

/**
 * Initialise Wi-Fi in station mode, start connection.
 * Reads SSID/password from NVS ("wifi" namespace, keys "ssid"/"pass");
 * falls back to compile-time defaults above.
 */
esp_err_t wifi_sta_init(void);

/**
 * Returns true when an IP address has been obtained.
 */
bool wifi_sta_is_connected(void);
