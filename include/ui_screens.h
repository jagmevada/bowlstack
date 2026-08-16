// The screens themselves. Pure LVGL -- no board, driver or framework headers,
// so this compiles unchanged for the ESP32-S3 panel and for the desktop SDL
// simulator. See ui_state.h for why that restriction exists.

#pragma once

#include <lvgl.h>

#include "ui_state.h"

namespace ui {

// Panel geometry. Named rather than scattered as literals because the layout is
// built with flex containers sized against these, and a future 2.8" board is a
// change here rather than a hunt through the widget code.
static const int16_t SCREEN_W = 240;
static const int16_t SCREEN_H = 320;

// Builds the widget tree. Call once, after lv_init() and after a display has
// been registered.
//
// `parent` defaults to the active screen. It is a parameter so the same view
// can be dropped into a tileview page alongside the specimen pages -- which is
// what makes "the sim and the device show the same thing" a structural fact
// rather than two code paths that happen to agree today.
void build(lv_obj_t *parent = nullptr);

// Pushes new values into the existing widgets. Deliberately separate from
// build(): rebuilding the tree on every update would churn the LVGL heap and
// invalidate the whole screen each time, when the usual change is one digit.
void update(const State &s);

// --- image-retention pixel shift -------------------------------------------
// Moves the whole layout around a small ring so no pixel holds the same
// high-contrast value indefinitely. Televisions and station clocks do this, and
// this device has the same problem for the same reason: units are powered ~8 h
// a day showing a digit that changes a handful of times per service, and a
// ghost of an earlier build was observed surviving several power cycles.
//
// The excursion is 2 px. Small enough that nobody watching notices, large
// enough that a glyph edge -- the part that retains, because it is where the
// contrast is -- never sits over one pixel for long.
static const int8_t SHIFT_MAX_PX = 2;

// A full cycle takes 8 * periodMs. The default suits the shipping UI: slow
// enough to be imperceptible, fast enough that a service never ends with the
// layout parked. The gallery's test page drives it far faster on purpose, so a
// person can actually watch what it does.
static const uint32_t SHIFT_PERIOD_MS = 60000;

// Returns true on the ticks where the offset actually changed.
bool pixelShiftTick(uint32_t nowMs, uint32_t periodMs = SHIFT_PERIOD_MS);

// Force a specific offset, for the test page and for parking at 0,0.
void applyPixelShift(int8_t dx, int8_t dy);

// The offset currently applied.
void pixelShiftOffset(int8_t *dx, int8_t *dy);

}  // namespace ui
