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
// THE WINDOW IS CHOSEN AT RUNTIME, from the Settings page or the console,
// because the right value is a judgement about the gesture rather than a fact
// about the hardware -- and a judgement is far easier to make by flipping
// between two of them with a mass on the platform than by reflashing.
//
// The whole trade is one line: NOISE FALLS WITH THE SQUARE ROOT of the window
// and LATENCY RISES LINEARLY with it.
//
// THE TIMINGS MOVED WHEN THE CONVERTER DID. These were derived at 80 SPS; the
// part now runs at 10 SPS, because the rate IS the anti-noise filter and 10 Hz
// puts the decimation nulls on 50 and 60 Hz mains (see nau7802.cpp). Each
// sample is therefore ~2.8x quieter than it was AND arrives eight times less
// often, so the same N is eight times the wait:
//
//     N     settling at 10 SPS
//     8     ~0.8 s        <- the default, and about right for a bowl
//    16     ~1.6 s
//    32     ~3.2 s
//    64     ~6.4 s
//   128     ~12.8 s       <- kept for completeness, not for use
//
// The gains shrink and the wait does not. Most of the noise reduction now comes
// from the converter rather than from this window, which is the better place
// for it to come from: a longer sinc filter rejects mains, and a boxcar average
// cannot.
//
// There is a second effect that looks like a coincidence and is not: a
// slower-moving number changes its rendered digits less often, and every digit
// change on this panel costs a repaint. Averaging harder makes the screen
// cheaper as well as steadier, so N and the frame rate move together -- the
// dashboard went from 11 fps to 13 on this change alone.
static const uint8_t WINDOW_CHOICES[] = {8, 16, 32, 64, 128};
static const uint8_t WINDOW_CHOICE_COUNT = 5;

// The ring is sized for the LARGEST choice and only `window()` of it is used, so
// changing N is a variable assignment rather than a reallocation. 128 int32s per
// cell is 1 kB of static RAM for both, which is the right thing to spend to keep
// a user-facing setting out of the heap.
static const uint8_t WINDOW_MAX = 128;

#ifndef BOWLSTACK_AVG_WINDOW
#define BOWLSTACK_AVG_WINDOW 64
#endif
static_assert(BOWLSTACK_AVG_WINDOW >= 4 && BOWLSTACK_AVG_WINDOW <= 128,
              "BOWLSTACK_AVG_WINDOW must be between 4 and 128");

// Samples currently averaged, and the setter. setWindow() snaps to the nearest
// allowed choice and persists it; the build flag is only the factory default for
// a unit that has never been told otherwise.
//
// CHANGING N CLEARS THE WINDOW rather than reinterpreting what is in it. A
// window that grew would average new samples against old ones taken under a
// different setting, and one that shrank would keep the newest N of a ring
// whose cursor is somewhere else entirely. Both produce a reading that is wrong
// for exactly one window-length -- which is precisely how long somebody would
// be looking at it after making the change.
uint8_t window();
void setWindow(uint8_t n);

// Steps to the next choice and wraps, for a menu row that is tapped rather than
// dragged. Returns the new value.
uint8_t cycleWindow();

// Trim scales with the window rather than being pinned, so it stays a small
// fraction of the samples instead of becoming a rounding error at 128 or half
// the data at 8. N/32, floored at 1:
//
//     8 -> 1    16 -> 1    32 -> 1    64 -> 2    128 -> 4
//
// Its job is to keep ONE knock on the bench out of the reading rather than
// smeared through it. A plain mean cannot do that; a mean of the middle can.
uint8_t windowTrim();

struct CellSnapshot {
  CellState state;
  uint8_t revision;   // 0xFF = never read
  int32_t counts;     // trimmed mean of the raw conversions, before tare
  int32_t rawCounts;  // the most recent single conversion, unfiltered
  uint16_t sps;       // conversions per second actually delivered
  uint8_t samples;    // how many are in the window right now

  // Peak-to-peak of the raw conversions currently in the window.
  //
  // THE FIGURE THAT SAYS WHETHER A CELL IS ALIVE. The trimmed mean beside it is
  // designed to look calm, so a cell whose bridge is disconnected and a cell
  // sitting perfectly still produce the same steady number -- and the first
  // time you find out which you had is when the weight does not move. A live
  // 350 ohm bridge at gain 128 wanders by tens to hundreds of counts between
  // conversions; an open input sits exactly where the offset calibration
  // parked it. See Nau7802::selfTest().
  int32_t pp;
  int32_t offset;     // tare, in counts
  float grams;        // this cell's share; meaningless unless `calibrated`

