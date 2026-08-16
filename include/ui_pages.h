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

}  // namespace ui
