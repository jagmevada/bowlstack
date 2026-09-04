#include "scale.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>   // esp_restart, for the cable-at-boot case

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "board_waveshare_s3.h"
#include "i2cmux.h"

// ON BY DEFAULT, because this branch is bring-up and the self-test answers the
// first question anybody asks of a new assembly: is that cell wired.
//
// WHAT IT COSTS IS SET BY THE OUTPUT RATE, and the comment that used to sit here
// said "roughly a second of boot per cell" -- true at the 80 SPS this firmware
// once ran at, and eight times wrong at the 10 SPS it runs at now. Measured, the
// full test was 5.4 s per cell, 16 s of a 20 s boot.
//
// So what runs at boot is the cheap half: four samples, bridge versus shorted,
// no channel-2 pass, about 1 s per cell. The full-depth test is on the console's
// 's' key, where it can take as long as it likes because somebody is watching.
//
// OFF BY DEFAULT NOW, and the measurement is what changed the default. Even the
// cheap half is ~1.2 s per cell at 10 SPS -- 3.5 s of a boot that the operator
// spends looking at a splash screen. That was over a third of the time to first
// dashboard, spent proving something that is true on every boot but the one
// where a cell has just been rewired.
//
// It has not been lost, which is the point of doing this rather than deleting
// the call: 's' on the console runs the FULL test, deeper than boot ever did,
// at the moment somebody actually suspects a cell. Boot prints one line saying
// so, because a diagnostic nobody knows about is the same as no diagnostic.
//
// -DBOWLSTACK_CELL_SELFTEST=1 puts it back at boot -- worth doing on a bench
// where cells are being wired and rewired.
#ifndef BOWLSTACK_CELL_SELFTEST
#define BOWLSTACK_CELL_SELFTEST 0
#endif

namespace scale {
namespace {

// --- bus assignment --------------------------------------------------------
// ONE BUS FOR ALL THREE CELLS, behind a TCA9548A. An lgfx port identifier,
// where the sign is the whole difference: a non-negative one is a hardware
// peripheral, a negative one is a bit-banged slot. See nau7802.h for why this
// is the abstraction rather than TwoWire.
//
// PORT 1, NOT PORT 0. Port 0 is LovyanGFX's -- the touch controller and the IMU
// on GPIO47/48 -- and it is opened by gfx.init() before this file runs. Port 1
// has no peripheral behind it until begin() opens it, so unlike the old cell A
// this bus IS initialised here.
//
// This replaced two buses, one of them bit-banged at 100 kHz off the render
// loop with no pull-ups fitted. Section 8 of board_waveshare_s3.h keeps the
// full account of why.
const int CELL_PORT = 1;

// 400 kHz, the same rate the touch controller runs at on the other port. The
// converters are rated for it, the TCA9548A is rated for it, and GPIO12/11
// carry real 4.7k pull-ups (R4/R5) rather than the ESP32's ~45 kohm internal
// ones -- which is precisely the thing that forced the old bus B down to 100.
//
// If a channel produces intermittent NAKs at this speed, the stub is missing
// its pull-ups. Fit them rather than slowing the bus: dropping the rate hides
// a soft edge instead of fixing it, and hides it unevenly.
const uint32_t CELL_HZ = 400000;

// The cell count here and the wiring table in the board header describe the
// same three cells, and there is no way to change one and be told about the
// other -- so say it at compile time. A short CELL_MUX_CH would otherwise be
// read past the end and the third cell would be configured for whatever channel
// followed it in memory.
static_assert(CELLS == board::CELL_COUNT, "scale::CELLS and board::CELL_COUNT disagree");

// Console names. The letters here, the Device page's NAME[] and the scope
// legend all have to agree about which corner is which, and index order is the
// only thing tying them together.
const char *const CELL_NAME[CELLS] = {"cell A", "cell B", "cell C"};

// --- persistence -----------------------------------------------------------
// NVS rather than a build flag, because tare and calibration belong to the
// ASSEMBLY and not to the firmware image: reflashing a unit must not silently
// throw away the gram factor someone derived with a known mass.
const char *NVS_NS = "bowlscale";
const char *KEY_CPG = "cpg";

// PER-CELL KEYS ARE BUILT FROM THE INDEX, not listed. They used to be two named
// constants each -- offA/offB, tarA/tarB -- which is a shape that does not
// survive a third cell: adding one means adding two more constants and
// remembering four call sites, and forgetting any of them leaves a cell reading
// a stored zero that belongs to nobody.
//
// The spelling is UNCHANGED for A and B, so a unit that was zeroed before the
// third cell existed keeps the constants it was set up with. C simply has no
// stored zero yet, which loadPersisted() already handles correctly.
const char *KEY_OFF_FMT = "off%c";
// The empty vessel's own mass. Ten were weighed and averaged about 2.5 kg, so
// the cycle below offers that and its neighbours rather than a free number: a
// tap-to-cycle row is the idiom this menu already uses for Average, Precision
// and Default page, and it needs no keypad.
const char *KEY_VESSEL = "vesg";
float vesselOffsetG_ = 0.0f;
const char *KEY_TARED_FMT = "tar%c";

// NVS keys are capped at 15 characters; these are four.
const char *cellKey(char *buf, size_t n, const char *fmt, uint8_t i) {
  snprintf(buf, n, fmt, (char)('A' + i));
  return buf;
}

// One figure per cell, joined as "a / b / c".
//
// WRITTEN ONCE BECAUSE FIVE CONSOLE LINES QUOTE THESE SIDE BY SIDE, and every
// one of them used to bake the cell count into its format string as "%ld / %ld"
// with two arguments. Adding a third cell would have dropped it silently from
// all five -- no warning, no wrong number, just a corner missing from exactly
// the lines someone reads to find out what a corner is doing.
const char *joinCells(char *buf, size_t n, const int32_t *v) {
  size_t k = 0;
  for (uint8_t i = 0; i < CELLS && k + 1 < n; i++) {
    const int w = snprintf(buf + k, n - k, "%s%ld", i ? " / " : "", (long)v[i]);
    if (w < 0) break;
    k += (size_t)w;
  }
  return buf;
}

// Wide enough for CELLS signed 24-bit values and their separators.
const size_t JOIN_BUF = CELLS * 14 + 1;
const char *KEY_WINDOW = "win";
const char *KEY_DECIMALS = "dec";
const char *KEY_SHOWCELLS = "cellrow";
const char *KEY_CALMASS = "calmass";

// What a calibration must clear, and the important half is the SECOND test.
//
// The first version of this demanded 200 g, which was a guess dressed as a
// guard: it was derived from an ASSUMED sensitivity of about a hundred counts
// per gram, and nobody had measured it. A 175 g reference mass -- a perfectly
// good one, 18,700 counts against 200 of noise -- was refused by a rule that had
// no idea what the cells were.
//
// So the real test is on COUNTS, which is sensitivity-independent: the
// deflection has to be far enough above the noise floor that the factor derived
// from it means something. 5,000 counts is twenty-five times the +/-200 measured
// here and is cleared by the 175 g reference with room to spare. The gram figure
// stays only as a sanity floor against a fat-fingered zero.
const float MIN_CAL_GRAMS = 20.0f;
const int32_t MIN_CAL_COUNTS = 5000;

// A NAU7802 conversion is signed 24-bit. Within a few per cent of the end of
// that range the part is no longer measuring the bridge, it is reporting its own
// ceiling -- and it does so as a large, steady, entirely plausible number. The
// weight just stops rising. Called out rather than left to be noticed.
const int32_t OVER_RANGE_COUNTS = 8000000;  // 95% of 2^23

// The readout cadence, and how often each cell is checked for a finished
// conversion. Both follow from the converter rate rather than being independent
// dials -- see the note in nau7802.cpp for why that rate is 10 SPS.
const uint32_t PUBLISH_MS = 100;  // 10 Hz, the fastest readout this product wants
const uint32_t POLL_MS = 10;

// --- step detection, and why it is not a PID -------------------------------
//
// A moving average has exactly one behaviour and it is the wrong one for half
// of what a scale does. At 128 samples and 10 SPS the window is 12.8 seconds,
// so putting a bowl down makes the reading CRAWL to the new value over a full
// window -- and taking it off crawls back. The old samples are not noise to be
// averaged away; they are a measurement of a platform that no longer exists.
//
// A PID would not help. There is no loop here and nothing to actuate: the
// reading is not chasing a setpoint, it is lagging because it is still averaging
// history. Derivative gain on a noisy load cell would amplify exactly the noise
// the window exists to remove.
//
// What a bench scale actually does, and what this does: WATCH FOR A STEP. When
// consecutive samples land far outside the current average, that is not noise,
// it is the load changing -- so throw the window away and restart from the new
// level. The reading jumps at once and then re-settles at the chosen averaging.
// Quiet when nothing is happening, immediate when something is.
//
// FIVE GRAMS, expressed in grams so it means something. Measured noise is about
// 150 counts peak-to-peak, or 1.4 g, so five is comfortably clear of it while
// being far below anything anybody puts on a platform on purpose. A slow drift
// of four grams never triggers -- correct, that is what the average is for.
const float STEP_GRAMS = 5.0f;

// The threshold before any calibration exists, in raw counts. ~9 g at the
// measured sensitivity, which is the same order as the calibrated figure.
const int32_t STEP_COUNTS_UNCAL = 1000;

// Consecutive samples beyond the threshold before the window is discarded.
// THREE, not one: a single sample outside the band is what noise looks like,
// and restarting the average on it would make the filter useless in exactly the
// conditions it exists for. Three at 10 SPS is 300 ms of detection latency,
// which is well inside the time it takes a hand to let go of a bowl.
const uint8_t STEP_CONFIRM = 3;

// The fewest samples a step can be judged against. See the note at the call
// site for why this is NOT the full window.
const uint8_t STEP_MIN_SAMPLES = 4;

// --- the automatic power-up tare -------------------------------------------
// A serving station is switched on with whatever tray or pot is already sitting
// on it, and asking somebody to remember to press Tare before every service is
// asking for it to be forgotten. So the device watches itself after boot and,
// once the reading holds still, zeroes to whatever is there.
//
// STABILITY IS PEAK-TO-PEAK OVER A WINDOW OF TIME, not a single comparison. A
// load cell settles asymptotically -- a pot put down two seconds ago is still
// creeping -- and a test that only asked "are two consecutive samples close"
// would pass in the middle of that creep and zero to a value the platform is
// still moving away from.
const uint32_t AUTOTARE_STABLE_MS = 3000;

// How still is still. 5 g against a measured 1.4 g of noise: loose enough that
// an ordinary bench passes in one pass, tight enough that a hand resting on the
// platform does not.
const float AUTOTARE_STABLE_G = 5.0f;
const int32_t AUTOTARE_STABLE_COUNTS_UNCAL = 1000;

// BOUNDED. If the platform never holds still -- somebody is loading it, a fan
// is blowing on it, a cell is intermittent -- the device says so and stops
// rather than lying in wait to zero away a real load ten minutes later. That
// second behaviour is the dangerous one: a silent tare during service would
// make a full bowl read empty.
const uint32_t AUTOTARE_DEADLINE_MS = 30000;

#ifndef BOWLSTACK_AUTOTARE
#define BOWLSTACK_AUTOTARE 1
#endif

// The factory default, overwritten by the first calibration on any unit. See
// the comment in platformio.ini for where the number came from and for what its
// size implies about the usable range.
#ifndef BOWLSTACK_COUNTS_PER_GRAM
#define BOWLSTACK_COUNTS_PER_GRAM 0.0f
#endif

// The mass the calibrate keypad OPENS WITH on a unit that has never been
// calibrated. After the first successful calibration the stored one wins, so
// this is a starting point rather than a setting.
#ifndef BOWLSTACK_CAL_MASS_G
#define BOWLSTACK_CAL_MASS_G 175
#endif

// --- the trimmed window ----------------------------------------------------
class CountWindow {
 public:
  void clear() {
    count_ = 0;
    next_ = 0;
  }
  // The active length is passed in rather than stored, so the one place that
  // owns the setting is scale::window() and this class cannot disagree with it.
  void push(int32_t v, uint8_t n) {
    if (n > WINDOW_MAX) n = WINDOW_MAX;
    ring_[next_] = v;
    next_ = (uint8_t)((next_ + 1) % n);
    if (count_ < n) count_++;
  }
  uint8_t size() const { return count_; }

