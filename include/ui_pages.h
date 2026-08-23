// The two pages, swipeable.
//
//   1  menu   -- settings tree, unchanged from the touch-ui branch
//   2  weight -- the shipping screen, and home
//
// The menu tree keeps its shape. What changed on this branch is what the pages
// UNDER it are about: Sensors is the load-cell scope rather than four ToF
// traces, and Settings gained a Scale entry for tare and calibration, which are
// the two things a weighing device cannot work without and which have no home
// on a readout-only dashboard.
//
// Replaces the earlier six-page gallery. Those pages existed to settle
// questions -- which type sizes are legible on this panel, how big a touch
// target needs to be, whether pixel-shifting softens edges -- and all three are
// settled and written down: the type scale in lv_conf.h, the shift verdict in
// todo.md. Scaffolding kept past the decision it was built for stops being
// documentation and starts being clutter. Git has them if they are ever wanted.

#pragma once

#include <stdint.h>

namespace ui {

// Replaces the active screen with a two-page tileview.
void buildPages();

// Drives whichever page needs a clock. Call every loop iteration.
void pagesTick(uint32_t nowMs);

// Returns to the stock page and closes any detail overlay. Called by the idle
// timeout, and available to anything else that needs to get home.
void pagesGoHome();

// Up one level in the menu tree. Wired to every sub-page's back button.
void pagesBack();

// --- what the Scale page does ----------------------------------------------
// Function pointers rather than direct calls, for the same reason ui_state.h
// exists: src/ui/ is pure LVGL and cannot reach scale.cpp, which needs
// Arduino, FreeRTOS and NVS. The firmware installs the real handlers; the
// desktop preview installs none and the rows are inert, which is the honest
// behaviour for a machine with no cells attached.
//
// The calibration mass is a build constant rather than an on-screen entry.
// Typing a number on a 2" panel to do a thing that is done once per assembly is
// the wrong trade -- put the known mass on the platform, tap the row, and the
// factor follows.
void pagesOnScaleTare(void (*cb)(void));
void pagesOnScaleCalibrate(void (*cb)(void));
void pagesOnScaleClearCal(void (*cb)(void));

// Steps the moving-average length to the next of 8/16/32/64/128 and wraps. A
// runtime setting rather than a build flag because the right value is a
// judgement about the gesture -- how long a bowl may take to settle against how
// much the last digit may wander -- and a judgement is far easier to make by
// flipping between two of them with a mass on the platform than by reflashing.
void pagesOnScaleCycleAvg(void (*cb)(void));

// How long without a touch before the UI returns to the stock page.
//
// A device left on the WiFi page is a device whose primary readout -- the bowl
// count someone walked over to see -- is not on screen. It also stops the more
// expensive pages rendering unattended, which matters on a board that has
// sensors to poll and a network to hold up.
static const uint32_t IDLE_HOME_MS = 60000;

}  // namespace ui
