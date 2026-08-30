// TRIAL HARNESS -- the blinded page.
//
// ##  TEMPORARY. ONE DEVICE. NOT PRODUCTION.  ##
//
// THIS PAGE EXISTS TO HIDE SOMETHING, and that is its entire design.
//
// The experiment is whether a person judging a vessel by eye is an acceptable
// substitute for a load cell. It only measures that if the person cannot see
// the load cell's answer. Leave the measured weight on screen and the attendant
// will -- reasonably, helpfully, and fatally -- dial the knob until the two
// agree, and the trial then measures their ability to read a number off a
// panel. Every result would look excellent and none would mean anything.
//
// So this page carries the knob percentage and nothing else. No total, no
// per-cell figures, no kilograms, no TARE. The weight page still exists and is
// one swipe away for whoever is running the trial; the attendant is simply not
// given it as the thing in front of them.
//
// The status bar is deliberately UNCHANGED and shared: device id, WiFi, time
// and battery are about the station, not about the food, and hiding them would
// cost the attendant the ability to notice a flat cell for no experimental
// benefit.
//
// Delete this file, its tile in ui_pages.cpp and the default-page setting to
// remove the harness.

#pragma once

#include <lvgl.h>

#include "ui_state.h"

namespace ui {

void buildKnob(lv_obj_t *parent);

// Same split as buildWeight/updateWeight, and for the same reason: rebuilding
// the tree per update would churn the LVGL heap and invalidate the whole screen
// for a change of one digit.
void updateKnob(const State &s);

// The gear, which goes to the menu -- the same place a swipe leads. A swipe on
// this panel has to be slow and deliberate to register, which is the wrong
// thing to require of somebody holding a serving spoon.
void knobOnSettings(void (*cb)(void));

}  // namespace ui
