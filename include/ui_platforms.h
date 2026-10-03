// The arithmetic of the platforms on the dashboard: how the counter becomes a row,
// and how the rows become the TOTAL.
//
// ONE IMPLEMENTATION, SHARED. The firmware fills State from its drivers and the
// simulator from the demo fixture; both call these, so the preview cannot compute a
// total the device would not (CLAUDE.md: a shared fixture or a genuinely
// platform-specific source, and no third category).
//
// THE TOTAL IS HONEST ABOUT WHAT IT LEAVES OUT. A platform with no weight is not
// added as zero -- zero is a real weight, an empty shelf -- and a total missing one
// is a LOWER BOUND, which the caption says ("at least, kg") and the chip names.

#pragma once

#include "ui_state.h"

namespace ui {

// The counter (three cells summed, scale.cpp) as one dashboard row: weight net of the
// vessel offset, the same ladder of reasons the counter's own caption and chip used.
void platformFromCounter(const ScaleView &sc, const char *label, PlatformRow &row);

struct PlatformTotal {
  bool any = false;        // at least one platform has a weight to add
  bool partial = false;    // the sum leaves something out, or rests on an unconfirmed count
  float grams = 0.0f;
  uint8_t problems = 0;    // platforms that are absent from the sum or only partly in it
  int8_t worst = -1;       // index of the most serious of them, for the chip; -1 if none
  bool fault = false;      // that one is a fault (no reading / saturated), not a caveat
};

PlatformTotal platformTotal(const State &s);

// The chip text for `t.worst`: "B3 offline", "C1 untared +1".
void platformChip(const State &s, const PlatformTotal &t, char *buf, unsigned len);

}  // namespace ui
