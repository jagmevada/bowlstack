// Real-time scope: four ToF traces, 0-500 mm, plus a frame-rate readout.
//
// The question it exists to answer is a capacity one -- can this board render
// four live sensor traces fast enough to be worth showing a person -- so the
// FPS figure is the output, not decoration. Everything else on the page is
// there to make that number honest.
//
// The data is fabricated for now. It is shaped like real VL53L0X output rather
// than like a sine wave: a baseline that steps when a bowl appears or goes,
// plus a few millimetres of ranging noise. A smooth synthetic signal would
// redraw a very different set of pixels from real data and would flatter the
// measurement.

#pragma once

#include <lvgl.h>
#include <stdint.h>

namespace ui {

// Full scale. The stack sits well inside this: PRESENT_BELOW_MM is 100 and
// ABSENT_ABOVE_MM is 400, so 500 shows both thresholds with headroom, and
// clipping there costs nothing real. A live sensor with no target reports
// ~8190 mm, which would otherwise flatten every trace against the floor.
static const int32_t SCOPE_MAX_MM = 500;

// Samples held across the plot. 115 over ~230 px is two pixels per sample --
// enough that a step is visibly a step, few enough that a redraw is not
// pushing 240 columns of chart through the bus every frame.
static const uint16_t SCOPE_POINTS = 115;

void buildScope(lv_obj_t *parent);

// SAMPLING and RENDERING are separate on purpose, and this split is what keeps
// the frame rate flat as pages are added.
//
// scopeSample() always runs: it writes into a plain ring buffer, touches no
// LVGL object, and invalidates nothing. Data therefore stays current whether or
// not anyone is looking, which was the requirement -- a scope that only
// collects while visible would show a gap on return, and in the real firmware
// the sensors do not stop ranging because someone swiped.
//
// scopeRender() only runs while the page is on screen. It is what costs, and it
// is what stops costing the moment the page is swiped away.
void scopeSample(uint32_t nowMs);
void scopeRender();

// Called on transition. Entering repopulates the whole chart from the ring in
// one pass, so the trace is continuous across the gap instead of scrolling in
// from the right edge as though the sensors had just been switched on.
void scopeSetVisible(bool visible);

// Last computed frames-per-second, for callers that want to log it.
uint16_t scopeFps();

// Pushes the shared perf figures into the on-screen readout. Once a second.
void scopeShowPerf();

}  // namespace ui
