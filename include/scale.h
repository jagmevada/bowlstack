// The weighing assembly: three load cells, one platform, one number.
//
// THREE, AND THAT IS A MECHANICAL FACT RATHER THAN A PREFERENCE. Two cells
// leave the platform free to rock about the line joining them, so the reading
// depends on where a hand last touched it; three points define a plane and it
// cannot rock at all. The prototype ran two, weighed correctly and wobbled, and
// nothing in software fixes that.
//
// CELLS UNDER ONE PLATFORM SUM, THEY DO NOT AVERAGE. Each cell carries the
// share of the load that its corner takes, and those shares change as the bowl
// moves across the platform -- but their SUM is the total force regardless of
// where it sits. That is the whole reason a multi-cell platform works, and it
// is why the calibration below has exactly one gain constant, applied to the
// sum, rather than one per cell:
//
//     total_g = (sum of counts - sum of zeros - sum of tares) / countsPerGram
//
// THE CONSTANT DID NOT CHANGE WHEN THE THIRD CELL ARRIVED, which is the useful
// part of that identity. A load split three ways still produces the same total
// counts, so counts-per-gram describes the ASSEMBLY and is invariant to how
// many corners share the load. Recalibrate anyway -- but if the figure lands
// far from the two-cell one, something other than the cell count changed.
//
// The per-cell figures the UI shows are that same constant applied to each cell
// alone, so they are each cell's SHARE of the load and they add up to the
// total. They are worth showing because an uneven split is how you see a bowl
// placed off-centre, a mount fouling, or one cell not working -- none of which
// the total can tell you.
//
// The assumption is that the cells have equal sensitivity, which is what buying
// a matched set means. If they do not, the split will be wrong even though the
// total is right, and the fix is per-corner calibration -- a different and much
// longer procedure than the one here.
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

// MUST EQUAL ui::CELLS in ui_state.h. The two are separate because ui_state.h
// may not include a driver header -- see the note there -- and a mismatch is
// caught at compile time by the static_assert in loadcell_main.cpp rather than
// by a cell quietly missing from the dashboard.
static const uint8_t CELLS = 3;

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

// --- how many decimals the reading carries ---------------------------------
// 0.0 kg, 0.00 kg or 0.000 kg. DISPLAY ONLY: nothing computes from it, the
// stored counts and the calibration are untouched, and the Diagnose page keeps
// its full three places whatever this says.
//
// IT IS A SETTING BECAUSE THE HONEST ANSWER DEPENDS ON THE ASSEMBLY, not on the
// converter. At the bench figure of ~104 counts/g, a still platform's
// peak-to-peak runs 60 to 160 counts -- somewhere between half a gram and a
// gram and a half. So the third decimal of a kilogram is at or below this
// hardware's own noise: it is a digit that will not sit still, and a digit that
// will not sit still reads as a broken scale rather than as a precise one. The
// second decimal is clear of it with room to spare. The first throws away
// resolution the cells genuinely have.
//
// Which of those is right depends on what is being weighed and how steady the
// bench is, which is a judgement about the job. So the device asks instead of
// choosing, and defaults to what it has always shown.
//
// Rounding, never truncation -- see formatKg() in ui_weight.cpp. Truncating
// would make a scale that reads consistently light, and by up to a whole unit
// of the last place shown.
// BOWLSTACK_DECIMALS IS A BUILD FLAG FOR THE SAME REASON THE WINDOW IS ONE: the
// desktop preview cannot include this header -- src/ui/ is pure LVGL and this
// reaches nau7802.h and Arduino behind it -- so a shared default has to travel
// as a -D that both environments carry. CLAUDE.md's rule is that anything the
// UI renders comes from a shared fixture or a genuinely platform-specific
// source, and "what the Precision row says before anybody touches it" is
// emphatically the first kind. A literal 3 typed into ui_demo.cpp would be the
// third category, and the third category is how the simulator stops being
// evidence.
#ifndef BOWLSTACK_DECIMALS
#define BOWLSTACK_DECIMALS 3
#endif
static_assert(BOWLSTACK_DECIMALS >= 1 && BOWLSTACK_DECIMALS <= 3,
              "BOWLSTACK_DECIMALS must be 1, 2 or 3");

