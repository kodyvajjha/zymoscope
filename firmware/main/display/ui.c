/*
 * OLED screen layouts — see ui.h.
 *
 * 128x64, designed so the top 16 rows work as a status strip (on two-tone
 * panels that band is yellow).
 */

#include "ui.h"
#include "gfx.h"

#include <math.h>
#include <stdio.h>

void ui_fmt_elapsed(uint32_t s, char *out, int len)
{
    unsigned d = s / 86400, h = (s / 3600) % 24, m = (s / 60) % 60;
    if (d > 0) snprintf(out, len, "%ud %02u:%02u", d, h, m);
    else       snprintf(out, len, "%02u:%02u", h, m);
}

/* ------------------------------------------------------------------ */
/*  Shared bits                                                       */
/* ------------------------------------------------------------------ */

/* Signal bars at the top-right corner, or an X when Wi-Fi is down.
   A "!" in front means Wi-Fi is up but the MQTT broker isn't. */
static void draw_link(uint8_t *fb, const ui_model_t *m)
{
    const int x0 = 116;
    if (!m->wifi) {
        gfx_line(fb, x0 + 4, 1, x0 + 10, 7, 1);
        gfx_line(fb, x0 + 10, 1, x0 + 4, 7, 1);
        return;
    }
    int bars = 4;
    if (m->rssi != 0) {
        bars = m->rssi > -55 ? 4 : m->rssi > -65 ? 3 : m->rssi > -75 ? 2 : 1;
    }
    for (int i = 0; i < 4; i++) {
        int h = 2 + 2 * i;
        if (i < bars) gfx_fill(fb, x0 + 3 * i, 8 - h, 2, h, 1);
        else          gfx_hline(fb, x0 + 3 * i, 7, 2, 1);
    }
    if (!m->mqtt) gfx_tiny(fb, x0 - 5, 2, "!", 1);
}

static void fmt_temp(char *out, int len, float t)
{
    if (isnan(t)) snprintf(out, len, "--.-");
    else          snprintf(out, len, "%.1f", t);
}

/* Fit the 4 h history and the target into [lo, hi], rounded out to
   0.5 °C, at least 1 °C tall. Returns false if there is no data. */
