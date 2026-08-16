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
void perfFrameStart(uint32_t nowMs);
void perfFrameEnd(uint32_t nowMs);

// True once per second, when a fresh window of figures is ready.
bool perfTick(uint32_t nowMs);

uint16_t perfFps();
uint8_t perfBusyPct();
uint16_t perfWorstMs();

// Writes "fps 14  busy 22%  worst 41ms  lvgl 38k/64k frag 12%" into buf.
void perfFormat(char *buf, uint32_t len);

}  // namespace ui