  // The conversion has hit the end of the 24-bit range, so this cell is no
  // longer measuring -- it is reporting its own ceiling.
  //
  // IT HAS TO BE SAID RATHER THAN INFERRED. A saturated cell does not fail; it
  // returns a large, steady, entirely plausible number, and the weight simply
  // stops rising as more is added. Everything else on this screen would keep
  // looking healthy. On a 20 kg cell at this gain the ceiling arrives around
  // 7 kg, which is well inside what someone would expect to be able to weigh.
  bool overRange;
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

  uint8_t window;   // samples currently averaged
  float calMassG;   // reference mass last calibrated against
  uint8_t online;  // cells currently producing conversions
  bool tared;      // a tare has been taken since the offsets were last cleared
  bool overRange;  // at least one cell is saturated -- the total is not a weight

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

// Zero ONE cell. Separate from tare() because the two answer different
// questions: tare() zeroes the assembly so the next thing placed on it reads
// its own weight, while this zeroes one corner against the others.
//
// It is the setup tool. Two cells under one platform rarely start level -- one
// mount sits proud, one cell has more of the platform over it -- and the raw
// counts say so loudly while the total, being a sum, says nothing at all. Being
// able to zero A and B independently is how you find out whether an uneven
// split is the mounting or the cell.
//
// A tare taken this way is persisted like any other.
void tareCell(uint8_t index);

// Why a calibration was refused, rather than a bare bool.
//
// THE REASON HAS TO COME FROM WHERE THE DECISION IS MADE. It used to be
// reconstructed by the caller from a published snapshot, which was both stale
// and incomplete: a below-minimum mass fell through to the caller's last rung
// and was reported as "no load" WITH THE ACTUAL DEFLECTION PRINTED BESIDE IT --
// a sentence contradicting its own number, and worse, one that names the
// platform as the fault. An operator who believes it re-tares with the
// reference mass still sitting there, which writes 175 g into the zero and
// persists it. A one-digit typo then becomes a destroyed tare.
enum class CalResult : uint8_t {
  Ok,
  Timeout,        // the measuring task did not answer -- distinct from a refusal
  NotTared,       // no tare, or one cell has never been zeroed
  CellsOffline,   // fewer than CELLS converting
  MassTooSmall,   // below MIN_CAL_GRAMS
  NoDeflection,   // the platform did not move enough to derive anything from
  Implausible,    // the factor came out nowhere near what this hardware can be
  Settling,       // the moving average has not filled since boot or a change
};

// A short line fit for a 240 px screen.
const char *calResultText(CalResult r);

// Records that the current load is `knownGrams` and derives countsPerGram from
// it. Requires every cell tared, and a deflection large enough to be worth
// measuring -- calibrating against noise would fix a wildly wrong factor.
//
// On success the mass is PERSISTED alongside the factor, so the next
// calibration on this unit opens pre-filled with the weight that was used last
// time, and `factorOut` receives the value that was actually computed.
//
// factorOut IS NOT OPTIONAL DECORATION. The caller cannot get it from
// snapshot(): the publish gate is tested against a timestamp latched before
// serviceCommands() runs, so the iteration that performs a calibration almost
// never publishes, and a snapshot taken straight afterwards carries the
// PREVIOUS factor up to 50 ms stale. The "calibrated to X counts/g" line was
// therefore usually quoting the value it had just replaced.
CalResult calibrate(float knownGrams, float *factorOut = nullptr);

// Loads the factor the firmware was built with -- the bench figure in
// platformio.ini -- and persists it as this unit's own.
//
// It exists so "clear" can mean CLEARED. Removing the NVS key made "cleared"
// and "never calibrated" the same stored state, so a unit cleared on purpose
// came back from its next power cycle showing kilograms again, derived from a
// factor nobody on that assembly had measured. With an explicit way back to the
// default, clearing can stick.
void restoreDefault();

// The build-time factor, for a UI that wants to say what Restore would load.
float defaultCountsPerGram();

// The reference mass last calibrated against on this unit, or the build default
// if it has never been calibrated. Grams.
float calMass();

// Forgets the calibration. The UI drops back to counts, which is the honest
// display for an uncalibrated scale.
void clearCalibration();

// Stack still unused, in BYTES -- uxTaskGetStackHighWaterMark returns bytes on
// ESP-IDF, not words. Scaling by 4 the way vanilla FreeRTOS requires reports
// four times the real headroom, which is how a task heading for overflow hides
// behind a comfortable number.
uint32_t stackFreeBytes();

}  // namespace scale
