/*
 * OLED screen manager — see display.h.
 */

#include "display.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "display/gfx.h"
#include "display/oled.h"
#include "display/ui.h"
#include "comms/mqtt_client.h"
#include "comms/ota.h"
#include "comms/wifi_sta.h"

static const char *TAG = "display";

#define REFRESH_MS        1000    /* normal pages */
#define ANIM_MS           150     /* easter eggs */
#define PET_SHOW_S        12
#define SHIFT_PERIOD_S    300     /* burn-in: move the picture every 5 min */

static SemaphoreHandle_t lock;          /* guards everything below */
static SemaphoreHandle_t quiet;         /* 1-Wire read in progress */

static struct {
    float temp, setpoint, duty;
    bool  heat, cool, have_readings;

    bool     have_batch;
    char     batch[24];
    uint32_t batch_base_s;
    int64_t  batch_set_us;

    ui_page_t page, page_before_pet;
    int64_t   pet_until_us;
    bool      next_pet_is_blob;
    bool      dirty;                    /* redraw now, page changed */
} st;

/* Chart history: a ring of UI_HIST_LEN points, each the mean of the
   readings in one UI_HIST_STEP_S bucket. */
static float  ring_temp[UI_HIST_LEN], ring_heat[UI_HIST_LEN];
static int    ring_head, ring_count;     /* head = next slot to write */
static float  acc_temp, acc_heat;
static int    acc_n_temp, acc_n;
static int64_t bucket_start_us;

static TaskHandle_t task;

/* ------------------------------------------------------------------ */

static void push_bucket(void)
{
    ring_temp[ring_head] = acc_n_temp ? acc_temp / acc_n_temp : NAN;
    ring_heat[ring_head] = acc_n ? acc_heat / acc_n : NAN;
    ring_head = (ring_head + 1) % UI_HIST_LEN;
    if (ring_count < UI_HIST_LEN) ring_count++;
    acc_temp = acc_heat = 0;
    acc_n_temp = acc_n = 0;
}

void display_set_readings(float temp, float setpoint, float duty,
                          bool heat, bool cool)
{
    if (!lock) return;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(lock, portMAX_DELAY);
    st.temp = temp;
    st.setpoint = setpoint;
    st.duty = duty;
    st.heat = heat;
    st.cool = cool;
    st.have_readings = true;

    if (bucket_start_us == 0) bucket_start_us = now;
    /* close finished buckets; a long stall leaves NAN gaps */
    while (now - bucket_start_us >= (int64_t)UI_HIST_STEP_S * 1000000) {
        push_bucket();
        bucket_start_us += (int64_t)UI_HIST_STEP_S * 1000000;
    }
    if (!isnan(temp)) {
        acc_temp += temp;
        acc_n_temp++;
    }
    acc_heat += heat ? 1.0f : 0.0f;
    acc_n++;
    xSemaphoreGive(lock);
}

void display_set_batch(const char *name, uint32_t elapsed_s)
{
    if (!lock) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    /* Keep printable ASCII only: the fonts have nothing else. */
    int n = 0;
    for (const char *p = name; *p && n < (int)sizeof(st.batch) - 1; p++) {
        if (*p >= 0x20 && *p <= 0x7E) st.batch[n++] = *p;
    }
    st.batch[n] = '\0';
    st.have_batch = n > 0;
    st.batch_base_s = elapsed_s;
    st.batch_set_us = esp_timer_get_time();
    st.dirty = true;
    xSemaphoreGive(lock);
    if (task) xTaskNotifyGive(task);
}

void display_command(const char *cmd)
{
    if (!lock) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool in_pet = st.page >= UI_PAGE_COUNT;
    ui_page_t base = in_pet ? st.page_before_pet : st.page;
    if (strcmp(cmd, "next") == 0) {
        st.page = (ui_page_t)((base + 1) % UI_PAGE_COUNT);
    } else if (strcmp(cmd, "main") == 0) {
        st.page = UI_PAGE_MAIN;
    } else if (strcmp(cmd, "chart") == 0) {
        st.page = UI_PAGE_CHART;
    } else if (strcmp(cmd, "info") == 0) {
        st.page = UI_PAGE_INFO;
    } else if (strcmp(cmd, "pet") == 0) {
        st.page_before_pet = base;
        st.page = st.next_pet_is_blob ? UI_PAGE_BLOB : UI_PAGE_CAT;
        st.next_pet_is_blob = !st.next_pet_is_blob;
        st.pet_until_us = esp_timer_get_time() + (int64_t)PET_SHOW_S * 1000000;
    } else {
        ESP_LOGW(TAG, "Unknown display command: %s", cmd);
    }
    st.dirty = true;
    xSemaphoreGive(lock);
    if (task) xTaskNotifyGive(task);
}