  // Peak-to-peak, untrimmed on purpose. The trim exists to keep a knock out of
  // the WEIGHT; here the extremes are the measurement -- the question is how
  // far the raw signal moves, and discarding the outliers would answer a
  // different one.
  int32_t pp() const {
    if (count_ < 2) return 0;
    int32_t lo = ring_[0], hi = ring_[0];
    for (uint8_t i = 1; i < count_; i++) {
      if (ring_[i] < lo) lo = ring_[i];
      if (ring_[i] > hi) hi = ring_[i];
    }
    return hi - lo;
  }

  // Discards the WINDOW_TRIM highest and lowest before averaging. Below
  // 2*TRIM+2 samples the trim is skipped rather than applied to almost nothing
  // -- trimming 2 of 3 samples reports the median and calls it an average.
  int32_t mean(uint8_t trim) const {
    if (count_ == 0) return 0;
    int32_t sorted[WINDOW_MAX];
    for (uint8_t i = 0; i < count_; i++) sorted[i] = ring_[i];
    // Insertion sort, and it stays an insertion sort at 64 samples. Worst case
    // is ~2,000 comparisons; this runs twice per publish at 20 Hz, so about
    // 80,000 comparisons a second against a 240 MHz core. Reaching for
    // something asymptotically better would cost more in code than it saves in
    // cycles, and on nearly-sorted data -- which a settled load cell produces --
    // insertion sort is close to linear anyway.
    //
    // int16_t, not int8_t: with WINDOW at 64 the index runs to 63 and j to -1,
    // which int8_t still holds, but the type should not have to be re-checked
    // every time somebody widens the window.
    for (uint8_t i = 1; i < count_; i++) {
      const int32_t v = sorted[i];
      int16_t j = (int16_t)i - 1;
      while (j >= 0 && sorted[j] > v) {
        sorted[j + 1] = sorted[j];
        j--;
      }
      sorted[j + 1] = v;
    }
    uint8_t lo = 0, hi = count_;
    if (count_ >= (uint8_t)(2 * trim + 2)) {
      lo = trim;
      hi = (uint8_t)(count_ - trim);
    }
    int64_t acc = 0;
    for (uint8_t i = lo; i < hi; i++) acc += sorted[i];
    return (int32_t)(acc / (int32_t)(hi - lo));
  }

