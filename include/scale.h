// The weighing assembly: two load cells, one platform, one number.
//
// TWO CELLS UNDER ONE PLATFORM SUM, THEY DO NOT AVERAGE. Each cell carries the
// share of the load that its corner takes, and those shares change as the bowl
// moves across the platform -- but their SUM is the total force regardless of
// where it sits. That is the whole reason a multi-cell platform works, and it
// is why the calibration below has exactly one gain constant, applied to the
// sum, rather than one per cell:
//
//     total_g = (countsA + countsB - offsetA - offsetB) / countsPerGram
//
// The per-cell figures the UI shows are that same constant applied to each cell
// alone, so they are each cell's SHARE of the load and they add up to the
// total. They are worth showing because an uneven split is how you see a bowl
// placed off-centre, a mount fouling, or one cell not working -- none of which
// the total can tell you.
//
// The assumption is that the two cells have equal sensitivity, which is what
// buying a matched pair means. If they do not, the split will be wrong even
// though the total is right, and the fix is per-corner calibration -- a
// different and much longer procedure than the one here.
//
// THE CELLS GET THEIR OWN TASK. CLAUDE.md's rule is that one task owns a
// subsystem and state crosses task boundaries only as immutable snapshots under
// a mutex; docs/firmware.md's is that measurement never shares a loop with
// anything that can block. Polling the converters from the render loop would
// tie the sample rate to the frame rate, which is the wrong way round -- the
// panel would be deciding how well the scale measures.

#pragma once

#include <stdint.h>

#include "nau7802.h"

namespace scale {

static const uint8_t CELLS = 2;

// Trimmed moving average over raw counts, the same shape as TrimmedWindow but
// signed and sized for this rate. A load cell's noise is not symmetric in
// practice -- a knock, a door slam or a footfall on the same bench arrives as
// one large sample rather than as spread -- so the extremes are discarded
// rather than averaged in.
//
// 8 samples at 80 SPS is 100 ms of group delay: fast enough that placing a bowl
// looks instantaneous, long enough to bury the converter's own noise.
static const uint8_t WINDOW = 8;
static const uint8_t WINDOW_TRIM = 1;

struct CellSnapshot {
  CellState state;
  uint8_t revision;   // 0xFF = never read
  int32_t counts;     // trimmed mean of the raw conversions, before tare
  int32_t rawCounts;  // the most recent single conversion, unfiltered
  uint16_t sps;       // conversions per second actually delivered
  uint8_t samples;    // how many are in the window right now
  int32_t offset;     // tare, in counts
  float grams;        // this cell's share; meaningless unless `calibrated`
};

struct Snapshot {
  CellSnapshot cell[CELLS];

  // Sum of the per-cell shares. Meaningless unless `calibrated`, and the UI is
  // expected to show counts instead in that case rather than a number with no
  // unit behind it.
  float totalGrams;

  // Counts per gram for the assembly as a whole, or 0 when no calibration has
  // been performed. Zero is the honest state: a scale that has never seen a
  // known mass cannot convert to grams, and inventing a factor would produce a
  // confident wrong weight -- the one failure this codebase refuses everywhere.
  float countsPerGram;
  bool calibrated;

  uint8_t online;  // cells currently producing conversions
  bool tared;      // a tare has been taken since the offsets were last cleared

  // Increments on every publish. Lets a reader tell a fresh snapshot from a
  // repeat without comparing floats.
  uint32_t seq;
};

// Opens both buses, brings up both converters, restores tare and calibration
// from NVS, then starts the task. Blocking, and called from setup() -- the
// converters' own power-up and self-calibration take tens of milliseconds each
// and there is nothing useful to show until they are done.
//
// MUST BE CALLED AFTER gfx.init(). Cell A shares the touch controller's I2C
// port, which LovyanGFX initialises when the display starts; there is no second
// driver on that peripheral and this code must not become one.
void begin();

// A copy, taken under the mutex. Callers must not hold references into it.
Snapshot snapshot();

// True once the task has published at least once.
bool ready();

// Zero both cells at whatever is on the platform now, and persist it. This is
// the PLATFORM's weight, not the converter's offset -- Nau7802::calibrateAfe()
// is the other one, and confusing them gives a scale that reads plausibly and
// wrongly.
void tare();

// Records that the current load is `knownGrams` and derives countsPerGram from
// it. Requires a tare first, and a mass large enough to be worth measuring --
// calibrating against 5 g of noise would fix a wildly wrong factor.
//
// Returns false and changes nothing if either condition fails.
bool calibrate(float knownGrams);

// Forgets the calibration. The UI drops back to counts, which is the honest
// display for an uncalibrated scale.
void clearCalibration();

// Stack still unused, in BYTES -- uxTaskGetStackHighWaterMark returns bytes on
// ESP-IDF, not words. Scaling by 4 the way vanilla FreeRTOS requires reports
// four times the real headroom, which is how a task heading for overflow hides
// behind a comfortable number.
uint32_t stackFreeBytes();

}  // namespace scale
