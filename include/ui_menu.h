// A vertical list of tappable rows, used for the menu page and every sub-menu
// under it.
//
// It exists so that adding a setting is adding a row rather than designing a
// screen. The previous arrangement hung the WiFi and battery pages off the
// status-bar icons, which worked for exactly two pages and had nowhere to put a
// third: an icon is a fine shortcut and a poor navigation tree, and every new
// option would have needed either another icon in a 240 px bar or its own
// bespoke entry point.
//
// Rows are drawn at the type scale's 18 px floor with a chevron, and sized for
// a finger rather than a cursor -- 44 px, which is the same target the keyboard
// keys settled on.

#pragma once

#include <lvgl.h>

namespace ui {

// Builds an empty list inside `parent`. `onBack` is optional: pass nullptr for
// the top-level menu, which is a swipeable page and needs no back button.
// `onBack` is optional: pass nullptr for the top-level menu, which is a
// swipeable page.
//
// `onHome` replaces the swipe HINT with a real button. The swipe still works,
// but on this panel it is a poor primary gesture: the touch controller is
// polled at ~30 Hz through a driver that retries, so a drag has to be
// deliberate and slow to register -- which is a lot to ask of somebody holding
// a bowl in their other hand. A button is one tap and cannot be half-completed.
lv_obj_t *menuCreate(lv_obj_t *parent, const char *title, void (*onBack)(void),
                     void (*onHome)(void) = nullptr);

// `hint` is the muted right-hand text -- a current value, a status, or nullptr.
// It is what makes a menu useful at a glance rather than a list of nouns.
void menuAddRow(lv_obj_t *menu, const char *label, const char *hint, void (*cb)(void));

// Updates a row's hint in place, by index. Cheap enough to call every second;
// it early-outs when the text has not changed.
void menuSetHint(lv_obj_t *menu, uint8_t index, const char *hint);

}  // namespace ui
