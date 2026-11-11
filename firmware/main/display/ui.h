/*
 * OLED screen layouts. Pure rendering from a snapshot of state into a
 * framebuffer — no I/O — so it can be previewed on a PC.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define UI_HIST_LEN     120     /* chart history points */
#define UI_HIST_STEP_S  120     /* seconds per point → 4 h */

typedef enum {
    UI_PAGE_MAIN = 0,
    UI_PAGE_CHART,
    UI_PAGE_INFO,
    UI_PAGE_COUNT,           /* pages "next" cycles through */
    UI_PAGE_CAT = UI_PAGE_COUNT,
    UI_PAGE_BLOB,            /* easter eggs, shown for a few seconds */
} ui_page_t;

typedef struct {
    ui_page_t page;
    uint32_t  frame;           /* animation counter (pet pages) */

    float temp;                /* probe, NAN = no reading */
    float setpoint;
    float duty;                /* controller demand: >0 heat, <0 cool */
    bool  heat, cool;          /* relay states right now */

    bool  wifi, mqtt;
    int   rssi;                /* dBm, 0 = unknown */
    int   ota_pct;             /* -1 = no update running */

    bool     have_batch;
    char     batch[24];
    uint32_t batch_elapsed_s;

    char     ip[16];
    char     fw[32];
    char     partition[17];
    uint32_t uptime_s;

    /* oldest → newest; NAN = no data. heat_frac: 0..1 heater on-time */
    const float *hist;
    const float *hist_heat;
    int          hist_n;
} ui_model_t;

void ui_render(const ui_model_t *m, uint8_t *fb);

/* "2d 04:12" / "04:12" (h:mm) */
void ui_fmt_elapsed(uint32_t s, char *out, int len);
