// Weight view -- the primary screen.
//
// Replaces the bowl-count stock view on this branch. Same job, different
// measurement: one number big enough to read across the room, and the parts it
// was made of small enough not to compete with it.
//
// THE TYPE SCALE IS THE ONE IN lv_conf.h AND IT IS NOT NEGOTIABLE HERE.
//
//   84   the total. The one thing someone walked over to see, and the largest
//        a WEIGHT fits on a 240 px panel -- 56 at two or three decimals and 28
//        for uncalibrated counts, which are longer strings. See ui_font.h.
//   24   the two icon buttons in the action row, both navigation.
//   16   the caption, which carries the unit as well now.
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

// Tell the page it is no longer the visible one.
//
// It exists for one thing: the knob's press dot. updateWeight() runs only while
// this page is showing, but the switch is read continuously, so without this
// the first frame after returning would see a moved press counter and flash the
// dot for a press made somewhere else. Calling it every tick the page is hidden
// is fine -- it sets one bool.
void weightPageHidden();

// THERE IS NO weightOnTare(). Taring is not reachable from this page at all
// any more -- it is Settings > Scale > Tare, wired through pagesOnScaleTare().
//
// It was here, as the widest button on the screen at thumb height, and it was
// being hit by accident during service. Everything else on this page is either
// a reading or navigation, so a mis-tap is recoverable; a mis-tap on that one
// silently moved zero. The Scale row navigates back to this page after it
// fires, which is what confirms it: the total reads 0.0 kg or it does not.
// The gear button, which goes to the menu page -- the same place a swipe leads.
//
// The swipe is not removed and still works. It is simply not good enough to be
// the ONLY way: a drag on this panel has to be slow and deliberate to register,
// because the touch controller is polled at ~30 Hz through a driver that
// retries on every read, and a quick flick never accumulates enough samples to
// be read as a drag. One tap cannot be half-completed.
void weightOnSettings(void (*cb)(void));

// TRIAL HARNESS. Jumps to the OTHER main page -- knob from weight, weight from
// knob. A swipe already does it and this exists because a swipe on this panel
// has to be slow and deliberate to register, which is the wrong thing to ask of
// somebody holding a serving spoon. It goes away with the trial, leaving the
// gear alone in this row.
void weightOnSwap(void (*cb)(void));

}  // namespace ui