static bool hist_range(const ui_model_t *m, float *lo, float *hi)
{
    float mn = INFINITY, mx = -INFINITY;
    for (int i = 0; i < m->hist_n; i++) {
        float v = m->hist[i];
        if (isnan(v)) continue;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    if (mn > mx) return false;
    if (!isnan(m->setpoint)) {
        if (m->setpoint < mn) mn = m->setpoint;
        if (m->setpoint > mx) mx = m->setpoint;
    }
    mn = floorf(mn * 2.0f) / 2.0f;
    mx = ceilf(mx * 2.0f) / 2.0f;
    if (mx - mn < 1.0f) {
        float c = (mn + mx) / 2.0f;
        mn = floorf((c - 0.5f) * 2.0f) / 2.0f;
        mx = mn + 1.0f;
    }
    *lo = mn;
    *hi = mx;
    return true;
}

/* Temperature line for history points [0, n) mapped onto x0..x1 and
   y0..y1 (y0 top). Newest point is at x1; with a partial history the
   line starts further right. Dashed line at the target. */
static void plot_history(uint8_t *fb, const ui_model_t *m, int x0, int x1,
                         int y0, int y1, float lo, float hi)
{
    int span = UI_HIST_LEN - 1;
    float k = (float)(y1 - y0) / (hi - lo);
    if (!isnan(m->setpoint)) {
        int ys = y1 - (int)lroundf((m->setpoint - lo) * k);
        gfx_hline_dashed(fb, x0, ys, x1 - x0 + 1, 2, 2);
    }
    int px = -1, py = -1;
    for (int i = 0; i < m->hist_n; i++) {
        int age = m->hist_n - 1 - i;              /* 0 = newest */
        int x = x1 - (age * (x1 - x0) + span / 2) / span;
        float v = m->hist[i];
        if (isnan(v)) { px = -1; continue; }
        int y = y1 - (int)lroundf((v - lo) * k);
        if (px >= 0) gfx_line(fb, px, py, x, y, 1);
        else         gfx_pixel(fb, x, y, 1);
        px = x;
        py = y;
    }
}

/* ------------------------------------------------------------------ */
/*  Main page: big temperature, target, heater, 4 h trend             */
/* ------------------------------------------------------------------ */

static void page_main(uint8_t *fb, const ui_model_t *m)
{
    char buf[32];

    /* Status strip: batch name (or product name), elapsed time, link */
    snprintf(buf, sizeof(buf), "%.13s", m->have_batch ? m->batch : "Zymoscope");
    gfx_text(fb, 0, 0, buf, 1);
    draw_link(fb, m);
    if (m->have_batch) {
        char el[20];
        ui_fmt_elapsed(m->batch_elapsed_s, el, sizeof(el));
        snprintf(buf, sizeof(buf), "RUNNING %s", el);
        gfx_tiny(fb, 0, 10, buf, 1);
    } else {
        gfx_tiny(fb, 0, 10, "NO BATCH", 1);
    }

    /* Big temperature, left */
    if (isnan(m->temp)) {
        gfx_text(fb, 0, 21, "NO PROBE", 1);
        gfx_tiny(fb, 0, 33, "HEATER OFF", 1);
    } else {
        fmt_temp(buf, sizeof(buf), m->temp);
        int x = gfx_big(fb, 0, 17, buf, 13, 27, 3);
        gfx_tiny(fb, x + 2, 17, "*C", 1);
    }

    /* Target + heater, right column */
    const int rx = 80;
    gfx_tiny(fb, rx, 17, "TARGET", 1);
    fmt_temp(buf, sizeof(buf), m->setpoint);
    gfx_text(fb, rx, 24, buf, 1);

    float d = m->duty;
    const char *what = d > 0.0f ? "HEAT" : d < 0.0f ? "COOL" : "IDLE";
    bool on = m->heat || m->cool;
    if (on) {
        /* relay energised right now: label in a lit box */
        gfx_fill(fb, rx - 2, 33, gfx_tiny_width(what) + 4, 9, 1);
        gfx_tiny(fb, rx, 35, what, 0);
    } else {
        gfx_tiny(fb, rx, 35, what, 1);
    }
    if (d != 0.0f) {
        snprintf(buf, sizeof(buf), "%d%%", (int)lroundf(fabsf(d) * 100.0f));
        gfx_tiny(fb, 127 - 1 - gfx_tiny_width(buf), 35, buf, 1);
    }
    gfx_rect(fb, rx, 42, 46, 4, 1);
    int fillw = (int)lroundf(fminf(fabsf(d), 1.0f) * 44.0f);
    gfx_hline(fb, rx + 1, 43, fillw, 1);
    gfx_hline(fb, rx + 1, 44, fillw, 1);

    /* 4 h trend along the bottom */
    float lo, hi;
    if (hist_range(m, &lo, &hi)) {
        plot_history(fb, m, 10, 126, 49, 62, lo, hi);
        gfx_tiny(fb, 0, 58, "4H", 1);
    }
}

/* ------------------------------------------------------------------ */
/*  Chart page: 4 h of temperature with axes, heater strip            */
/* ------------------------------------------------------------------ */

static void page_chart(uint8_t *fb, const ui_model_t *m)
{
    char buf[40];
    gfx_text(fb, 0, 0, "Last 4 h", 1);
    draw_link(fb, m);

    float mn = INFINITY, mx = -INFINITY, heat = 0;
    int n = 0, nh = 0;
    for (int i = 0; i < m->hist_n; i++) {
        float v = m->hist[i];
        if (!isnan(v)) {
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            n++;
        }
        if (m->hist_heat && !isnan(m->hist_heat[i])) {
            heat += m->hist_heat[i];
            nh++;
        }
    }
    float lo, hi;
    if (!n || !hist_range(m, &lo, &hi)) {
        gfx_tiny(fb, 0, 30, "COLLECTING DATA...", 1);
        return;
    }
    if (nh) {
        snprintf(buf, sizeof(buf), "LO %.1f HI %.1f HEAT %d%%", mn, mx,
                 (int)lroundf(100.0f * heat / nh));
    } else {
        snprintf(buf, sizeof(buf), "LO %.1f  HI %.1f", mn, mx);
    }
    gfx_tiny(fb, 0, 10, buf, 1);

    const int x0 = 18, x1 = 127, y0 = 18, y1 = 56;
    snprintf(buf, sizeof(buf), "%.1f", hi);
    gfx_tiny(fb, 0, y0, buf, 1);
    snprintf(buf, sizeof(buf), "%.1f", lo);
    gfx_tiny(fb, 0, y1 - 4, buf, 1);
    gfx_vline(fb, x0 - 2, y0, y1 - y0 + 1, 1);
    /* hour ticks, counted back from now */
    int span = UI_HIST_LEN - 1;
    for (int h = 0; h <= 4; h++) {
        int pts = h * 3600 / UI_HIST_STEP_S;
        if (pts > span) break;
        int x = x1 - (pts * (x1 - x0) + span / 2) / span;
        for (int y = y0; y <= y1; y += 3) gfx_pixel(fb, x, y, 1);
    }
    plot_history(fb, m, x0, x1, y0, y1, lo, hi);

    /* Heater on-time per point: bar height 0..3 px under the plot */
    if (m->hist_heat) {
        for (int i = 0; i < m->hist_n; i++) {
            float f = m->hist_heat[i];
            if (isnan(f) || f <= 0.0f) continue;
            int age = m->hist_n - 1 - i;
            int x = x1 - (age * (x1 - x0) + span / 2) / span;
            int h = 1 + (int)(f * 3.0f);
            if (h > 4) h = 4;
            gfx_vline(fb, x, 63 - h + 1, h, 1);
        }
    }
    gfx_tiny(fb, 0, 59, "HEAT", 1);
}

/* ------------------------------------------------------------------ */
/*  Info page                                                         */
/* ------------------------------------------------------------------ */

static void info_row(uint8_t *fb, int y, const char *label, const char *val)
{
    gfx_tiny(fb, 0, y + 2, label, 1);
    char v[16];
    snprintf(v, sizeof(v), "%.12s", val);
    gfx_text(fb, 30, y, v, 1);
}

static void page_info(uint8_t *fb, const ui_model_t *m)
{
    char buf[32];
    info_row(fb, 0, "IP", m->wifi && m->ip[0] ? m->ip : "--");
    if (m->wifi && m->rssi != 0) snprintf(buf, sizeof(buf), "%d dBm", m->rssi);
    else snprintf(buf, sizeof(buf), "%s", m->wifi ? "on" : "off");
    info_row(fb, 9, "WIFI", buf);
    info_row(fb, 18, "MQTT", m->mqtt ? "connected" : "down");
    info_row(fb, 27, "FW", m->fw);
    info_row(fb, 36, "SLOT", m->partition);
    ui_fmt_elapsed(m->uptime_s, buf, sizeof(buf));
    info_row(fb, 45, "UP", buf);
    if (m->have_batch) {
        ui_fmt_elapsed(m->batch_elapsed_s, buf, sizeof(buf));
        info_row(fb, 54, "BATCH", buf);
    } else {
        info_row(fb, 54, "BATCH", "none");
    }
}

/* ------------------------------------------------------------------ */
/*  Easter eggs                                                       */
/* ------------------------------------------------------------------ */

static const char *const cat_head[] = {
    "....#..........#",
    "....##........##",
    "....#.#......#.#",
    "....#..######..#",
    "...#............#",
    "...#............#",
    "...#..##....##..#",
    "##.#..##....##..#.##",
    "...#.....##.....#",
    "##.#....#..#....#.##",
    "....#..........#",
    ".....##......##",
    "....#..######..#",
    "...#............#",
    "...#............#",
    "...#............#",
    "...#............#",
    "...#............#",
    "...#..##....##..#",
    "....##..####..##",
};
#define CAT_ROWS ((int)(sizeof(cat_head) / sizeof(cat_head[0])))

/* Rows 6-7 with the eyes shut */
static const char *const cat_blink[] = {
    "...#............#",
    "##.#..##....##..#.##",
};

static const char *const cat_tail_a[] = {   /* drawn from row 13 */
    "...##",
    "....#",
    "....#",
    "...#",
    "..#",
    "###",
};
static const char *const cat_tail_b[] = {   /* drawn from row 12 */
    ".....#",
    "....#",
    "...#",
    "...#",
    "...#",
    "..#",
    "##",
};

static const char *const heart[] = {
    ".#.#.",
    "#####",
    "#####",
    ".###.",
    "..#..",
};

/* Small up-down bob: 0,1,2,1,0,... */
static int bob(uint32_t f, int period)
{
    int p = (int)(f % (uint32_t)period);
    int half = period / 2;
    return p <= half ? p * 2 / half : (period - p) * 2 / half;
}

static void page_cat(uint8_t *fb, const ui_model_t *m)
{
    const int s = 2, x = 30;
    int y = 22 - 2 * s + bob(m->frame, 8);
    /* clear the eye rows when blinking */
    bool blink = (m->frame % 28) >= 26;
    for (int r = 0; r < CAT_ROWS; r++) {
        const char *row = cat_head[r];
        if (blink && (r == 6 || r == 7)) row = cat_blink[r - 6];
        gfx_sprite(fb, x, y + r * s, &row, 1, s, 1);
    }
    if ((m->frame / 4) % 2 == 0) {
        gfx_sprite(fb, x + 17 * s, y + 13 * s, cat_tail_a, 6, s, 1);
    } else {
        gfx_sprite(fb, x + 17 * s, y + 12 * s, cat_tail_b, 7, s, 1);
    }
    /* Hearts drifting up on the right */
    for (int i = 0; i < 3; i++) {
        int t = (int)((m->frame + i * 9) % 27);
        int hx = 92 + (i * 11) % 24 + (t / 6) % 2;
        int hy = 50 - t * 2;
        if (hy > -5) gfx_sprite(fb, hx, hy, heart, 5, 1, 1);
    }
    gfx_tiny(fb, 0, 0, (m->frame / 10) % 2 ? "PURR" : "MEOW", 1);
    if (!isnan(m->temp)) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f*C", m->temp);
        gfx_tiny(fb, 0, 59, buf, 1);
    }
}