 private:
  int32_t ring_[WINDOW_MAX] = {0};
  uint8_t count_ = 0;
  uint8_t next_ = 0;
};

// --- owned by the task, touched by nothing else -----------------------------
Nau7802 cell_[CELLS];
CountWindow window_[CELLS];
// THE PLATFORM'S OWN WEIGHT. Persisted -- it is a constant of the assembly, not
// a measurement, and every device gets a different platform bolted to it.
int32_t platformZero_[CELLS] = {0, 0, 0};

// THIS SESSION'S ZERO. Deliberately NOT persisted: a tare taken around a bowl
// that has since been carried away is worse than no tare at all, so every power
// cycle starts from the platform zero and re-tares.
int32_t tare_[CELLS] = {0, 0, 0};
bool tareSet_[CELLS] = {false, false, false};

// --- the I2C cable is a USB-C pigtail, and it can be unplugged ---------------
// 3V3, SDA, SCL and GND run from the electronics enclosure to the load-cell
// housing over a USB-C lead. It is a connector, so it will be disconnected --
// during assembly, during cleaning, and by accident mid-service.
//
// TWO CASES, AND THEY NEED OPPOSITE TREATMENT. The distinction is whether this
// power cycle ever had working cells, which is what everOnline_ records.
//
//   CABLE PULLED MID-RUN.  The tare is still valid: the platform has not moved,
//     the stored zero still describes it, and the food on it is the same food.
//     Re-taring here would silently redefine empty as whatever is currently on
//     the scale -- so the converters are re-initialised and the tare is left
//     exactly alone. Re-init touches only the NAU7802's own registers (gain,
//     LDO, rate); platformZero_ and tare_ live up here and are not disturbed.
//
//   CABLE ABSENT AT BOOT.  Nothing was ever initialised and nothing was ever
//     tared, so there is no session state worth preserving -- and the auto-tare
//     that should have run at power-up has by now given up on its deadline.
//     Re-initialising in place would leave a scale that works and has never
//     been zeroed, which reads as a working scale. A full restart is both
//     simpler and more honest: it re-runs the whole boot, including auto-tare,
//     and lands in the state the device would have had if the cable had been
//     connected in the first place.
bool everOnline_ = false;
uint32_t nextLinkProbeMs_ = 0;
bool linkLostAnnounced_ = false;

// One second, as asked. Cheap: a single byte written to the mux address, and
// only ever attempted when no cell is answering, so a healthy station never
// runs it at all.
const uint32_t LINK_PROBE_MS = 1000;

// Everything the reading is measured from.
inline int32_t offsetOf(uint8_t i) { return platformZero_[i] + tare_[i]; }
float countsPerGram_ = 0.0f;
// PER CELL, not one flag for the assembly. The old single flag was set at the
// end of a block that handles both a global tare and a per-cell one -- so
// taring cell A alone marked the whole platform tared, the "not tared" chip
// disappeared, and a calibration taken then absorbed cell B's full absolute
// reading into the factor. It also survived a reboot, so a global tare taken
// while one cell was offline came back claiming both were zeroed.
bool cellZeroed_[CELLS] = {false, false, false};
uint8_t window_n_ = BOWLSTACK_AVG_WINDOW;
// Consecutive samples outside the step band, per cell.
uint8_t stepRun_[CELLS] = {0, 0, 0};
// The samples that tripped the detector, kept so the restarted window can be
// seeded with them instead of starting from one raw conversion.
int32_t stepBuf_[CELLS][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
float calMass_ = (float)BOWLSTACK_CAL_MASS_G;
uint8_t decimals_ = DECIMALS_DEFAULT;
// OFF by default. The dashboard answers one question; the breakdown is for
// somebody levelling a platform, and it persists so that person does not have
// to re-enable it after every power cycle mid-job.
bool showCells_ = false;
volatile uint8_t wantWindow_ = 0;  // 0 = no change pending
// DEFERRED THE SAME WAY THE WINDOW IS, and not for symmetry. Applying it
// straight from the menu handler would write NVS from the UI task while the
// scale task is using the same Preferences object, which is neither reentrant
// nor guarded -- a corrupt namespace rather than a wrong reading, and one that
// would show up as a unit that forgot its calibration. The value itself is a
// byte and would have been safe; `prefs_` is what is not.
volatile uint8_t wantDecimals_ = 0;  // 0 = no change pending
// TRISTATE RATHER THAN A BOOL, for the same reason wantDecimals_ is a value
// rather than a flag: 0 has to mean "nothing pending", and a plain bool has no
// spare state for that. 1 = turn off, 2 = turn on.
volatile uint8_t wantShowCells_ = 0;
// 0 = nothing pending; otherwise the vessel offset in grams, +1 so that a
// pending "off" is distinguishable from "no request". See cycleVesselOffset().
volatile uint16_t wantVessel_ = 0;

Preferences prefs_;

// --- published across the task boundary -------------------------------------
SemaphoreHandle_t mutex_ = nullptr;
Snapshot published_{};
bool ready_ = false;
TaskHandle_t task_ = nullptr;

// Commands from other tasks. Deliberately flags rather than direct calls: tare
// and calibrate must run where the windows live, or they would read a filtered
// value that the task is concurrently rewriting.
volatile bool wantTare_ = false;
// A BITMASK rather than a flag, so "tare A" and "tare B" arriving in the same
// task period do not lose one of each other.
volatile uint8_t wantTareMask_ = 0;
volatile bool wantClearCal_ = false;
volatile float wantCalGrams_ = 0.0f;
volatile uint8_t calCode_ = 0;    // a CalResult
volatile float calFactor_ = 0.0f;
volatile bool calDone_ = true;
volatile bool wantRestore_ = false;
volatile bool wantPlatformZero_ = false;
volatile bool wantSelfTest_ = false;

// --- auto-tare state, owned by the task ------------------------------------
#if BOWLSTACK_AUTOTARE
AutoTare autoTare_ = AutoTare::Waiting;
#else
AutoTare autoTare_ = AutoTare::Off;
#endif
uint32_t autoStartMs_ = 0;     // when the current stable-observation began
uint32_t autoBootMs_ = 0;      // when the task first ran, for the deadline
int32_t autoLo_[CELLS] = {0, 0, 0};
int32_t autoHi_[CELLS] = {0, 0, 0};

// Every cell's filter is full, so a tare or a calibration is taken against the
// averaging the operator actually selected.
//
// WITHOUT THIS BOTH USE WHATEVER HAPPENS TO BE IN THE WINDOW. Straight after a
// boot, after a cell recovers, or -- most reachably -- immediately after
// stepping the Average row, the window holds a handful of samples and the mean
// carries several times its settled noise. A tare taken then bakes that error
// into the zero and persists it; a calibration taken then derives the factor
// from it. Both look completely normal.
// The step threshold in raw counts, derived from the calibration when there is
// one so the figure means five grams rather than an arbitrary number of counts.
int32_t stepCounts() {
  if (countsPerGram_ > 0.0f) {
    const int32_t c = (int32_t)(STEP_GRAMS * countsPerGram_);
    return c > 0 ? c : STEP_COUNTS_UNCAL;
  }
  return STEP_COUNTS_UNCAL;
}

int32_t autoTareBandCounts() {
  if (countsPerGram_ > 0.0f) {
    const int32_t c = (int32_t)(AUTOTARE_STABLE_G * countsPerGram_);
    return c > 0 ? c : AUTOTARE_STABLE_COUNTS_UNCAL;
  }
  return AUTOTARE_STABLE_COUNTS_UNCAL;
}

bool windowsFull() {
  for (uint8_t i = 0; i < CELLS; i++) {
    if (cell_[i].state() != CellState::Online) continue;
    if (window_[i].size() < window_n_) return false;
  }
  return true;
}

// Every ONLINE cell has a stored platform zero. An offline cell is not counted
// against it: nothing can be zeroed while it is not converting, and refusing a
// calibration on that basis would be refusing it for a reason the operator
// cannot act on from this page.
bool allCellsZeroed() {
  for (uint8_t i = 0; i < CELLS; i++) {
    if (cell_[i].state() != CellState::Online) continue;
    if (!cellZeroed_[i]) return false;
  }
  return true;
}

bool allCellsTared() {
  for (uint8_t i = 0; i < CELLS; i++) {
    if (cell_[i].state() != CellState::Online) continue;
    if (!tareSet_[i]) return false;
  }
  return true;
}

void loadPersisted() {
  char key[8];
  prefs_.begin(NVS_NS, false);
  for (uint8_t i = 0; i < CELLS; i++)
    platformZero_[i] = prefs_.getInt(cellKey(key, sizeof(key), KEY_OFF_FMT, i), 0);
  // Not per cell -- one vessel offset for the platform.
  vesselOffsetG_ = prefs_.getFloat(KEY_VESSEL, 0.0f);
  // The build-time default is the fallback, so a freshly flashed board reads
  // kilograms straight away instead of counts. NVS still wins: a unit that has
  // been calibrated against its own mass keeps that figure across reflashes,
  // which is the whole reason the factor lives in NVS rather than in the image.
  countsPerGram_ = prefs_.getFloat(KEY_CPG, BOWLSTACK_COUNTS_PER_GRAM);
  // Stored as its own flag rather than inferred from a non-zero offset. The
  // inference is almost always right and wrong exactly once -- a platform that
  // happened to tare at 0 counts would come back from a reboot claiming it had
  // never been tared.
  for (uint8_t i = 0; i < CELLS; i++)
    cellZeroed_[i] = prefs_.getBool(cellKey(key, sizeof(key), KEY_TARED_FMT, i), false);

  // AN UNTARED CELL HAS NO OFFSET, and the two are made to agree here rather
  // than left to drift apart. They can disagree for a real reason -- this
  // firmware replaced a single assembly-wide `tared` key with one per cell, so
  // the first boot after the update finds offsets stored and no flag vouching
  // for them -- and the result was a display quietly subtracting a tare while
  // the chip said "not tared". Believing the flag and dropping the offset is
  // the safe direction: it shows the platform's true absolute reading, which is
  // obviously not zero, instead of a plausible number nothing stands behind.
  for (uint8_t i = 0; i < CELLS; i++) {
    if (!cellZeroed_[i] && platformZero_[i] != 0) {
      Serial.printf("  cell %c: platform zero %ld discarded -- nothing recorded it\n",
                    'A' + i, (long)platformZero_[i]);
      platformZero_[i] = 0;
    }
  }
  window_n_ = prefs_.getUChar(KEY_WINDOW, (uint8_t)BOWLSTACK_AVG_WINDOW);
  if (window_n_ < 4 || window_n_ > WINDOW_MAX) window_n_ = BOWLSTACK_AVG_WINDOW;
  decimals_ = prefs_.getUChar(KEY_DECIMALS, DECIMALS_DEFAULT);
  if (decimals_ < 1 || decimals_ > 3) decimals_ = DECIMALS_DEFAULT;
  showCells_ = prefs_.getBool(KEY_SHOWCELLS, false);
  calMass_ = prefs_.getFloat(KEY_CALMASS, (float)BOWLSTACK_CAL_MASS_G);
  if (calMass_ < MIN_CAL_GRAMS) calMass_ = (float)BOWLSTACK_CAL_MASS_G;
  prefs_.end();

  if (countsPerGram_ > 0.0f) {
    char zeros[JOIN_BUF];
    Serial.printf("  %.3f counts/g, platform zero %s, window %u samples\n", countsPerGram_,
                  joinCells(zeros, sizeof(zeros), platformZero_), window_n_);
    // THE RESTORED PRECISION, SAID OUT LOUD, for the same reason the window and
    // the platform zero above it are: everything in this namespace survives a
    // power cycle, and a value that survives silently cannot be told apart from
    // one that reset to its default. Confirming this setting had come back
    // meant power-cycling the board and pressing the cycle key to see what it
    // stepped FROM -- which is detective work to answer a question the boot
    // line should simply have answered.
    Serial.printf("  reading 0.%0*d kg (%u decimals, %ld g per step)\n", (int)decimals_, 0,
                  decimals_, (decimals_ == 1) ? 100L : (decimals_ == 2) ? 10L : 1L);
    // THE CEILING, STATED AT BOOT. A signed 24-bit conversion divided by the
    // measured sensitivity is the most load a single cell can report before it
    // saturates -- and on this assembly that lands well below the number
    // printed on the side of the cell. Far better to read it once here than to
    // meet it as a weight that stopped going up.
    Serial.printf("  full scale: ~%.2f kg on ONE cell before the ADC saturates\n",
                  (float)OVER_RANGE_COUNTS / countsPerGram_ / 1000.0f);
  } else {
    // Said out loud rather than left to be inferred from a screen showing
    // counts. An uncalibrated scale is a working ADC, not a broken scale.
    Serial.println("  no calibration stored -- readings will be in COUNTS, not grams");
  }
}

// NVS WRITES BLOCK THIS TASK, and briefly the other core with it: a flash erase
// disables the instruction cache, so code running from flash elsewhere stalls
// for the duration. That is acceptable here because both callers are things a
// person just tapped -- a tare or a calibration, seconds apart at the very most
// -- and unacceptable anywhere on the sampling path, which is why neither the
// poll loop nor publish() ever touches NVS.
void storeOffsets() {
  char key[8];
  prefs_.begin(NVS_NS, false);
  for (uint8_t i = 0; i < CELLS; i++) {
    prefs_.putInt(cellKey(key, sizeof(key), KEY_OFF_FMT, i), platformZero_[i]);
    prefs_.putBool(cellKey(key, sizeof(key), KEY_TARED_FMT, i), cellZeroed_[i]);
  }
  prefs_.end();
}

void storeFactor() {
  prefs_.begin(NVS_NS, false);
  prefs_.putFloat(KEY_CPG, countsPerGram_);
  // The MASS goes with the factor, not because anything computes from it but
  // because the next person to calibrate this unit almost certainly has the
  // same weight in their hand. Pre-filling the keypad with it turns a four-tap
  // job into one.
  prefs_.putFloat(KEY_CALMASS, calMass_);
  prefs_.end();
}

void publish() {
  Snapshot s{};
  s.window = window_n_;
  s.decimals = decimals_;
  s.showCells = showCells_;
  s.calMassG = calMass_;
  s.countsPerGram = countsPerGram_;
  s.calibrated = countsPerGram_ > 0.0f;
  s.tared = allCellsTared();
  s.zeroed = allCellsZeroed();
  s.autoTare = autoTare_;

  int32_t sumNet = 0;
  for (uint8_t i = 0; i < CELLS; i++) {
    CellSnapshot &c = s.cell[i];
    c.state = cell_[i].state();
    c.revision = cell_[i].revision();
    c.rawCounts = cell_[i].counts();
    c.counts = window_[i].mean(windowTrim());
    c.samples = window_[i].size();
    // AN ONLINE CELL WITH AN EMPTY WINDOW HAS NO READING, and mean() answers 0
    // for one. Zero counts is not zero grams -- it is minus the tare, which on
    // a tared platform is kilograms, with the wrong sign. weightState() takes
    // no sample count, and -2000 g sits inside WEIGHT_PUBLISH_MIN_G, so the
    // snapshot went out as a perfectly ordinary weight_state 'ok'.
    //
    // The panel never showed it -- the operator is on the settings page and the
    // snapshot lives about 100 ms -- but the uplink task snapshots on its own
    // 250 ms clock, so it PATCHed device_status and appended a permanent
    // weight_samples row saying the vessel weighed minus two kilograms.
    //
    // Reachable from two window_[i].clear() sites: Settings > Scale > Average,
    // and the console self-test. The other two clears are already covered, one
    // by an Online gate and one by an immediate reseed; these two were the gap.
    //
    // Returning abandons the whole snapshot rather than patching this cell,
    // because published_ then keeps its previous value -- which was true -- and
    // a total assembled from two live corners and one empty one is not a total.
    if (c.state == CellState::Online && c.samples == 0) return;
    c.pp = window_[i].pp();
    // Tested on the RAW conversion, not the filtered mean. A trimmed average of
    // saturated samples is still saturated, but it lags -- and the point of this
    // flag is to fire the moment the part stops measuring.
    c.overRange = (c.rawCounts > OVER_RANGE_COUNTS) || (c.rawCounts < -OVER_RANGE_COUNTS);
    if (c.overRange) s.overRange = true;
    c.sps = cell_[i].sps();
    c.platformZero = platformZero_[i];
    c.tare = tare_[i];

    const int32_t net = c.counts - offsetOf(i);
    // A cell that is not Online contributes NOTHING, and specifically not its
    // last known value. A stale count added into the total would keep a dead
    // cell's share of the load in the number indefinitely, which is a confident
    // wrong weight rather than a visibly missing one.
    if (c.state == CellState::Online) {
      sumNet += net;
      s.online++;
      c.grams = s.calibrated ? (float)net / countsPerGram_ : 0.0f;
    } else {
      c.grams = 0.0f;
    }
  }
  // REAL MASS, ALWAYS. The vessel offset is carried alongside rather than
  // baked in here, because three things read this figure and only two of them
  // want it net: the panel and the uplink do, the per-cell share arithmetic
  // does NOT (its denominator must match the per-cell grams beside it), and
  // taring must not either -- a tare has to zero the REAL platform or the
  // offset would be applied twice.
  s.totalGrams = s.calibrated ? (float)sumNet / countsPerGram_ : 0.0f;
  // NOT GATED ON calibrated. Both consumers that apply it are already inside
  // their own calibrated check, and the one place this actually reaches is the
  // menu HINT -- which, gated, read "off" after Clear calibration while NVS
  // still held 2500. The next tap would then read the real value and jump to
  // 3000, so the operator asks for 2.0 kg and silently gets 3.0.
  s.vesselOffsetG = vesselOffsetG_;

  // A COMMISSIONED ZERO IS A ZERO. Every online cell carries a platformZero
  // restored from NVS, so the reading is referenced to an empty platform even
  // with no session tare -- which is the normal state now that the power-up
  // auto-tare is compiled out.
  // FROM THE FLAG, NOT FROM THE VALUE. allCellsZeroed() reads the per-cell
  // record that vouches for each platform zero; testing platformZero != 0
  // instead would re-introduce exactly the inference loadPersisted() refuses --
  // "a platform that happened to tare at 0 counts would come back from a reboot
  // claiming it had never been tared".
  s.platformZeroed = s.zeroed;
  s.seq = published_.seq + 1;

  if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) return;
  published_ = s;
  ready_ = true;
  xSemaphoreGive(mutex_);
}

void serviceCommands() {
  if (wantVessel_) {
    vesselOffsetG_ = (float)(wantVessel_ - 1);
    wantVessel_ = 0;
    prefs_.begin(NVS_NS, false);
    prefs_.putFloat(KEY_VESSEL, vesselOffsetG_);
    prefs_.end();
    Serial.printf("scale: vessel offset -> %.1f kg\n", vesselOffsetG_ / 1000.0f);
  }

  if (wantWindow_) {
    const uint8_t n = wantWindow_;
    wantWindow_ = 0;
    if (n != window_n_) {
      window_n_ = n;
      // CLEARED, not reinterpreted. A window that grew would average new
      // samples against old ones taken under a different setting; one that
      // shrank would keep the newest N of a ring whose cursor is elsewhere.
      // Either way the reading is wrong for exactly one window-length, which is
      // precisely how long somebody stares at it after making the change.
      for (uint8_t i = 0; i < CELLS; i++) window_[i].clear();
      prefs_.begin(NVS_NS, false);
      prefs_.putUChar(KEY_WINDOW, window_n_);
      prefs_.end();
      // The settling time is derived from the cell's MEASURED rate, not from a
      // number typed into a format string. It said "at 79 SPS" for a while
      // after the converter was dropped to 10, which quietly understated every
      // settling time it printed by a factor of eight -- the one figure this
      // line exists to give.
      const uint16_t sps = cell_[0].sps() ? cell_[0].sps() : 10;
      Serial.printf("scale: averaging %u samples (~%u ms at %u SPS), trim %u\n", window_n_,
                    (unsigned)((window_n_ * 1000UL) / sps), sps, windowTrim());
    }
  }

  if (wantDecimals_) {
    const uint8_t d = wantDecimals_;
    wantDecimals_ = 0;
    if (d != decimals_) {
      decimals_ = d;
      // NOTHING IS CLEARED, unlike a window change: this touches how the number
      // is printed and not what goes into it, so the filter, the zeros and the
      // calibration are all still describing the same thing they were a
      // millisecond ago. The reading changes its last digit and nothing else.
      prefs_.begin(NVS_NS, false);
      prefs_.putUChar(KEY_DECIMALS, decimals_);
      prefs_.end();
      // The resolution is quoted against the MEASURED sensitivity rather than
      // the bench figure, so a unit calibrated to its own cells says what its
      // own last digit is worth. An uncalibrated unit has no grams to quote.
      if (countsPerGram_ > 0.0f) {
        const long stepG = (decimals_ == 1) ? 100L : (decimals_ == 2) ? 10L : 1L;
        Serial.printf("scale: reading -> %u decimals (%ld g per step, ~%.1f counts)\n",
                      decimals_, stepG, (double)(stepG * countsPerGram_));
      } else {
        Serial.printf("scale: reading -> %u decimals\n", decimals_);
      }
    }
  }

  // DEFERRED HERE FOR THE SAME REASON AS DECIMALS, and the reason is `prefs_`
  // rather than the value. The value is a bool and would have been safe to set
  // from the menu handler; the Preferences object is neither reentrant nor
  // guarded, and writing it from the UI task while this one is using it
  // corrupts the namespace -- which surfaces as a unit that forgot its
  // calibration, not as a display setting that misbehaved.
  if (wantShowCells_) {
    const bool want = (wantShowCells_ == 2);
    wantShowCells_ = 0;
    if (want != showCells_) {
      showCells_ = want;
      prefs_.begin(NVS_NS, false);
      prefs_.putBool(KEY_SHOWCELLS, showCells_);
      prefs_.end();
      Serial.printf("scale: cells on home -> %s\n", showCells_ ? "shown" : "hidden");
    }
  }

  // A tare is refused on a half-filled window for the same reason a calibration
  // is -- it would bake the filter's start-up error into the zero and persist
  // it. Left pending rather than dropped, so the tap is honoured a moment later
  // instead of being silently lost.
  if ((wantTare_ || wantTareMask_) && !windowsFull()) {
    static uint32_t nextSayMs = 0;
    const uint32_t now = millis();
    if ((int32_t)(now - nextSayMs) >= 0) {
      nextSayMs = now + 1000;
      Serial.printf("scale: tare pending -- filter %u/%u samples\n", window_[0].size(),
                    window_n_);
    }
  } else if (wantTare_ || wantTareMask_) {
    // wantTare_ means the whole assembly; the mask means named cells. Reading
    // both here rather than having tare() set an all-ones mask keeps "zero
    // everything" working even if a cell is offline at the moment it is asked.
    const uint8_t mask = wantTare_ ? (uint8_t)0xFF : wantTareMask_;
    wantTare_ = false;
    wantTareMask_ = 0;

    for (uint8_t i = 0; i < CELLS; i++) {
      if (!(mask & (1u << i))) continue;
      // Only a cell that is actually converting can be tared. Zeroing an
      // offline cell against whatever its window last held would bake that
      // stale value in as the platform's weight.
      // The flag is set HERE, inside the Online branch, beside the assignment it
      // describes -- so a cell that was skipped is not marked zeroed, and the
      // per-cell buttons cannot claim the assembly is ready when only one
      // corner has been done.
      if (cell_[i].state() == CellState::Online) {
        // MEASURED FROM THE PLATFORM ZERO, not from raw. A tare of 0 then means
        // "nothing on top of the platform", which is what anybody reading the
        // Device page expects it to mean -- and it keeps the two constants
        // independent, so re-taring never disturbs the stored platform weight.
        tare_[i] = window_[i].mean(windowTrim()) - platformZero_[i];
        tareSet_[i] = true;
      }
    }
    // DELIBERATELY NOT PERSISTED -- see the note on tare() in scale.h. The
    // platform zero is the constant of the assembly; this is the constant of
    // the next thirty seconds.
    // "AB-" rather than a count, so the line says WHICH corners moved. Built
    // from the mask rather than written out, for the same reason joinCells
    // exists: the old two-argument version could not have grown a third letter
    // without somebody noticing it was missing.
    char who[CELLS + 1];
    for (uint8_t i = 0; i < CELLS; i++) who[i] = (mask & (1u << i)) ? (char)('A' + i) : '-';
    who[CELLS] = '\0';
    char tares[JOIN_BUF];
    Serial.printf("scale: tared %s at %s above the platform\n", who,
                  joinCells(tares, sizeof(tares), tare_));
  }

  // --- the platform zero, the one that persists ----------------------------
  if (wantPlatformZero_) {
    wantPlatformZero_ = false;
    if (!windowsFull()) {
      // Same reason a tare waits. Zeroing against a filter that has not settled
      // bakes its start-up error into a constant that then survives every power
      // cycle -- the worst possible place for it to live.
      Serial.println("scale: platform zero deferred -- filter still settling, try again");
    } else {
      for (uint8_t i = 0; i < CELLS; i++) {
        if (cell_[i].state() != CellState::Online) continue;
        platformZero_[i] = window_[i].mean(windowTrim());
        cellZeroed_[i] = true;
        // The session tare goes to zero with it. Both describe the same
        // reading, and leaving the old tare in place would subtract the
        // platform twice.
        tare_[i] = 0;
        tareSet_[i] = true;
      }
      storeOffsets();
      char zeros[JOIN_BUF];
      Serial.printf("scale: platform zero stored at %s counts\n",
                    joinCells(zeros, sizeof(zeros), platformZero_));
    }
  }

  if (!calDone_) {
    const float known = wantCalGrams_;
    CalResult r = CalResult::Ok;
    float derived = 0.0f;

    int32_t sumNet = 0;
    uint8_t online = 0;
    for (uint8_t i = 0; i < CELLS; i++) {
      if (cell_[i].state() != CellState::Online) continue;
      sumNet += window_[i].mean(windowTrim()) - offsetOf(i);
      online++;
    }

    // ORDERED MOST SPECIFIC FIRST, so the reason names the thing the operator
    // has to change. Every rung here is a condition this code can actually
    // test; nothing falls through to a catch-all that has to guess.
    if (online < CELLS) {
      r = CalResult::CellsOffline;
    } else if (!allCellsTared()) {
      r = CalResult::NotTared;
    } else if (known < MIN_CAL_GRAMS) {
      // ITS OWN RUNG. This used to fall through to the deflection case and be
      // reported as "no load" with the real deflection printed beside it --
      // which told the operator to fix the platform when what needed fixing was
      // the number they typed.
      r = CalResult::MassTooSmall;
    } else if (!windowsFull()) {
      r = CalResult::Settling;
    } else if (sumNet < MIN_CAL_COUNTS) {
      // A negative sum lands here too, and it should: the cells wired to
      // subtract rather than add is the same "this is not a usable deflection"
      // answer, and the count is printed so the sign is visible.
      r = CalResult::NoDeflection;
    } else {
      derived = (float)sumNet / known;
      // A PLAUSIBILITY BAND, because every guard above is one-sided and a
      // mistyped mass sails through all of them. Typing 1750 for a 175 g
      // reference gives a tenth of the right factor and a confident green tick;
      // typing 20 with a 2 kg mass gives a hundred times too much. Both persist,
      // and nothing else on the screen contradicts them.
      //
      // Banded against the BUILD DEFAULT rather than absolute numbers, so it
      // travels: an image built for 5 kg cells carries a different default and
      // the band moves with it.
      //
      // 0.3x TO 3x, NOT 0.1x TO 10x, AND THE OLD BAND CAUGHT NOTHING. It was
      // exactly one decade wide with strict comparisons, so a 10x slip -- the
      // one error this guard names, "typing 1750 for a 175 g reference" --
      // always landed on a boundary rather than outside it, and always passed
      // in one of its two directions: a factor-of-ten-high slip is admitted
      // whenever the true sensitivity is at or above the build default, and a
      // ten-low slip whenever it is at or below. On the very bench assembly the
      // 106.857 default came from, the 175/1750 case cleared the lower bound by
      // 0.00001 counts/g.
      //
      // A decade was chosen to be "far wider than part-to-part spread", which
      // it is -- and so is 3x. Real spread between load cells of one type is
      // tens of percent; the tightest thing this must not reject is a genuinely
      // different but correctly measured assembly, and 3x clears that with room
      // to spare while a decimal-place slip no longer fits inside it.
      const float ref = (float)BOWLSTACK_COUNTS_PER_GRAM;
      if (ref > 0.0f && (derived < ref * 0.3f || derived > ref * 3.0f)) {
        r = CalResult::Implausible;
        Serial.printf("scale: %.3f counts/g is outside %.1f..%.1f -- check the mass\n",
                      derived, ref * 0.3f, ref * 3.0f);
      }
    }

    if (r == CalResult::Ok) {
      countsPerGram_ = derived;
      calMass_ = known;
      storeFactor();
      Serial.printf("scale: calibrated -- %ld counts for %.0f g = %.3f counts/g\n",
                    (long)sumNet, known, countsPerGram_);
    } else {
      Serial.printf("scale: calibration refused -- %s (online %u/%u, net %ld counts, %.0f g)\n",
                    calResultText(r), online, CELLS, (long)sumNet, known);
    }

    // The FACTOR travels back with the verdict. The caller cannot read it from
    // a snapshot -- see the note on calibrate() in scale.h.
    calFactor_ = derived;
    calCode_ = (uint8_t)r;
    calDone_ = true;
  }

  // SERVICED AFTER THE CALIBRATION, not before. When both are pending in one
  // pass a clear must win, or a request that was still in flight when somebody
  // tapped "Clear calibration" would resurrect the factor that tap just removed.
  if (wantClearCal_) {
    wantClearCal_ = false;
    countsPerGram_ = 0.0f;
    // STORED AS ZERO, not removed. Removing the key made "cleared" and "never
    // calibrated" the same state, and loadPersisted() resolves that state to the
    // build default -- so a unit cleared deliberately came back from its next
    // power cycle showing kilograms again from a factor nobody on that assembly
    // had derived. Zero is a state of its own and it sticks. Restore default is
    // now the deliberate way back.
    prefs_.begin(NVS_NS, false);
    prefs_.putFloat(KEY_CPG, 0.0f);
    prefs_.end();
    Serial.println("scale: calibration cleared -- counts, and it stays cleared");
  }

  if (wantRestore_) {
    wantRestore_ = false;
    countsPerGram_ = (float)BOWLSTACK_COUNTS_PER_GRAM;
    calMass_ = (float)BOWLSTACK_CAL_MASS_G;
    storeFactor();
    Serial.printf("scale: restored the built-in default -- %.3f counts/g from %d g\n",
                  countsPerGram_, (int)BOWLSTACK_CAL_MASS_G);
  }

  // --- the self-test, on request ------------------------------------------
  // SERVICED HERE RATHER THAN CALLED FROM THE CONSOLE HANDLER, and for the
  // reason every other command in this block is: the console runs on the UI
  // task, and this one shorts the PGA inputs and re-runs the AFE calibration on
  // parts that scaleTask is concurrently polling. Two tasks driving one
  // converter's configuration registers is not a race that shows up as a crash
  // -- it shows up as a reading.
  //
  // IT BLOCKS THE MEASUREMENT FOR SEVERAL SECONDS and that is accepted, because
  // somebody asked for it. The windows are cleared afterwards: the offset
  // calibration has been re-run, so every sample taken before it describes a
  // different zero.
  if (wantSelfTest_) {
    wantSelfTest_ = false;
    Serial.println("\nscale: self-test -- measurement pauses while this runs\n");
    for (uint8_t i = 0; i < CELLS; i++) cell_[i].selfTest(16, true);
    for (uint8_t i = 0; i < CELLS; i++) window_[i].clear();
    Serial.println("\nscale: self-test done, filters restarted\n");
  }
}

// --- the automatic power-up tare -------------------------------------------
// Runs once per boot, before any manual command, and stops for good the moment
// it succeeds or gives up. It never re-arms: a device that could silently
// re-zero itself mid-service would turn a full bowl into an empty reading, and
// that failure is far worse than the inconvenience it would be saving.
// Called every pass of the scale task. Does nothing while a cell is answering.
void serviceLink(uint32_t now) {
  uint8_t online = 0;
  for (uint8_t i = 0; i < CELLS; i++)
    if (cell_[i].state() == CellState::Online) online++;

  if (online > 0) {
    everOnline_ = true;
    linkLostAnnounced_ = false;
    // ONE CELL DOWN IS DELIBERATELY NOT RETRIED, and it is a different fault
    // from the cable being out. The whole lead going dark is a connector
    // somebody can push back in; a single converter dropping while the other
    // two answer is wiring, and wiring is not something the firmware should
    // paper over.
    //
    // An automatic retry was written here and then removed on exactly that
    // reasoning: it would revive an INTERMITTENT cell over and over, and a
    // corner that keeps coming and going produces plausible weights that are
    // quietly wrong, which is worse for a scale than a corner that is plainly
    // dead. As it stands the station degrades honestly -- weight_state
    // 'cells_partial', weight_g null, "at least, kg" and a red "N cell(s) down"
    // on the panel -- and stays that way until somebody looks at the harness.
    // That is the intended behaviour, not a gap.
    return;
  }

  if (!linkLostAnnounced_) {
    linkLostAnnounced_ = true;
    Serial.println(
        everOnline_
            ? "scale: every cell went silent -- I2C cable unplugged? probing 1 Hz"
            : "scale: no cells at boot -- CONNECT THE USB-C CABLE TO THE LOAD "
              "CELL HOUSING. Probing 1 Hz; the device will restart when it "
              "appears.");
  }

  if ((int32_t)(now - nextLinkProbeMs_) < 0) return;
  nextLinkProbeMs_ = now + LINK_PROBE_MS;

  // The mux is the whole cable's proxy: it sits in the load-cell housing at the
  // far end of the lead, so it cannot answer unless 3V3, GND, SDA and SCL are
  // all through. Probing a converter instead would need a channel selected
  // first and would report the same thing more slowly.
  i2cmux::invalidate();
  if (!i2cmux::begin()) return;

  // THE CONVERTERS ARE ASKED BEFORE THE MUX ANSWERING IS ALLOWED TO MEAN
  // ANYTHING, and the order is the whole fix. This block used to sit BELOW the
  // restart below it, which made the restart fire on the mux alone:
  //
  //   boot, cable in, one cell's connector loose
  //     -> mux answers, everOnline_ false            -> esp_restart()
  //     -> boot again, cells still do not come up    -> mux answers
  //     -> esp_restart() ... every 1-2 s, for ever
  //
  // No UI, no telemetry, and nothing on the console but the same line scrolling
  // -- on a unit whose whole cable is hot-pluggable by design. Worse, the
  // "mux answers but no converter does" message right below was UNREACHABLE at
  // boot, which is precisely the moment it was written to fire: !everOnline_
  // short-circuited to the restart first, so the one fault it names could never
  // announce itself.
  uint8_t up = 0;
  for (uint8_t i = 0; i < CELLS; i++)
    if (cell_[i].begin()) up++;
  if (up == 0) {
    // The mux answered and no converter did. The lead is in but something
    // downstream of the mux is not, which is a different fault from a missing
    // cable and is worth saying so rather than looping silently.
    Serial.println("scale: mux answers but no converter does -- check the cells "
                   "behind it, not the cable");
    return;
  }

  if (!everOnline_) {
    // Nothing to preserve, and an un-tared scale is worse than a restart. Safe
    // to restart now and not before: at least one converter has answered, so
    // the boot this triggers has something to find.
    Serial.println("scale: cable detected -- restarting so the normal boot runs "
                   "(including auto-tare)");
    Serial.flush();
    delay(50);  // let the line reach the console before the reset takes it
    esp_restart();
  }

  // TARE DELIBERATELY UNTOUCHED. See everOnline_ above.
  Serial.printf("scale: I2C link restored -- %u/%u converters re-initialised, "
                "tare kept\n", up, CELLS);
  linkLostAnnounced_ = false;
}

void serviceAutoTare(uint32_t now) {
  if (autoTare_ == AutoTare::Done || autoTare_ == AutoTare::GaveUp ||
      autoTare_ == AutoTare::Off) {
    return;
  }
  if (!autoBootMs_) autoBootMs_ = now;

  // Nothing can be judged until every cell is converting AND its filter is
  // full. Before that the mean is still climbing out of its own start-up and
  // would look stable while being wrong.
  uint8_t online = 0;
  for (uint8_t i = 0; i < CELLS; i++)
    if (cell_[i].state() == CellState::Online) online++;
  if (online < CELLS || !windowsFull()) {
    autoTare_ = AutoTare::Waiting;
    autoStartMs_ = 0;
    // The deadline runs from BOOT, not from the first stable-looking moment, so
    // a board with a dead cell gives up rather than waiting for ever.
    if ((uint32_t)(now - autoBootMs_) > AUTOTARE_DEADLINE_MS) {
      autoTare_ = AutoTare::GaveUp;
      Serial.println("scale: auto-tare gave up -- cells never both settled. Tare by hand.");
    }
    return;
  }

  const int32_t band = autoTareBandCounts();
  int32_t m[CELLS];
  for (uint8_t i = 0; i < CELLS; i++) m[i] = window_[i].mean(windowTrim());

  // Start, or restart, the observation.
  if (autoTare_ != AutoTare::Observing) {
    autoTare_ = AutoTare::Observing;
    autoStartMs_ = now;
    for (uint8_t i = 0; i < CELLS; i++) autoLo_[i] = autoHi_[i] = m[i];
    return;
  }

  // PEAK-TO-PEAK ACROSS THE WHOLE OBSERVATION, not sample-to-sample. A load
  // cell settles asymptotically, so consecutive samples during a slow creep are
  // always close to each other while the reading as a whole is still moving.
  // Only the span over time catches that.
  bool steady = true;
  for (uint8_t i = 0; i < CELLS; i++) {
    if (m[i] < autoLo_[i]) autoLo_[i] = m[i];
    if (m[i] > autoHi_[i]) autoHi_[i] = m[i];
    if (autoHi_[i] - autoLo_[i] > band) steady = false;
  }

  if (!steady) {
    // Begin again from here rather than abandoning: whatever disturbed it may
    // have been someone putting the pot down, and the next three seconds are
    // exactly when it will settle.
    autoStartMs_ = now;
    for (uint8_t i = 0; i < CELLS; i++) autoLo_[i] = autoHi_[i] = m[i];
    if ((uint32_t)(now - autoBootMs_) > AUTOTARE_DEADLINE_MS) {
      autoTare_ = AutoTare::GaveUp;
      Serial.println("scale: auto-tare gave up -- platform never held still. Tare by hand.");
    }
    return;
  }

  if ((uint32_t)(now - autoStartMs_) < AUTOTARE_STABLE_MS) return;

  for (uint8_t i = 0; i < CELLS; i++) {
    tare_[i] = m[i] - platformZero_[i];
    tareSet_[i] = true;
  }
  autoTare_ = AutoTare::Done;
  char tares[JOIN_BUF];
  Serial.printf("scale: auto-tared at %s above the platform (steady %lu ms)\n",
                joinCells(tares, sizeof(tares), tare_), (unsigned long)AUTOTARE_STABLE_MS);
}

void scaleTask(void *) {
  // --- converter bring-up, moved off setup() -------------------------------
  // See the note where begin() used to do this. It runs here, first, before a
  // single poll, so the invariant that one task owns the converters holds from
  // the first register write.
  uint8_t up = 0;
  for (uint8_t i = 0; i < CELLS; i++) {
    if (cell_[i].begin()) up++;
  }
  Serial.printf("  %u/%u converters answering at 0x%02X\n", up, CELLS, board::NAU7802_ADDR);
  if (up == 0) {
    // Expected on a board with nothing wired, and said so rather than left to
    // read as a fault -- the same call the ToF bring-up makes. If the mux was
    // also missing, begin() already said so and this is its consequence.
    Serial.println("  none responded - expected with no cells attached.");
  }

#if BOWLSTACK_CELL_SELFTEST
  // THE CHEAP HALF ONLY: bridge versus internally-shorted inputs, on four
  // samples, and no channel-2 pass. That pair is what answers "is this cell
  // alive" -- an open input barely moves when the PGA is shorted because it
  // already looks like a short, while a live bridge moves by six figures. The
  // channel-2 pass answers a different, one-off question (did the bridge land on
  // VIN2) and costs more than everything else here put together, so it lives
  // behind the console's 's' instead.
  //
  // BOOT TIME IS THE WHOLE REASON THIS IS SPLIT. At 10 SPS every sample is
  // 100 ms of wall clock, and the full test measured 5.4 s per cell -- 16 s of a
  // 20 s boot, on a device somebody switches on in a kitchen.
  if (up > 0) {
    Serial.println("\n  self-test -- bridge vs internally-shorted inputs:");
    for (uint8_t i = 0; i < CELLS; i++) cell_[i].selfTest(4, false);
    Serial.println(
        "  A BRIDGE READING NEAR ZERO THAT BARELY MOVES WHEN SHORTED IS AN INPUT THAT IS\n"
        "  NOT THERE. A live 350 ohm cell at gain 128 sits tens of thousands of counts\n"
        "  off zero and shifts by as much again when the PGA is shorted; a delta of a\n"
        "  few hundred is an open bridge, a dead excitation, or a cell on VIN2.\n"
        "  Press 's' on the console for the full-depth test, including channel 2.");
  }
#endif

  uint32_t nextPublishMs = 0;
  for (;;) {
    const uint32_t now = millis();

    for (uint8_t i = 0; i < CELLS; i++) {
      if (cell_[i].poll(now)) {
        const int32_t v = cell_[i].counts();

        // FROM FOUR SAMPLES, NOT FROM A FULL WINDOW, and that distinction is
        // the whole bug this replaced.
        //
        // Detecting a step CLEARS the window. Gating detection on the window
        // being full therefore switched the detector off for a whole window
        // immediately after every step it caught -- 12.8 s at N=128. So putting
        // a bowl down was caught and taking it off again a few seconds later
        // was not, and the reading crawled back over the refill exactly as if
        // no detector existed. The first window after boot was blind for the
        // same reason.
        //
        // Four is the floor at which a mean means anything. Below it the "mean"
        // is essentially the previous sample and every reading looks like a
        // step against it.
        if (window_[i].size() >= STEP_MIN_SAMPLES) {
          const int32_t m = window_[i].mean(windowTrim());
          const int32_t d = (v > m) ? (v - m) : (m - v);
          if (d > stepCounts()) {
            // Kept so the restart can be seeded with them rather than with one
            // raw sample -- see below.
            if (stepRun_[i] < STEP_CONFIRM) stepBuf_[i][stepRun_[i]] = v;
            if (++stepRun_[i] >= STEP_CONFIRM) {
              // THE WINDOW IS THROWN AWAY, not blended out of. Its contents
              // describe a load that is gone; averaging them against the new
              // one is what produced the long crawl.
              window_[i].clear();
              // SEEDED WITH THE THREE SAMPLES THAT TRIGGERED IT, not left empty
              // for the next push alone. All three are already measurements of
              // the NEW load, so using them makes the value the reading jumps to
              // a three-sample mean rather than one raw conversion -- about
              // 1.7x quieter at the instant somebody is looking hardest.
              for (uint8_t k = 0; k < STEP_CONFIRM; k++)
                window_[i].push(stepBuf_[i][k], window_n_);
              stepRun_[i] = 0;
              // RATE LIMITED, because a slow pour is a continuous step. Liquid
              // going into a bowl crosses the threshold every three samples for
              // as long as the pouring lasts, and an unthrottled line would put
              // three messages a second on a console somebody is trying to read
              // the weight from. One a second says the same thing.
              static uint32_t nextStepSayMs[CELLS] = {0, 0, 0};
              if ((int32_t)(now - nextStepSayMs[i]) >= 0) {
                nextStepSayMs[i] = now + 1000;
                Serial.printf("scale: cell %c step %+ld counts -- filter restarted\n",
                              'A' + i, (long)(v - m));
              }
            }
          } else {
            // A run has to be CONSECUTIVE. One sample back inside the band means
            // the excursion was noise, and the count starts again.
            stepRun_[i] = 0;
          }
        }

        window_[i].push(v, window_n_);
      }
      // A cell that dropped offline must not keep a window that still averages
      // to a plausible weight. Cleared here rather than in publish(), so the
      // sample count on the diagnostics page tells the truth about what is
      // actually in the filter.
      if (cell_[i].state() == CellState::Offline) window_[i].clear();
    }

    // BEFORE the auto-tare service, so a restored link is seen as online on
    // the same pass rather than counting as one more "cells not ready" tick
    // against the auto-tare deadline.
    serviceLink(now);
    serviceAutoTare(now);
    serviceCommands();

    // 10 Hz, matching the converters. That is the fastest readout this product
    // wants, and publishing faster than samples arrive would republish the same
    // averaged value while costing the UI task a mutex it would rather not
    // contend for.
    if ((int32_t)(now - nextPublishMs) >= 0) {
      nextPublishMs = now + PUBLISH_MS;
      publish();
    }

    // 10 ms. At 10 SPS a conversion lands every 100 ms, so this still checks
    // each cell ten times per sample -- enough that a completed conversion is
    // picked up promptly, which is what keeps the MEASURED rate equal to the
    // configured one rather than some beat frequency between the two.
    //
    // It was 2 ms when the converters ran at 80 SPS. Five times fewer polls is
    // five times less I2C traffic on both buses, and the bit-banged one costs
    // real CPU per transaction -- it busy-waits through delayMicroseconds at
    // priority 3, above the UI.
    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
  }
}

}  // namespace

// Reports what acknowledges, and -- just as importantly -- separates "nothing is
// there" from "the bus is stuck". A pair of lines held low by a wedged slave
// looks identical to an empty bus through a scan alone, so the idle levels are
// read as plain GPIO first, before any transaction is attempted.
//
// This is the ToF bring-up harness's scanBus() brought back. The failure it was
// written for used to be this branch's too -- cell A shared GPIO47/48 with the
// touch controller, so a dead cell and a dead screen were one fault wearing two
// faces. The mux ended that particular version of it; what stays true is that a
// stub holding SDA low still kills its channel, and a scan is the only thing
// that tells you which.
//
// Returns the number of devices that acknowledged.
uint8_t scanBus(int port, const char *name, int sda, int scl, uint32_t hz) {
  Serial.printf("  %s (SDA=%d SCL=%d, i2c port %d)\n", name, sda, scl, port);

  uint8_t found = 0;
  for (uint8_t addr = 0x08; addr < 0x78; addr++) {
    uint8_t b = 0;
    if (!lgfx::i2c::transactionRead(port, addr, &b, 1, hz).has_value()) continue;
    const char *known = "";
    if (addr == 0x15) known = "  <- CST816D touch";
    else if (addr == 0x6A || addr == 0x6B) known = "  <- QMI8658 IMU";
    else if (addr == board::MUX_ADDR) known = "  <- TCA9548A mux";
    else if (addr == board::NAU7802_ADDR) known = "  <- NAU7802 load cell";
    Serial.printf("    0x%02X ack%s\n", addr, known);
    found++;
  }
  if (found == 0) Serial.println("    (nothing acknowledged)");
  return found;
}

// Walks the channels the cells are wired to and says what is behind each.
//
// THE TRUNK IS SCANNED WITH EVERY CHANNEL CLOSED FIRST, which is what makes the
// per-channel answers mean anything: if 0x2A shows up on the trunk with the
// switch shut, a converter is wired straight to GPIO12/11 past the mux, and
// every channel would then appear to contain a cell whether it did or not.
//
// A channel that reports nothing is a channel with nothing on it OR one whose
// stub has no pull-ups -- those two look identical from here, which is why the
// pull-up rule is stated at every point it could bite.
void scanMuxChannels() {
  Serial.println("\n  mux channels:");
  for (uint8_t i = 0; i < CELLS; i++) {
    const int8_t ch = board::CELL_MUX_CH[i];
    if (!i2cmux::select(ch)) {
      Serial.printf("    ch %d (cell %c)  -- SELECT FAILED, the mux is not answering\n", ch,
                    (char)('A' + i));
      continue;
    }
    uint8_t b = 0;
    const bool cell =
        lgfx::i2c::transactionRead(CELL_PORT, board::NAU7802_ADDR, &b, 1, CELL_HZ).has_value();
    Serial.printf("    ch %d (cell %c)  %s\n", ch, (char)('A' + i),
                  cell ? "0x2A acknowledged" : "nothing -- check wiring and the stub's 4.7k");
  }
  // CLOSED AGAIN before anything else touches the bus. Leaving the last channel
  // open would work by luck: the first cell to be configured would select its
  // own channel anyway, but a scan is a diagnostic and a diagnostic that leaves
  // state behind is a source of the next bug.
  i2cmux::select(-1);
}

void begin() {
  mutex_ = xSemaphoreCreateMutex();
  published_ = Snapshot{};
  // WARMING, NOT OFFLINE, for the window before the first publish. A
  // zero-initialised Snapshot puts every cell in Offline -- enum value 0 -- and
  // the dashboard renders that as "no cell talking" in fault red. That window
  // used to be a few milliseconds; the converters are now brought up on the
  // scale task rather than here, so it is over a second, and a red fault flag
  // on every single boot is a claim this firmware has no business making.
  //
  // Warming is the accurate word and the vocabulary already exists for it: the
  // channel scan a few lines below proves a converter answers on each channel,
  // so what is unknown is not whether the cell is there but whether it has
  // concluded anything yet.
  for (uint8_t i = 0; i < CELLS; i++) published_.cell[i].state = CellState::Warming;

  Serial.println("\n--- load cells ---");

  // The cell bus has no peripheral behind it until now -- unlike the old cell A,
  // which rode a port LovyanGFX had already opened and therefore had to be left
  // alone. Port 1 is nobody else's, so this is the one and only init of it.
  const bool busOk =
      lgfx::i2c::init(CELL_PORT, board::CELL_SDA, board::CELL_SCL).has_value();
  Serial.printf("  cell bus  i2c port %d (hardware)  SDA=%d SCL=%d  %s\n", CELL_PORT,
                board::CELL_SDA, board::CELL_SCL, busOk ? "" : "-- INIT FAILED");

  i2cmux::configure(CELL_PORT, board::MUX_ADDR, CELL_HZ);
  const bool muxOk = i2cmux::begin();
  Serial.printf("  mux       TCA9548A at 0x%02X  %s\n", board::MUX_ADDR,
                muxOk ? "answering, all channels closed" : "-- NOT ANSWERING");
  if (!muxOk) {
    // THE ONE FAILURE THAT EXPLAINS EVERY OTHER ONE. With no mux there is no
    // path to any converter, so all three will report Offline and the obvious
    // reading of that is three dead cells. Said once, plainly, at the top.
    Serial.println("  !! NO MUX, THEREFORE NO CELLS. Every cell below will read Offline and");
    Serial.println("     that is a consequence, not three separate faults. Check 3V3 and GND");
    Serial.println("     at the module, A0/A1/A2 strapped to GND, and !RESET pulled to 3V3 --");
    Serial.println("     a floating !RESET holds the switch open and looks exactly like this.");
  }

  Serial.println("\n  bus scan:");
  scanBus(0, "port 0 -- touch + IMU (no cells here any more)", board::TP_SDA, board::TP_SCL,
          400000);
  Serial.println("    expect 0x15 (touch) and 0x6A or 0x6B (IMU), and NOTHING else.");
  Serial.println("    0x15 MISSING means that bus is down, and the SCREEN goes with it --");
  Serial.println("    every failed touch read costs ~13 ms of I2C timeout inside the");
  Serial.println("    render loop, which shows up as a collapsed frame rate.");
  scanBus(CELL_PORT, "cell trunk -- mux only, channels closed", board::CELL_SDA,
          board::CELL_SCL, CELL_HZ);
  Serial.printf("    expect 0x%02X and nothing else. A 0x%02X HERE means a converter is\n",
                board::MUX_ADDR, board::NAU7802_ADDR);
  Serial.println("    wired to the trunk rather than behind a channel.");
  scanMuxChannels();

  Serial.println();
  for (uint8_t i = 0; i < CELLS; i++) {
    // Names live in a table rather than being built per call site, because the
    // console lines, the Device page and the scope legend all have to agree
    // about which corner is which and there is no second place to be wrong.
    cell_[i].configure(CELL_PORT, CELL_HZ, CELL_NAME[i], board::CELL_MUX_CH[i]);
  }

  // THE CONVERTERS ARE BROUGHT UP ON THE TASK, NOT HERE, and that is a boot-time
  // decision with a measurement behind it. Each Nau7802::begin() resets the
  // part, waits for its power-up-ready flag and runs its internal offset
  // calibration; at 10 SPS that measured ~414 ms, and three of them is 1.24 s of
  // setup() blocking with a splash on the screen.
  //
  // Nothing in setup() needs them up. The channel scan above has already proved
  // a converter answers on each channel -- which is the diagnostic worth having
  // at boot -- and everything downstream reads through the snapshot, which
  // reports Warming until they conclude. So the wait buys nothing except a
  // later dashboard.
  //
  // It stays a BLOCKING sequence, just on the other side of the task boundary:
  // one task owns the converters, and moving their bring-up anywhere else would
  // put two tasks on the same registers. See scaleTask().

#if !BOWLSTACK_CELL_SELFTEST
  // SAID OUT LOUD, because a diagnostic that exists and is switched off is worse
  // than one that does not exist: the first time somebody needs it they have to
  // find out it is there. One line at boot is the whole cost of that.
  Serial.println("  self-test skipped at boot -- press 's' on the console to run it");
#endif

  loadPersisted();

  // Core 1, priority 3: the measurement core, above the UI and above anything
  // the radio does. docs/firmware.md's whole argument is that a network stall
  // must not be able to delay a measurement, and this is where that is enforced.
  xTaskCreatePinnedToCore(scaleTask, "scale", 4096, nullptr, 3, &task_, 1);
  Serial.println("  task started on core 1");
}

Snapshot snapshot() {
  Snapshot s{};
  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    s = published_;
    xSemaphoreGive(mutex_);
  }
  return s;
}

