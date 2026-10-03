#include "ui_platforms.h"

#include <stdio.h>
#include <string.h>

namespace ui {

void platformFromCounter(const ScaleView &sc, const char *label, PlatformRow &r) {
  memset(&r, 0, sizeof(r));
  r.present = true;
  r.role = PlatformRole::Counter;
  snprintf(r.label, sizeof(r.label), "%s", label);

  bool warming = false;
  for (uint8_t i = 0; i < CELLS; i++)
    if (sc.cell[i].state == Cell::Warming) warming = true;

  // THE SAME LADDER THE COUNTER'S OWN SCREEN USED, in its order: the first thing
  // that stops this being a weight is the one that is named.
  if (sc.online == 0) {
    r.state = warming ? Cell::Warming : Cell::Offline;
    // "no cells", not "offline": the counter's cells live in a separate housing on a
    // USB-C lead, and the fix for this is almost always that lead.
    snprintf(r.why, sizeof(r.why), "%s", warming ? "warming" : "no cells");
    return;
  }
  r.state = Cell::Online;
  if (sc.overRange) {
    r.overRange = true;
    snprintf(r.why, sizeof(r.why), "OVER");
    return;
  }
  if (!sc.calibrated) {
    snprintf(r.why, sizeof(r.why), "uncal");
    return;
  }
  // Untared only when there is NO zero at all -- a commissioned platform zero is a
  // zero (see the note in ui_weight.cpp's history: this once wore a permanent red
  // warning on a correctly-reading station).
  if (!sc.tared && !sc.platformZeroed) {
    snprintf(r.why, sizeof(r.why), "untared");
    return;
  }
  r.kgKnown = true;
  // NET OF THE EMPTY VESSEL, as the counter's total always was on this panel.
  r.grams = sc.totalGrams - sc.vesselOffsetG;
  if (sc.online < CELLS) {
    // The cells that ARE working give a genuine lower bound -- a missing corner
    // cannot carry a negative share -- so it is added in, and flagged.
    r.partial = true;
    snprintf(r.why, sizeof(r.why), "%u of %u", sc.online, CELLS);
  }
}

namespace {

// Larger is worse: what decides which platform the chip names.
uint8_t severity(const PlatformRow &r) {
  if (r.overRange) return 5;
  if (r.state == Cell::Offline) return 4;
  if (!r.kgKnown && r.state == Cell::Online) return 3;  // uncal / untared / no zero
  if (!r.kgKnown) return 2;                             // warming / settling
  if (r.partial) return 1;                              // lower bound, bowls unconfirmed
  return 0;
}

}  // namespace

PlatformTotal platformTotal(const State &s) {
  PlatformTotal t;
  uint8_t worstSev = 0;
  for (uint8_t i = 0; i < PLATFORMS; i++) {
    const PlatformRow &r = s.platforms[i];
    if (!r.present) continue;
    if (r.kgKnown) {
      t.any = true;
      t.grams += r.grams;
    }
    const uint8_t sev = severity(r);
    if (sev > 0) {
      t.partial = true;
      t.problems++;
      if (sev > worstSev) {
        worstSev = sev;
        t.worst = (int8_t)i;
      }
    }
  }
  t.fault = worstSev >= 4;
  return t;
}

void platformChip(const State &s, const PlatformTotal &t, char *buf, unsigned len) {
  if (t.worst < 0) {
    if (len) buf[0] = '\0';
    return;
  }
  const PlatformRow &r = s.platforms[t.worst];
  if (t.problems > 1)
    snprintf(buf, len, "%s %s +%u", r.label, r.why, (unsigned)(t.problems - 1));
  else
    snprintf(buf, len, "%s %s", r.label, r.why);
}

}  // namespace ui
