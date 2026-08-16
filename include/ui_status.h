// The permanent status bar.
//
// Lives OUTSIDE the tileview, so it is not a page and cannot be swiped away.
// The screen is a column: this bar on top, the pages below it. Anything a
// person needs regardless of what they are looking at belongs here --
// which station this is, the time, whether the link is up, how much battery
// is left.
//
//   BWL-001        14:32        ((( )  [====]
//
// Everything in it is drawn from primitives rather than from a symbol font,
// with one exception (the charging bolt). That is deliberate: a WiFi glyph in
// a font is one shape, and signal STRENGTH needs four states plus a
// disconnected one. Four rectangles of increasing height say that; one glyph
// cannot.

#pragma once

#include <lvgl.h>

#include "ui_state.h"

namespace ui {

// Height reserved at the top of the screen. Sized to hold 16 px text with
// breathing room, which is the "information" tier of the type scale.
static const int16_t STATUS_H = 26;

void buildStatus(lv_obj_t *parent);

// Cheap enough to call every frame; it early-outs when nothing it displays has
// changed, so a steady state costs a handful of comparisons and no redraw.
void updateStatus(const State &s);

// The signal cluster is a tap target that opens the WiFi page. Registered as a
// callback rather than calling ui_pages directly, so the status bar stays a
// leaf -- it knows what was tapped, not what should happen next.
void statusOnWifiTap(void (*cb)(void));
void statusOnBatteryTap(void (*cb)(void));

}  // namespace ui