bool ready() {
  bool r = false;
  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    r = ready_;
    xSemaphoreGive(mutex_);
  }
  return r;
}

void tare() { wantTare_ = true; }

void setPlatformZero() { wantPlatformZero_ = true; }

void requestSelfTest() { wantSelfTest_ = true; }

float vesselOffsetG() { return vesselOffsetG_; }

// off -> 2.0 -> 2.5 -> 3.0 -> 3.5 -> off. Persisted immediately: this is a
// property of the crockery, not of the session, and re-entering it after every
// power cycle is exactly the kind of chore that gets skipped.
float setVesselOffset(float grams) {
  // QUEUED, NOT APPLIED, and this is the whole reason the flag exists. Writing
  // NVS here would write it from the UI task while the scale task is using the
  // same Preferences object -- see the note above wantWindow_: not reentrant,
  // not guarded, and the failure is a CORRUPT NAMESPACE rather than a wrong
  // reading. A unit that forgets its calibration factor and its platform zeros
  // reports weight_state 'untared' and sends no mass at all, which is exactly
  // the blackout the platformZeroed change was made to end.
  //
  // The scale task runs at priority 3 on the same core as loop()'s priority 1,
  // so it preempts mid-write; this is a real race, not a theoretical one.
  //
  // Bounded at 65 kg by the uint16 that carries it, which is far above any
  // serving vessel and far below anything three 20 kg cells could hold anyway.
  if (grams < 0.0f) grams = 0.0f;
  if (grams > 60000.0f) grams = 60000.0f;
  wantVessel_ = (uint16_t)(grams + 0.5f) + 1;  // +1 so a pending 0 is a request
  return grams;
}

