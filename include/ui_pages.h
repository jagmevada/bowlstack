// The two pages, swipeable.
//
//   1  stock view -- the shipping screen
//   2  scope      -- four live ToF traces and a frame-rate readout
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

// How long without a touch before the UI returns to the stock page.
//
// A device left on the WiFi page is a device whose primary readout -- the bowl
// count someone walked over to see -- is not on screen. It also stops the more
// expensive pages rendering unattended, which matters on a board that has
// sensors to poll and a network to hold up.
static const uint32_t IDLE_HOME_MS = 60000;

}  // namespace ui