/* Zymo, a budding yeast cell. */
static void page_blob(uint8_t *fb, const ui_model_t *m)
{
    uint32_t f = m->frame;
    int hop = bob(f, 10);                    /* 0..2 */
    int squash = hop == 0 ? 2 : 0;           /* flatten on landing */
    int cx = 58, cy = 36 - 3 * hop;
    int rx = 22 + squash, ry = 18 - squash;

    /* Bubbles rising around Zymo */
    static const int bx[] = { 6, 18, 100, 112, 122 };
    static const int bspeed[] = { 2, 3, 2, 3, 1 };
    for (int i = 0; i < 5; i++) {
        int y = 66 - (int)((f * bspeed[i] + i * 17) % 72);
        int r = 1 + (i % 3);
        gfx_circle(fb, bx[i] + ((f / 3 + i) % 2), y, r, 1);
    }

    /* Bud (upper right) and body: outlines merge where they overlap */
    int bdx = cx + 19, bdy = cy - 14;
    gfx_fill_ellipse(fb, bdx, bdy, 7, 6, 1);
    gfx_fill_ellipse(fb, cx, cy, rx, ry, 1);
    gfx_fill_ellipse(fb, bdx, bdy, 5, 4, 0);
    gfx_fill_ellipse(fb, cx, cy, rx - 2, ry - 2, 0);
    /* Feet */
    gfx_fill_ellipse(fb, cx - 9, cy + ry + 1, 4, 2, 1);
    gfx_fill_ellipse(fb, cx + 9, cy + ry + 1, 4, 2, 1);

    /* Eyes: look left / ahead / right, blink now and then */
    int look = (int)((f / 12) % 4);
    int pdx = look == 1 ? -1 : look == 3 ? 1 : 0;
    bool blink = (f % 30) >= 28;
    for (int side = -1; side <= 1; side += 2) {
        int ex = cx + side * 8, ey = cy - 4;
        if (blink) {
            gfx_hline(fb, ex - 2, ey + 1, 5, 1);
        } else {
            gfx_fill_ellipse(fb, ex + pdx, ey, 3, 4, 1);
            gfx_fill(fb, ex + pdx - 2, ey - 3, 2, 2, 0); /* shine */
        }
    }
    /* Smile, and a little "o" when it lands */
    if (squash) {
        gfx_circle(fb, cx, cy + 7, 2, 1);
    } else {
        gfx_line(fb, cx - 5, cy + 5, cx - 3, cy + 7, 1);
        gfx_hline(fb, cx - 2, cy + 8, 5, 1);
        gfx_line(fb, cx + 3, cy + 7, cx + 5, cy + 5, 1);
    }
    /* Freckles (it's a cell, they're organelles) */
    gfx_pixel(fb, cx - 13, cy + 3, 1);
    gfx_pixel(fb, cx - 11, cy + 4, 1);
    gfx_pixel(fb, cx + 12, cy + 3, 1);
    gfx_pixel(fb, cx + 14, cy + 4, 1);

    gfx_tiny(fb, 0, 0, (f / 8) % 3 == 0 ? "BLUB" : "ZYMO", 1);
    if (!isnan(m->temp)) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f*C", m->temp);
        gfx_tiny(fb, 0, 59, buf, 1);
    }
}

