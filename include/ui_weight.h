// Weight view -- the primary screen.
//
// Replaces the bowl-count stock view on this branch. Same job, different
// measurement: one number big enough to read across the room, and the parts it
// was made of small enough not to compete with it.
//
// THE TYPE SCALE IS THE ONE IN lv_conf.h AND IT IS NOT NEGOTIABLE HERE.
//
//   48   the total. The one thing someone walked over to see.
//   24   the TARE button, the only control on the page.
//   16   the caption and the unit.
//   14   the rate line, which is diagnostics rather than information.
//
// 14 and 16 are the "read deliberately, up close" tier and 18 is the floor for
// anything glanceable; the per-cell figures sit at 20 because they are numbers
// rather than words, and a digit needs more pixels than a letter to stay
// unambiguous on a ~200 DPI panel.

#pragma once

#include <lvgl.h>

#include "ui_state.h"

namespace ui {

void buildWeight(lv_obj_t *parent = nullptr);

// Pushes new values into the existing widgets. Separate from buildWeight() for
// the same reason ui_screens splits them: rebuilding the tree per update would
// churn the LVGL heap and invalidate the whole screen, when the usual change is
// one digit.
//
// EVERY WRITE INSIDE IS GUARDED. LVGL's setters do not compare before acting --
// lv_label_set_text reallocates and marks the object dirty whether or not the
// string changed -- and this runs every frame against a value that moves.
void updateWeight(const State &s);

// The one button on the page. Tares BOTH cells.
//
// PER-CELL ZEROING IS NOT HERE ANY MORE. Zeroing one corner against the other
// is a setup job -- it is how you tell an uneven mounting from an uneven pair
// of cells -- and it is meaningless without the raw counts beside it, so it
// lives on Settings > Diagnose with the figures that give it meaning. What is
// left on the dashboard is the action somebody performs while holding a bowl.
//
// A function pointer for the same reason ui_state.h exists: src/ui/ is pure
// LVGL and cannot reach scale.cpp.
void weightOnTare(void (*cb)(void));

}  // namespace ui
