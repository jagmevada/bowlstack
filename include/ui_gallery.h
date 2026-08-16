// A swipeable specimen: type at every available size, and a widget gallery.
//
// Exists to be looked at on the REAL PANEL. A monitor is ~96 DPI and this
// display is ~200; judging whether 14 px text is legible, or whether a stem
// looks ragged, on the wrong one of those two is worse than not checking at
// all, because it produces a confident wrong answer.
//
// It builds on the same pure-LVGL rules as ui_screens.h -- no board or driver
// headers -- so the identical code runs in the desktop preview, where it is
// useful for layout and wording, and on the device, where it is the only place
// the typography question can actually be settled.

#pragma once

namespace ui {

// Replaces the active screen with a tileview. Swipe horizontally, or press
// left/right, to move between pages.
void buildGallery();

}  // namespace ui
