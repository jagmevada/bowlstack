// Frame-rate and cost instrumentation.
//
// Exists because "the UI feels slow" and "the UI is slow" are different claims,
// and only one of them can be optimised. Every number here is measured:
//
//   fps        completed LVGL refreshes per second, from LV_EVENT_REFR_READY.
//              NOT counted in the flush callback -- flush runs once per
//              invalidated AREA, so with partial rendering one frame calls it
//              several times and the figure would be a multiple of the truth.
//
//   busy %     share of wall-clock spent inside lv_timer_handler(). THIS is the
//              number that matters for this product. The ESP32 also has four
//              VL53L0X to poll, a WiFi link to hold up and Supabase to POST to;
//              a UI that renders at 30 fps while eating 80% of a core has not
//              earned its place. Low fps with low busy% is a UI that is idle
//              because nothing changed, which is the goal, not a problem.
//
//   worst ms   longest single lv_timer_handler() call in the window. An average
//              hides the 120 ms full-screen repaint that actually blocks the
//              sensor task.
//
//   lvgl mem   used / free / largest-free from lv_mem_monitor. Fragmentation is
//              what turns a working UI into an intermittently failing one after
//              an hour, and it is invisible without asking.

#pragma once

#include <stdint.h>

namespace ui {

// Call once, after a display exists.
void perfBegin();

// Bracket every lv_timer_handler() call.
// Called from the flush callback with the pixel count AND the wall time of each
// transfer.
//
// The pixel count alone was not enough. This board reported 10,000 px/frame and
// a 250 ms frame, and a derived "spi~4ms" figure computed from the pixel count
// at the bus clock -- which is a LOWER BOUND on the transfer and says nothing
// about how long the call actually took. Two very different faults produce that
// picture: a renderer that is slow, and a flush that blocks. Measuring the
// flush is what tells them apart.
//
// Separates the two halves of a frame's cost, which behave completely
// differently: pixels pushed is SPI time and scales with INVALIDATED AREA,
// while the remainder is software rendering and scales with how many draw
// operations cover that area. Optimising the wrong one is free effort wasted,
// and from the outside they are indistinguishable.
void perfFlush(uint32_t px, uint32_t us);

// Brackets the LVGL half specifically, inside the whole-iteration bracket.
// Two figures rather than one, because "the UI costs X" and "the loop costs X"
// are different claims and only the first answers "how much is the screen
// charging me". The difference between them is the sensors, the radio and the
// clock.
void perfUiStart(uint32_t nowMs);
void perfUiEnd(uint32_t nowMs);

// Wall time inside the touch read, accumulated. A touch controller that has
// stopped acknowledging costs ~13 ms per failed I2C transaction and LVGL polls
// it from inside lv_timer_handler, so it can dominate the UI figure while the
// renderer is doing almost nothing.
void perfTouch(uint32_t us);

void perfFrameStart(uint32_t nowMs);
void perfFrameEnd(uint32_t nowMs);

// True once per second, when a fresh window of figures is ready.
bool perfTick(uint32_t nowMs);

uint16_t perfFps();
uint8_t perfBusyPct();
uint8_t perfUiPct();
uint16_t perfWorstMs();

// Share of the window spent inside the flush callback and inside the touch
// read. Subtract both from perfUiPct() and what remains is LVGL rendering --
// which is the only way to know which of the three to go and fix.
uint8_t perfFlushPct();
uint8_t perfTouchPct();

// Pixels pushed per refresh, averaged over the window, and flushes per refresh.
uint32_t perfPxPerFrame();
uint16_t perfFlushesPerFrame();

// Writes "fps 14  busy 22%  worst 41ms  lvgl 38k/64k frag 12%" into buf.
void perfFormat(char *buf, uint32_t len);

// The same window, cut to what fits ON THE PANEL: "fps 12  ui 4%  worst 8ms".
//
// Two formats rather than one, because the two readers are different. The
// console line is read by someone diagnosing, with a scrollback and a terminal
// as wide as they like; the on-screen line has 184 px beside a back button, and
// the full string wrapped to three 14 px rows there -- which pushed the top of
// it out of a 30 px header, so the fps figure, the one number the line exists
// for, was the part that got clipped.
void perfFormatShort(char *buf, uint32_t len);

}  // namespace ui
