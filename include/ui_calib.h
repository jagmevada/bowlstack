// Calibrate page -- type in the mass you actually have.
//
// The mass was a BUILD CONSTANT and that was wrong. It assumed the person
// calibrating owns the weight the firmware was compiled against, when what they
// actually own is whatever is to hand: a 500 g packet, a 1 kg kitchen weight, a
// bottle they put on a shop scale. Reflashing to change a number that belongs to
// the bench rather than to the product is the wrong trade, and it makes
// calibrating a second unit with a different weight a code change.
//
// So: a numeric keypad, the entered mass, and the live deflection in counts
// beside it. Tap OK and the factor is derived, stored in NVS, and restored on
// every boot thereafter.
//
// THE LIVE COUNTS ARE ON THIS PAGE ON PURPOSE. The single most common way to
// get a calibration wrong is to run it with nothing on the platform, or with
// the tare taken while the mass was already sitting there -- both produce a
// deflection near zero, and both are invisible if the page only shows what you
// typed. A number that moves when you press on the platform is proof you are
// calibrating against something.

#pragma once

#include <lvgl.h>

#include "ui_state.h"

namespace ui {

void buildCalibPage(lv_obj_t *parent);

// Live deflection and cell health. Call while the page is visible.
void updateCalibPage(const State &s);

void calibOnClose(void (*cb)(void));

// Installed by the firmware. Called with the entered mass in GRAMS when OK is
// tapped; src/ui/ is pure LVGL and cannot reach scale.cpp itself.
void calibOnApply(void (*cb)(float grams));

// The firmware's answer, shown on the page. `ok` picks the colour -- a
// calibration that was refused has to look refused, because the only other
// evidence is a factor the person cannot see from here.
void calibSetResult(const char *msg, bool ok);

// Pre-fills the entry with the last mass used on this unit, so recalibrating
// with the same weight is one tap rather than four.
void calibSetMass(float grams);

}  // namespace ui