/* ------------------------------------------------------------------ */

static void read_link(ui_model_t *m)
{
    m->wifi = wifi_sta_is_connected();
    m->mqtt = mqtt_is_connected();
    m->rssi = 0;
    m->ip[0] = '\0';
    if (!m->wifi) return;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) m->rssi = ap.rssi;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        snprintf(m->ip, sizeof(m->ip), IPSTR, IP2STR(&ip.ip));
    }
}

static void display_task(void *arg)
{
    (void)arg;
    static uint8_t fb[GFX_FB_SIZE];
    static float hist[UI_HIST_LEN], hist_heat[UI_HIST_LEN];
    static ui_model_t m;
    static const int8_t orbit[][2] = { {0, 0}, {1, 0}, {1, 1}, {0, 1} };

    const esp_partition_t *run = esp_ota_get_running_partition();
    snprintf(m.fw, sizeof(m.fw), "%s", esp_app_get_description()->version);
    snprintf(m.partition, sizeof(m.partition), "%.16s", run ? run->label : "?");

    uint32_t frame = 0;
    bool was_pet = false;

    for (;;) {
        int64_t now = esp_timer_get_time();

        xSemaphoreTake(lock, portMAX_DELAY);
        if (st.page >= UI_PAGE_COUNT && now >= st.pet_until_us) {
            st.page = st.page_before_pet;
        }
        m.page = st.page;
        m.temp = st.have_readings ? st.temp : NAN;
        m.setpoint = st.have_readings ? st.setpoint : NAN;
        m.duty = st.duty;
        m.heat = st.heat;
        m.cool = st.cool;
        m.have_batch = st.have_batch;
        memcpy(m.batch, st.batch, sizeof(m.batch));
        m.batch_elapsed_s = st.batch_base_s +
                            (uint32_t)((now - st.batch_set_us) / 1000000);
        /* ring → oldest-first array */
        int n = ring_count;
        for (int i = 0; i < n; i++) {
            int idx = (ring_head - n + i + UI_HIST_LEN) % UI_HIST_LEN;
            hist[i] = ring_temp[idx];
            hist_heat[i] = ring_heat[idx];
        }
        st.dirty = false;
        xSemaphoreGive(lock);

        bool pet = m.page >= UI_PAGE_COUNT;
        if (was_pet && !pet) {
            ESP_LOGI(TAG, "stack high-water mark: %u bytes",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        was_pet = pet;

        m.hist = hist;
        m.hist_heat = hist_heat;
        m.hist_n = n;
        m.frame = frame++;
        m.uptime_s = (uint32_t)(now / 1000000);
        m.ota_pct = ota_progress();
        read_link(&m);

        int s = (int)((now / 1000000 / SHIFT_PERIOD_S) % 4);
        if (pet) gfx_set_origin(0, 0);
        else     gfx_set_origin(orbit[s][0], orbit[s][1]);
        ui_render(&m, fb);

        /* Never send I2C traffic while the sensor task bit-bangs 1-Wire */
        if (xSemaphoreTake(quiet, pdMS_TO_TICKS(2000)) == pdTRUE) {
            esp_err_t err = oled_flush(fb);
            xSemaphoreGive(quiet);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "flush failed: %s", esp_err_to_name(err));
            }
        }

        /* Sleep until the next frame, or wake early on a command */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(pet || m.ota_pct >= 0 ? ANIM_MS : REFRESH_MS));
    }
}

esp_err_t display_start(SemaphoreHandle_t quiet_mutex)
{
    quiet = quiet_mutex;
    lock = xSemaphoreCreateMutex();
    if (!lock) return ESP_ERR_NO_MEM;
    st.page = UI_PAGE_MAIN;
    if (xTaskCreate(display_task, "display", 4096, NULL, 3, &task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