AutoTare autoTareState() { return autoTare_; }

const char *autoTareText(AutoTare s) {
  switch (s) {
    case AutoTare::Waiting: return "settling";
    case AutoTare::Observing: return "auto-taring";
    case AutoTare::Done: return "";
    case AutoTare::GaveUp: return "not tared";
    case AutoTare::Off: return "";
  }
  return "";
}

void tareCell(uint8_t index) {
  if (index >= CELLS) return;
  wantTareMask_ = (uint8_t)(wantTareMask_ | (1u << index));
}

uint8_t window() { return window_n_; }

float calMass() { return calMass_; }

uint8_t windowTrim() {
  const uint8_t t = (uint8_t)(window_n_ / 32);
  return t ? t : (uint8_t)1;
}

void setWindow(uint8_t n) {
  // Snapped to the nearest allowed choice rather than accepted verbatim. The
  // menu and the console can only produce values from the list, but a future
  // caller -- a config file, a provisioning script -- should not be able to put
  // the filter somewhere the UI cannot then describe.
  uint8_t best = WINDOW_CHOICES[0];
  uint16_t bestErr = 0xFFFF;
  for (uint8_t i = 0; i < WINDOW_CHOICE_COUNT; i++) {
    const int16_t d = (int16_t)WINDOW_CHOICES[i] - (int16_t)n;
    const uint16_t err = (uint16_t)(d < 0 ? -d : d);
    if (err < bestErr) {
      bestErr = err;
      best = WINDOW_CHOICES[i];
    }
  }
  wantWindow_ = best;
}