static const uint8_t DECIMAL_CHOICES[] = {1, 2, 3};
static const uint8_t DECIMAL_CHOICE_COUNT = 3;
static const uint8_t DECIMALS_DEFAULT = BOWLSTACK_DECIMALS;

uint8_t decimals();

// Clamped into range rather than accepted verbatim, for the same reason
// setWindow() snaps: the menu can only produce legal values, but a provisioning
// script must not be able to put the display somewhere the UI cannot describe.
void setDecimals(uint8_t d);

// Steps 1 -> 2 -> 3 -> 1, for a menu row that is tapped. Returns the new value.
uint8_t cycleDecimals();

// --- the per-cell breakdown on the dashboard -------------------------------
// DISPLAY ONLY, and persisted for the same reason the precision is: it belongs
// to the unit and to the job somebody is doing with it, not to the firmware
// image, so a power cycle in the middle of levelling a platform must not turn
// it back off.
//
// Off by default. The dashboard exists to answer one question, and three
// supporting numbers on it are for setup rather than for service.
bool showCells();

// Flips it, for a menu row that is tapped. Returns the new value. Takes effect
// on the scale task like every other setting here -- see the note beside
// wantDecimals_ for why the deferral is about `prefs_` and not about the value.
bool toggleShowCells();

// --- the automatic power-up tare -------------------------------------------
// WHAT IT IS FOR: a serving station is powered up with whatever tray or pot is
// already sitting on it, and asking somebody to remember to press Tare before
// every service is asking to be forgotten. So the device watches itself for a
// few seconds after boot and, if the reading holds still, zeroes to whatever is
// there. Volatile, like any tare -- the next power cycle looks again.
//
// It reports what it is doing rather than only what it did, because a device
// that quietly failed to zero itself must not look identical to one that
// succeeded.
//
// Waiting   cells not all converting yet, or their filters are not full
// Observing watching for a steady reading
// Done      a tare was taken automatically
// GaveUp    the platform never held still long enough; tare by hand
// Off       compiled out with -DBOWLSTACK_AUTOTARE=0
enum class AutoTare : uint8_t { Waiting, Observing, Done, GaveUp, Off };

// The empty vessel's mass, subtracted from the DISPLAYED and REPORTED figure
// only. Cycles off -> 2.0 -> 2.5 -> 3.0 -> 3.5 kg; persisted to NVS.
float vesselOffsetG();
// Zero switches it off. Queued and written on the scale task, never here.
float setVesselOffset(float grams);

AutoTare autoTareState();

// A short line fit for the dashboard.
const char *autoTareText(AutoTare s);

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
  // TWO OFFSETS, NOT ONE, because they are set at different times by different
  // people for different reasons and only one of them should survive a power
  // cycle. See the note above tare() below.
  int32_t platformZero;  // the platform's own weight. NVS. Set once per device.
  int32_t tare;          // this session's zero. RAM only. Set per measurement.
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

  uint8_t window;    // samples currently averaged
  uint8_t decimals;  // decimal places the kilogram reading is shown to
  bool showCells;    // per-cell breakdown under the dashboard total
  float calMassG;   // reference mass last calibrated against
  uint8_t online;  // cells currently producing conversions
  bool zeroed;     // every online cell has a stored platform zero
  bool tared;      // every online cell has a tare for this session

  // What the automatic power-up tare is doing. The dashboard says so rather
  // than leaving a device that quietly failed to zero itself looking identical
  // to one that succeeded.
  AutoTare autoTare;

  // EVERY ONLINE CELL HAS A COMMISSIONED PLATFORM ZERO, restored from NVS at
  // boot. Distinct from `tared`, which is this SESSION's trim and is forgotten
  // at power-off by design. A unit with a platform zero and no session tare
  // still knows what an empty platform reads -- it is referenced, not adrift.
  bool platformZeroed;

  // The mass of the empty serving vessel, subtracted from what is DISPLAYED and
  // REPORTED. 0 means the feature is off. Never applied to taring or to the
  // per-cell figures -- see the note where it is used.
  float vesselOffsetG;
  bool overRange;  // at least one cell is saturated -- the total is not a weight

  // Increments on every publish. Lets a reader tell a fresh snapshot from a
  // repeat without comparing floats.
  uint32_t seq;
};

