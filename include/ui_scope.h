// Real-time scope: both load cells, plus a frame-rate readout.
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
// THE Y AXIS AUTO-RANGES, which the ToF version did not need and this one
// cannot do without. A ToF trace has a fixed physical scale -- 0 to 500 mm --
// and a load cell has none: the same page has to show raw converter counts on
// an uncalibrated unit, where a hand on the platform is a few hundred thousand,
// and grams on a calibrated one, where it is a few hundred. A fixed scale would
// be wrong for at least one of them and probably both.
//
// Auto-ranging costs the optimisation above whenever the range MOVES, because a
// rescale repaints every column. That is why the range is refitted only when a
// sample falls outside it, or when the trace has settled into a small part of
// the window -- not per frame. In the steady state it never fires and the sweep
// is as cheap as it was.
//
// THE BUFFER COMES FROM THE PLATFORM. A 232x176 RGB565 canvas is ~82 KB, which
// belongs in the ESP32-S3's 8 MB of PSRAM rather than in LVGL's 96 KB pool or
// the internal SRAM that WiFi and TLS will want. The desktop preview passes
// plain heap. That is a genuine platform difference in the CLAUDE.md sense --
// one machine has PSRAM and the other does not -- not a divergence in fixtures.

#pragma once

#include <lvgl.h>
#include <stdint.h>

#include "ui_state.h"

namespace ui {

// Plot size in pixels. One sample per column, so this is also the history
// depth: 232 columns at 20 Hz is about 12 seconds of trace on screen.
static const uint16_t SCOPE_W = 232;
// 176 rather than the ToF version's 190. The values line under the plot now
// carries the auto-range window as well as the readings, which is a second row
// of 14 px text, and the page has to hold the plot, that line and the legend
// inside 294 px below the status bar. Fourteen pixels of plot is the cheapest
// thing on this page to give up.
static const uint16_t SCOPE_H = 176;

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
// or not anyone is looking. The cells do not stop converting because someone
// swiped.
//
// Supplies one value per cell with `valid` false for a cell that is not
// producing. `unit` labels the axis -- "cts" or "g" -- and is stored by
// pointer, so it must be a literal or otherwise outlive the call.
//
// Once called, the fabricated generator stops for good. Without this the scope
// drew healthy traces with NO CELLS ATTACHED, on the one page someone would
// swipe to in order to judge whether a cell is working at all.
void scopeFeed(const int32_t value[CELLS], const bool valid[CELLS], const char *unit);

void scopeSample(uint32_t nowMs);
void scopeRender();

// Entering repaints the whole trace from the ring in one pass, so the history
// is there rather than sweeping in from nothing.
void scopeSetVisible(bool visible);

void scopeOnClose(void (*cb)(void));

uint16_t scopeFps();
void scopeShowPerf();

}  // namespace ui
