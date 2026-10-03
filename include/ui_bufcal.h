// Calibrate a buffer platform: type the known mass that is on it, in kilograms.
//
// ITS OWN PAGE, modelled on ui_vessel.cpp's kg keypad rather than built by
// re-plumbing that one: the vessel page works in the field, and a keypad that serves
// two masters through a mode flag is how the wrong number ends up in the wrong
// place. This one carries the slot it is calibrating, and only ever calibrates.
//
// KILOGRAMS WITH A DECIMAL POINT, because a known mass for a 200 kg platform is
// spoken in kilograms ("20 kg", "12.5 kg"), unlike the counter's reference mass,
// which is whole grams.

#pragma once

#include <lvgl.h>
#include <stdint.h>

namespace ui {

void buildBufCalPage(lv_obj_t *parent);
void bufCalOnClose(void (*cb)(void));
void bufCalOnApply(void (*cb)(uint8_t slot, float grams));
// Called on every entry: which platform this page now calibrates, and its label for
// the title. Clears the entry -- a half-typed mass must never survive to the next
// person who opens the page.
void bufCalOpenFor(uint8_t slot, const char *label);

}  // namespace ui
