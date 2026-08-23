// Device page -- the numbers behind the number.
//
// The dashboard is deliberately a WEIGHT and nothing else: one figure, smoothed,
// tared, in kilograms. Everything that got it there is hidden, which is right
// for a serving station and useless the moment something looks wrong.
//
// This page is the other half. Raw conversions with nothing done to them, the
// filtered mean beside them so the two can be compared, the peak-to-peak that
// says whether a cell is alive at all, the tare being subtracted, and the two
// settings -- counts per gram and the averaging window -- that turn one into the
// other. Read top to bottom it explains the dashboard's number completely.
//
// ONE LABEL, NOT TWENTY. Every figure here changes continuously, and a page of
// separate labels would be twenty invalidations per update where a single text
// block is one. It is also throttled well below the frame rate: nobody reads
// raw counts faster than a few times a second, and this page has no business
// costing what the dashboard costs.

#pragma once

#include <lvgl.h>

#include "ui_state.h"

namespace ui {

void buildDevicePage(lv_obj_t *parent);

// Call only while the page is visible. Rate-limits itself internally and
// compares the formatted block before writing, so a caller that forgets to gate
// it costs a string compare rather than a repaint.
void updateDevicePage(const State &s);

void deviceOnClose(void (*cb)(void));

}  // namespace ui
