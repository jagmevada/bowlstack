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

}  // namespace ui
