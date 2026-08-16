// Battery detail page, reached by tapping the battery icon in the status bar.
//
// This is the DIAGNOSTIC surface, and that is what licenses it to show a
// percentage at all. The status bar deliberately shows only a band, because a
// resting-voltage estimate moves several points with load, temperature, cell
// age and per-unit ADC calibration -- so a number on a glanceable surface would
// render precision the measurement does not have.
//
// Here the audience is different: someone standing at the device, usually with
// a multimeter, asking why it says what it says. That is the same audience the
// console's power line already serves, and this page shows the same figures --
// pin voltage, cell voltage, percentage, band, charge state. Anything less and
// the screen is worse than the serial log it is meant to replace in the field.

#pragma once

#include <lvgl.h>

#include "ui_state.h"

namespace ui {

void buildBatteryPage(lv_obj_t *parent);
void updateBatteryPage(const State &s);
void batteryOnClose(void (*cb)(void));

}  // namespace ui
