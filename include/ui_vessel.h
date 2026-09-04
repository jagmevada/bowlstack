// Vessel offset page -- type in the mass of the empty serving vessel.
//
// It was a cycling menu row offering off / 2.0 / 2.5 / 3.0 / 3.5 kg, on the
// reasoning that ten vessels averaged about 2.5 kg so the useful values were
// few and known. That was wrong about the kitchen: vessels are whatever the
// kitchen owns, and a real one weighs 1.1 or 2.6 or 0.8 kg. Rounding a
// subtracted mass to the nearest half kilogram puts up to 250 g of error into
// every reading the trial is trying to measure -- which is larger than most of
// what the comparison is looking for.
//
// So: a keypad, in KILOGRAMS with a decimal point, because that is the unit the
// number is spoken in. The calibrate page's keypad is deliberately in whole
// grams -- a reference mass IS a whole number of grams -- and this one is not,
// which is why it is its own page rather than a mode on that one.
//
// ZERO IS OFF. There is no separate enable, because a vessel offset of nothing
// and no vessel offset are the same thing, and two controls that can disagree
// about one fact is one control too many.

#pragma once

#include <lvgl.h>

#include "ui_state.h"

namespace ui {

void buildVesselPage(lv_obj_t *parent);

void vesselOnClose(void (*cb)(void));

// Installed by the firmware. Called with the entered offset in GRAMS when OK is
// tapped; src/ui/ is pure LVGL and cannot reach scale.cpp itself.
void vesselOnApply(void (*cb)(float grams));

// Pre-fills the entry with what the unit currently has, so adjusting it is an
// edit rather than a retype. Called on every entry to the page.
void vesselSetOffset(float grams);

}  // namespace ui
