/*
 * Monochrome 128x64 framebuffer drawing (SSD1306 page layout).
 *
 * Pure C with no ESP-IDF dependencies, so the screen layouts can be
 * compiled and previewed on a PC (see firmware/tools/oled_preview.py).
 *
 * Framebuffer layout: pixel (x, y) is bit (y & 7) of fb[(y >> 3) * 128 + x].
 * Colours: 1 = lit, 0 = dark, 2 = invert.
 */
#pragma once

#include <stdint.h>

#define GFX_W        128
#define GFX_H        64
#define GFX_FB_SIZE  (GFX_W * GFX_H / 8)

/* Everything drawn is offset by this (burn-in shift) and clipped. */
void gfx_set_origin(int dx, int dy);

void gfx_clear(uint8_t *fb);
void gfx_pixel(uint8_t *fb, int x, int y, int color);
void gfx_hline(uint8_t *fb, int x, int y, int w, int color);
void gfx_vline(uint8_t *fb, int x, int y, int h, int color);
void gfx_hline_dashed(uint8_t *fb, int x, int y, int w, int on, int off);
void gfx_rect(uint8_t *fb, int x, int y, int w, int h, int color);
void gfx_fill(uint8_t *fb, int x, int y, int w, int h, int color);
void gfx_line(uint8_t *fb, int x0, int y0, int x1, int y1, int color);
/* Filled ellipse centred on (cx, cy) with radii rx, ry. */
void gfx_fill_ellipse(uint8_t *fb, int cx, int cy, int rx, int ry, int color);
void gfx_circle(uint8_t *fb, int cx, int cy, int r, int color);

/* 8x8 font (printable ASCII). Returns the x after the text. */
int  gfx_text(uint8_t *fb, int x, int y, const char *s, int color);
int  gfx_text_width(const char *s);

/* 3x5 font: digits, A-Z (lowercase is shown as uppercase), . : - + % / < > !
 * and '*' drawn as a degree sign. 4 px advance. */
int  gfx_tiny(uint8_t *fb, int x, int y, const char *s, int color);
int  gfx_tiny_width(const char *s);

/* Seven-segment style digits for '0'-'9', '-', '.', ' '.
 * Digit cell is w x h with stroke t; returns the x after the text. */
int  gfx_big(uint8_t *fb, int x, int y, const char *s, int w, int h, int t);
int  gfx_big_width(const char *s, int w, int t);

/* ASCII-art sprite: rows of '#' (lit) / anything else (skip), scaled. */
void gfx_sprite(uint8_t *fb, int x, int y, const char *const *rows,
                int nrows, int scale, int color);