uint8_t decimals() { return decimals_; }

bool showCells() { return showCells_; }

bool toggleShowCells() {
  // Reads the PENDING value if one is queued, so two quick taps land back where
  // they started rather than both toggling off the same value and appearing to
  // do nothing. Same reasoning as cycleDecimals(), and the same bug without it.
  const bool from = wantShowCells_ ? (wantShowCells_ == 2) : showCells_;
  const bool next = !from;
  wantShowCells_ = next ? 2 : 1;
  return next;
}

void setDecimals(uint8_t d) {
  if (d < DECIMAL_CHOICES[0]) d = DECIMAL_CHOICES[0];
  if (d > DECIMAL_CHOICES[DECIMAL_CHOICE_COUNT - 1])
    d = DECIMAL_CHOICES[DECIMAL_CHOICE_COUNT - 1];
  wantDecimals_ = d;
}

uint8_t cycleDecimals() {
  // Reads the PENDING value if one is queued, so two quick taps step twice
  // rather than both stepping off the same starting point -- the same reason
  // cycleWindow() does, and the same bug if it did not: on a panel this slow to
  // register a tap, a double tap is a normal thing for a person to do.
  const uint8_t from = wantDecimals_ ? wantDecimals_ : decimals_;
  const uint8_t next =
      (from >= DECIMAL_CHOICES[DECIMAL_CHOICE_COUNT - 1]) ? DECIMAL_CHOICES[0] : (uint8_t)(from + 1);
  wantDecimals_ = next;
  return next;
}

