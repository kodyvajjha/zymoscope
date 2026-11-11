#!/usr/bin/env python3
"""Render the OLED screens to PNG files on a PC, without the hardware.

Compiles main/display/gfx.c + ui.c with the host C compiler, renders each
page with made-up readings and writes enlarged PNGs:

    python tools/oled_preview.py [out_dir]       # default: build/oled_preview

Needs only gcc (or cc) and the Python standard library.
"""

from __future__ import annotations

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

FW = Path(__file__).resolve().parent.parent
DISPLAY = FW / "main" / "display"
SCALE = 4

HARNESS = r"""
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "gfx.h"
#include "ui.h"

static float hist[UI_HIST_LEN], heat[UI_HIST_LEN];
static uint8_t fb[GFX_FB_SIZE];

static void emit(const char *name, const ui_model_t *m)
{
    ui_render(m, fb);
    printf("%s ", name);
    for (int i = 0; i < GFX_FB_SIZE; i++) printf("%02x", fb[i]);
    printf("\n");
}

int main(void)
{
    /* A warm-up from 24 C that overshoots a little and settles at 35 */
    for (int i = 0; i < UI_HIST_LEN; i++) {
        float t = i * UI_HIST_STEP_S / 60.0f;              /* minutes */
        hist[i] = 35.0f - 11.0f * expf(-t / 40.0f) + 0.9f * expf(-t / 60.0f) * sinf(t / 25.0f)
                  + 0.08f * sinf(t / 3.5f);
        heat[i] = t < 60 ? 1.0f : 0.55f + 0.3f * sinf(t / 7.0f);
    }
    ui_model_t m = {
        .page = UI_PAGE_MAIN, .temp = 34.62f, .setpoint = 35.0f, .duty = 0.62f,
        .heat = true, .wifi = true, .mqtt = true, .rssi = -61, .ota_pct = -1,
        .have_batch = true, .batch = "Greek yogurt", .batch_elapsed_s = 187000,
        .ip = "10.0.0.193", .fw = "563d38f-dirty", .partition = "ota_1",
        .uptime_s = 5400, .hist = hist, .hist_heat = heat, .hist_n = UI_HIST_LEN,
    };
    emit("main", &m);
    m.page = UI_PAGE_CHART;              emit("chart", &m);
    m.page = UI_PAGE_INFO;               emit("info", &m);

    ui_model_t p = m;                    /* just booted, no batch, relay off */
    p.page = UI_PAGE_MAIN; p.have_batch = false; p.heat = false; p.duty = 0.18f;
    p.hist_n = 20; p.hist = hist + UI_HIST_LEN - 20; p.hist_heat = heat + UI_HIST_LEN - 20;
    p.temp = 60.0f; p.setpoint = 60.0f; p.rssi = -78; p.mqtt = false;
    emit("main_no_batch", &p);

    ui_model_t f = m;                    /* probe fault, Wi-Fi down */
    f.page = UI_PAGE_MAIN; f.temp = NAN; f.duty = 0; f.heat = false; f.wifi = false;
    emit("main_probe_fault", &f);

    ui_model_t o = m; o.page = UI_PAGE_MAIN; o.ota_pct = 45;    emit("ota", &o);

    for (int i = 0; i < 4; i++) {
        ui_model_t c = m; c.page = UI_PAGE_CAT; c.frame = (uint32_t)(i * 7 + 1);
        char n[16]; snprintf(n, sizeof n, "cat_%d", i); emit(n, &c);
    }
    ui_model_t cb = m; cb.page = UI_PAGE_CAT; cb.frame = 27; emit("cat_blink", &cb);
    for (int i = 0; i < 4; i++) {
        ui_model_t b = m; b.page = UI_PAGE_BLOB; b.frame = (uint32_t)(i * 6 + 2);
        char n[16]; snprintf(n, sizeof n, "blob_%d", i); emit(n, &b);
    }
    ui_model_t bb = m; bb.page = UI_PAGE_BLOB; bb.frame = 29; emit("blob_blink", &bb);
    return 0;
}
"""


def png(path: Path, fb: bytes) -> None:
    w, h = 128 * SCALE, 64 * SCALE
    on, off, grid = (150, 220, 255), (8, 10, 14), (0, 0, 0)
    rows = []
    for y in range(h):
        py = y // SCALE
        row = bytearray([0])
        for x in range(w):
            px = x // SCALE
            if x % SCALE == SCALE - 1 or y % SCALE == SCALE - 1:
                row += bytes(grid)            # pixel gap, like the real panel
            elif fb[(py >> 3) * 128 + px] >> (py & 7) & 1:
                row += bytes(on)
            else:
                row += bytes(off)
        rows.append(bytes(row))
    raw = zlib.compress(b"".join(rows), 9)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(
            ">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", raw) + chunk(b"IEND", b""))


def main() -> int:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else FW / "build" / "oled_preview"
    out.mkdir(parents=True, exist_ok=True)
    cc = os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc")
    if not cc:
        print("No C compiler found (set CC)", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "harness.c"
        exe = Path(tmp) / "harness"
        src.write_text(HARNESS)
        subprocess.run([cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                        "-Wno-format-truncation", "-I", str(DISPLAY), str(src),
                        str(DISPLAY / "gfx.c"), str(DISPLAY / "ui.c"), "-lm",
                        "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    for line in result.stdout.splitlines():
        name, hexfb = line.split()
        png(out / f"{name}.png", bytes.fromhex(hexfb))
        print(out / f"{name}.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
