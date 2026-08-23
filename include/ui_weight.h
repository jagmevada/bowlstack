// Weight view -- the primary screen.
//
// Replaces the bowl-count stock view on this branch. Same job, different
// measurement: one number big enough to read across the room, and the parts it
// was made of small enough not to compete with it.
//
// THE TYPE SCALE IS THE ONE IN lv_conf.h AND IT IS NOT NEGOTIABLE HERE.
//
//   48   the total. The one thing someone walked over to see.
//   20   each cell's share -- readable if you look, invisible if you do not.
//   16   captions and units.
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

// Each cell panel carries its own zero button, and this is what it calls --
// with 0 for A and 1 for B.
//
// A BUTTON PER CELL RATHER THAN ONE FOR THE PAIR, because they answer different
// questions. Zeroing the assembly is what you do before weighing something.
// Zeroing ONE corner is what you do while setting the platform up, when the two
// raw counts start far apart and you want to know whether that is the mounting
// or the cell -- and the total, being a sum, cannot tell you.
//
// It sits IN the cell's own panel rather than in a row underneath, so which
// button belongs to which cell needs no label. A function pointer for the same
// reason ui_state.h exists: src/ui/ is pure LVGL and cannot reach scale.cpp.
void weightOnTareCell(void (*cb)(uint8_t cell));

}  // namespace ui