/* ------------------------------------------------------------------ */
/*  Firmware update overlay                                           */
/* ------------------------------------------------------------------ */

static void page_ota(uint8_t *fb, const ui_model_t *m)
{
    char buf[16];
    gfx_text(fb, 0, 0, "Updating", 1);
    draw_link(fb, m);
    gfx_tiny(fb, 0, 18, "NEW FIRMWARE OVER WI-FI", 1);
    int pct = m->ota_pct < 0 ? 0 : m->ota_pct > 100 ? 100 : m->ota_pct;
    gfx_rect(fb, 0, 28, 128, 10, 1);
    gfx_fill(fb, 2, 30, pct * 124 / 100, 6, 1);
    snprintf(buf, sizeof(buf), "%d%%", pct);
    gfx_tiny(fb, 0, 42, buf, 1);
    gfx_tiny(fb, 0, 56, "DO NOT UNPLUG", 1);
}

/* ------------------------------------------------------------------ */

void ui_render(const ui_model_t *m, uint8_t *fb)
{
    gfx_clear(fb);
    if (m->ota_pct >= 0) {
        page_ota(fb, m);
        return;
    }
    switch (m->page) {
    case UI_PAGE_CHART: page_chart(fb, m); break;
    case UI_PAGE_INFO:  page_info(fb, m);  break;
    case UI_PAGE_CAT:   page_cat(fb, m);   break;
    case UI_PAGE_BLOB:  page_blob(fb, m);  break;
    case UI_PAGE_MAIN:
    default:            page_main(fb, m);  break;
    }
}