uint8_t cycleWindow() {
  // Reads the PENDING value if one is queued, so two quick taps step twice
  // rather than both stepping off the same starting point.
  const uint8_t from = wantWindow_ ? wantWindow_ : window_n_;
  for (uint8_t i = 0; i < WINDOW_CHOICE_COUNT; i++) {
    if (WINDOW_CHOICES[i] != from) continue;
    const uint8_t next = WINDOW_CHOICES[(i + 1) % WINDOW_CHOICE_COUNT];
    wantWindow_ = next;
    return next;
  }
  wantWindow_ = WINDOW_CHOICES[0];
  return WINDOW_CHOICES[0];
}

const char *calResultText(CalResult r) {
  switch (r) {
    case CalResult::Ok: return "ok";
    case CalResult::Timeout: return "no answer from the scale";
    case CalResult::NotTared: return "tare every cell first";
    case CalResult::CellsOffline: return "a cell is not converting";
    case CalResult::MassTooSmall: return "mass too small to calibrate";
    case CalResult::NoDeflection: return "the platform did not move";
    case CalResult::Implausible: return "factor way off -- check the mass";
    case CalResult::Settling: return "still settling -- wait for the filter";
  }
  return "?";
}

CalResult calibrate(float knownGrams, float *factorOut) {
  if (factorOut) *factorOut = 0.0f;
  wantCalGrams_ = knownGrams;
  calDone_ = false;

  // THREE SECONDS, not the half a second this started at, and the reason is
  // what happens inside the acknowledged window. The task takes the request,
  // may first service a pending tare (an NVS write), prints several lines to a
  // USB-CDC endpoint that BLOCKS when no host is draining it, then commits the
  // factor to NVS -- all before it can answer. Half a second was inside that
  // envelope, so a perfectly good calibration could report failure while
  // succeeding, which is the worst of the possible outcomes.
  //
  // Three seconds cannot be reached by any of that. Reaching it means the task
  // is genuinely stuck, in which case nothing on this device is working and
  // saying so is right.
  const uint32_t deadline = millis() + 3000;
  while (!calDone_ && (int32_t)(millis() - deadline) < 0) delay(5);

  if (!calDone_) return CalResult::Timeout;
  if (factorOut) *factorOut = calFactor_;
  return (CalResult)calCode_;
}

void clearCalibration() { wantClearCal_ = true; }

void restoreDefault() { wantRestore_ = true; }

float defaultCountsPerGram() { return (float)BOWLSTACK_COUNTS_PER_GRAM; }

uint32_t stackFreeBytes() { return task_ ? uxTaskGetStackHighWaterMark(task_) : 0; }

}  // namespace scale
