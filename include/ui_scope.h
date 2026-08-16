// Real-time scope: four ToF traces, 0-500 mm, plus a frame-rate readout.
//
// A SWEEPING CURSOR, NOT A SCROLLING CHART, and that is the whole design.
//
// The first version used lv_chart in SHIFT mode. Measured on hardware it cost
// 49,000 pixels and ~56 ms per frame -- 19 ms of SPI and ~37 ms of software
// rendering -- for 79% of a core at 14 fps. The reason is structural rather
// than a tuning problem: scrolling moves every sample left, so every pixel of
// the plot changes on every new reading, and lv_chart_set_next_value()
// invalidates the whole object. Narrower lines and fewer points were tried and
// changed nothing measurable, because the cost is AREA, not segments.
//
// A hardware oscilloscope does not scroll. It sweeps a cursor across a
// persistent trace, overwriting the oldest column with the newest. Only that
// column changes, so the invalidated area per sample drops from the whole plot
// to a single column -- around 200 pixels instead of 49,000.
//
// The trade is that the trace is no longer strictly left-to-right in time: the
// newest data sits at the cursor and the oldest just ahead of it. Anyone who
// has used a scope reads that immediately, and a blanked gap ahead of the
// cursor makes the wrap position obvious.
//
// THE BUFFER COMES FROM THE PLATFORM. A 232x190 RGB565 canvas is ~88 KB, which
// belongs in the ESP32-S3's 8 MB of PSRAM rather than in LVGL's 64 KB pool or
// the internal SRAM that WiFi and TLS will want. The desktop preview passes
// plain heap. That is a genuine platform difference in the CLAUDE.md sense --
// one machine has PSRAM and the other does not -- not a divergence in fixtures.

#pragma once

#include <lvgl.h>
#include <stdint.h>

namespace ui {

// Full scale. The stack sits well inside this: PRESENT_BELOW_MM is 100 and
// ABSENT_ABOVE_MM is 400, so 500 shows both thresholds with headroom. A live
// sensor with no target reports ~8190 mm, which would otherwise flatten every
// trace against the floor.
static const int32_t SCOPE_MAX_MM = 500;

// Plot size in pixels. One sample per column, so this is also the history
// depth: 232 columns at 10 Hz is about 23 seconds of trace on screen.
static const uint16_t SCOPE_W = 232;
static const uint16_t SCOPE_H = 190;

// What the caller must allocate. RGB565, two bytes per pixel.
static const uint32_t SCOPE_BUF_BYTES = (uint32_t)SCOPE_W * SCOPE_H * 2;

// Hand the canvas its backing store before buildScope(). Passing nullptr leaves
// the scope disabled rather than crashing -- a failed allocation must not take
// the rest of the UI down with it.
void scopeSetBuffer(void *buf);

void buildScope(lv_obj_t *parent);

// SAMPLING and RENDERING are separate, which is what keeps the frame rate flat
// as pages are added. scopeSample() always runs: it writes to a ring buffer,
// touches no LVGL object and invalidates nothing, so data stays current whether
// or not anyone is looking. In the shipping firmware the sensors do not stop
// ranging because someone swiped.
void scopeSample(uint32_t nowMs);
void scopeRender();

// Entering repaints the whole trace from the ring in one pass, so the history
// is there rather than sweeping in from nothing.
void scopeSetVisible(bool visible);

uint16_t scopeFps();
void scopeShowPerf();

}  // namespace ui