// Opens the cell bus, brings up the mux and every converter behind it, restores
// tare and calibration from NVS, then starts the task. Blocking, and called
// from setup() -- the converters' own power-up and self-calibration take tens
// of milliseconds each and there is nothing useful to show until they are done.
//
// NO LONGER REQUIRES gfx.init() FIRST, though it is still called after it. Cell
// A used to share the touch controller's I2C port, so this had to run after
// LovyanGFX had opened it; the cells own port 1 now and this function opens
// that itself. What has not changed is that both go through lgfx rather than
// one of them opening Wire -- there must never be a second driver on a
// peripheral LovyanGFX holds.
void begin();

// A copy, taken under the mutex. Callers must not hold references into it.
Snapshot snapshot();

// True once the task has published at least once.
bool ready();

// --- the two zeros ---------------------------------------------------------
//
// A device is built in three steps and each one leaves a different constant
// behind. Collapsing them into a single offset -- which this did -- means the
// wrong one gets overwritten by the wrong person at the wrong time.
//
//   counts/g       once per CELL BUILD, against a known mass. NVS.
//   platform zero  once per DEVICE, after the platform is bolted on. NVS.
//                  Every unit gets a different platform and its weight is not
//                  a measurement anybody wants to see; it is a constant of the
//                  assembly, and it must survive every power cycle.
//   tare           once per MEASUREMENT. RAM ONLY. Whatever is sitting on the
//                  platform right now becomes zero, and the next power cycle
//                  forgets it -- because a tare taken around a bowl that has
//                  since been carried away is worse than no tare at all.
//
// setPlatformZero() is a commissioning action and is the only one that writes.
// tare() is the everyday one and deliberately does not.
void setPlatformZero();

// Zero this session at whatever is on the platform now. NOT persisted.
void tare();

// Zero ONE cell. Separate from tare() because the two answer different
// questions: tare() zeroes the assembly so the next thing placed on it reads
// its own weight, while this zeroes one corner against the others.
//
// It is the setup tool. Cells under one platform rarely start level -- one
// mount sits proud, one cell has more of the platform over it -- and the raw
// counts say so loudly while the total, being a sum, says nothing at all. Being
// able to zero each corner independently is how you find out whether an uneven
// split is the mounting or the cell.
//
// ON THREE CELLS IT ALSO FINDS THE CORNER THE PLATFORM IS NOT SITTING ON, which
// two cells could not show: with two, any load is shared and a proud mount only
// skews the split, but with three a platform can genuinely bridge one corner
// and leave it reading nothing while the total stays correct.
//
// A tare taken this way is persisted like any other.
void tareCell(uint8_t index);

// Ask the scale task to run the full-depth converter self-test on every cell:
// bridge, internally-shorted inputs, and channel 2, at sixteen samples each.
//
// RETURNS IMMEDIATELY -- it only raises a flag. The test itself blocks the
// measuring task for several seconds, because at 10 SPS every sample is 100 ms
// and the answer is worth more than the samples it costs. It must run THERE
// rather than on the caller's task: it shorts the PGA inputs and re-runs the
// offset calibration on parts scaleTask is polling, and two tasks writing one
// converter's configuration produces a wrong reading rather than a crash.
//
// The filters are cleared when it finishes; every sample taken before the
// re-calibration describes a different zero.
void requestSelfTest();

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
