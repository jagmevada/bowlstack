#include "scale.h"

#include <Arduino.h>
#include <Preferences.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "board_waveshare_s3.h"

// ON BY DEFAULT, because this branch is bring-up and the self-test answers the
// first question anybody asks of a new assembly: is that cell wired. It costs
// roughly a second of boot per cell -- two channel switches, each needing its
// own offset calibration -- so turn it off with -DBOWLSTACK_CELL_SELFTEST=0
// once the wiring is settled and the boot time starts to matter.
#ifndef BOWLSTACK_CELL_SELFTEST
#define BOWLSTACK_CELL_SELFTEST 1
#endif

namespace scale {
namespace {

// --- bus assignment --------------------------------------------------------
// Both numbers are lgfx port identifiers, and the sign is the whole difference:
// a non-negative one is a hardware peripheral, a negative one is a bit-banged
// slot. See nau7802.h for why this is the abstraction rather than TwoWire.
//
// Cell A rides port 0, the touch controller's own bus, and is NOT initialised
// here -- LovyanGFX did that when the display started, and a second init would
// tear down a working bus under a live touch driver.
const int PORT_A = 0;
const int PORT_B = -1;  // bit-banged slot 0

// 400 kHz on the hardware bus, matching what the touch controller already runs
// at. There is no gain in going slower and the part is rated for it.
const uint32_t HZ_A = 400000;

// 100 kHz on the bit-banged bus, and this is a decision rather than a default.
//
// GPIO11/12 have no pull-ups fitted, so the driver falls back on the ESP32's
// internal ones at roughly 45 kohm. Against a few tens of picofarads of trace
// and lead that is a rise time of several microseconds -- comfortably inside a
// 10 us half-period at 100 kHz, and NOT inside a 1.25 us one at 400 kHz. Asking
// for 400 kHz on this bus does not fail cleanly; it produces occasional NAKs
// that read as a flaky converter.
//
// Fit 4.7k to 3V3 on both lines and this can go to 400000.
const uint32_t HZ_B = 100000;

// --- persistence -----------------------------------------------------------
// NVS rather than a build flag, because tare and calibration belong to the
// ASSEMBLY and not to the firmware image: reflashing a unit must not silently
// throw away the gram factor someone derived with a known mass.
const char *NVS_NS = "bowlscale";
const char *KEY_OFF_A = "offA";
const char *KEY_OFF_B = "offB";
const char *KEY_CPG = "cpg";
const char *KEY_TARED_A = "tarA";
const char *KEY_TARED_B = "tarB";
const char *KEY_WINDOW = "win";
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
int32_t offset_[CELLS] = {0, 0};
float countsPerGram_ = 0.0f;
// PER CELL, not one flag for the assembly. The old single flag was set at the
// end of a block that handles both a global tare and a per-cell one -- so
// taring cell A alone marked the whole platform tared, the "not tared" chip
// disappeared, and a calibration taken then absorbed cell B's full absolute
// reading into the factor. It also survived a reboot, so a global tare taken
// while one cell was offline came back claiming both were zeroed.
bool cellTared_[CELLS] = {false, false};
uint8_t window_n_ = BOWLSTACK_AVG_WINDOW;
// Consecutive samples outside the step band, per cell.
uint8_t stepRun_[CELLS] = {0, 0};
float calMass_ = (float)BOWLSTACK_CAL_MASS_G;
volatile uint8_t wantWindow_ = 0;  // 0 = no change pending

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

bool windowsFull() {
  for (uint8_t i = 0; i < CELLS; i++) {
    if (cell_[i].state() != CellState::Online) continue;
    if (window_[i].size() < window_n_) return false;
  }
  return true;
}

bool allCellsTared() {
  for (uint8_t i = 0; i < CELLS; i++)
    if (!cellTared_[i]) return false;
  return true;
}

void loadPersisted() {
  prefs_.begin(NVS_NS, false);
  offset_[0] = prefs_.getInt(KEY_OFF_A, 0);
  offset_[1] = prefs_.getInt(KEY_OFF_B, 0);
  // The build-time default is the fallback, so a freshly flashed board reads
  // kilograms straight away instead of counts. NVS still wins: a unit that has
  // been calibrated against its own mass keeps that figure across reflashes,
  // which is the whole reason the factor lives in NVS rather than in the image.
  countsPerGram_ = prefs_.getFloat(KEY_CPG, BOWLSTACK_COUNTS_PER_GRAM);
  // Stored as its own flag rather than inferred from a non-zero offset. The
  // inference is almost always right and wrong exactly once -- a platform that
  // happened to tare at 0 counts would come back from a reboot claiming it had
  // never been tared.
  cellTared_[0] = prefs_.getBool(KEY_TARED_A, false);
  cellTared_[1] = prefs_.getBool(KEY_TARED_B, false);

  // AN UNTARED CELL HAS NO OFFSET, and the two are made to agree here rather
  // than left to drift apart. They can disagree for a real reason -- this
  // firmware replaced a single assembly-wide `tared` key with one per cell, so
  // the first boot after the update finds offsets stored and no flag vouching
  // for them -- and the result was a display quietly subtracting a tare while
  // the chip said "not tared". Believing the flag and dropping the offset is
  // the safe direction: it shows the platform's true absolute reading, which is
  // obviously not zero, instead of a plausible number nothing stands behind.
  for (uint8_t i = 0; i < CELLS; i++) {
    if (!cellTared_[i] && offset_[i] != 0) {
      Serial.printf("  cell %c: offset %ld discarded -- nothing recorded taring it\n",
                    'A' + i, (long)offset_[i]);
      offset_[i] = 0;
    }
  }
  window_n_ = prefs_.getUChar(KEY_WINDOW, (uint8_t)BOWLSTACK_AVG_WINDOW);
  if (window_n_ < 4 || window_n_ > WINDOW_MAX) window_n_ = BOWLSTACK_AVG_WINDOW;
  calMass_ = prefs_.getFloat(KEY_CALMASS, (float)BOWLSTACK_CAL_MASS_G);
  if (calMass_ < MIN_CAL_GRAMS) calMass_ = (float)BOWLSTACK_CAL_MASS_G;
  prefs_.end();

  if (countsPerGram_ > 0.0f) {
    Serial.printf("  %.3f counts/g, tare %ld / %ld, window %u samples\n", countsPerGram_,
                  (long)offset_[0], (long)offset_[1], window_n_);
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
  prefs_.begin(NVS_NS, false);
  prefs_.putInt(KEY_OFF_A, offset_[0]);
  prefs_.putInt(KEY_OFF_B, offset_[1]);
  prefs_.putBool(KEY_TARED_A, cellTared_[0]);
  prefs_.putBool(KEY_TARED_B, cellTared_[1]);
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
  s.calMassG = calMass_;
  s.countsPerGram = countsPerGram_;
  s.calibrated = countsPerGram_ > 0.0f;
  s.tared = allCellsTared();

  int32_t sumNet = 0;
  for (uint8_t i = 0; i < CELLS; i++) {
    CellSnapshot &c = s.cell[i];
    c.state = cell_[i].state();
    c.revision = cell_[i].revision();
    c.rawCounts = cell_[i].counts();
    c.counts = window_[i].mean(windowTrim());
    c.samples = window_[i].size();
    c.pp = window_[i].pp();
    // Tested on the RAW conversion, not the filtered mean. A trimmed average of
    // saturated samples is still saturated, but it lags -- and the point of this
    // flag is to fire the moment the part stops measuring.
    c.overRange = (c.rawCounts > OVER_RANGE_COUNTS) || (c.rawCounts < -OVER_RANGE_COUNTS);
    if (c.overRange) s.overRange = true;
    c.sps = cell_[i].sps();
    c.offset = offset_[i];

    const int32_t net = c.counts - offset_[i];
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
  s.totalGrams = s.calibrated ? (float)sumNet / countsPerGram_ : 0.0f;
  s.seq = published_.seq + 1;

  if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) return;
  published_ = s;
  ready_ = true;
  xSemaphoreGive(mutex_);
}

void serviceCommands() {
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
      Serial.printf("scale: averaging %u samples (~%u ms at 79 SPS), trim %u\n", window_n_,
                    (unsigned)((window_n_ * 1000UL) / 79UL), windowTrim());
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
        offset_[i] = window_[i].mean(windowTrim());
        cellTared_[i] = true;
      }
    }
    storeOffsets();
    // Which cells were zeroed, not just that a tare happened. With two buttons
    // on the dashboard the interesting part of the log line is WHICH one was
    // pressed.
    Serial.printf("scale: tared %c%c at %ld / %ld counts\n", (mask & 1) ? 'A' : '-',
                  (mask & 2) ? 'B' : '-', (long)offset_[0], (long)offset_[1]);
  }

  if (!calDone_) {
    const float known = wantCalGrams_;
    CalResult r = CalResult::Ok;
    float derived = 0.0f;

    int32_t sumNet = 0;
    uint8_t online = 0;
    for (uint8_t i = 0; i < CELLS; i++) {
      if (cell_[i].state() != CellState::Online) continue;
      sumNet += window_[i].mean(windowTrim()) - offset_[i];
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
      // the band moves with it. An order of magnitude either way is far wider
      // than part-to-part spread and still catches every decimal-place slip.
      const float ref = (float)BOWLSTACK_COUNTS_PER_GRAM;
      if (ref > 0.0f && (derived < ref * 0.1f || derived > ref * 10.0f)) {
        r = CalResult::Implausible;
        Serial.printf("scale: %.3f counts/g is outside %.1f..%.1f -- check the mass\n",
                      derived, ref * 0.1f, ref * 10.0f);
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
}

void scaleTask(void *) {
  uint32_t nextPublishMs = 0;
  for (;;) {
    const uint32_t now = millis();

    for (uint8_t i = 0; i < CELLS; i++) {
      if (cell_[i].poll(now)) {
        const int32_t v = cell_[i].counts();

        // Only meaningful once the window is full -- a filling window is
        // already tracking the input as fast as it can, and every early sample
        // would look like a step against a mean built from two others.
        if (window_[i].size() >= window_n_) {
          const int32_t m = window_[i].mean(windowTrim());
          const int32_t d = (v > m) ? (v - m) : (m - v);
          if (d > stepCounts()) {
            if (++stepRun_[i] >= STEP_CONFIRM) {
              // THE WINDOW IS THROWN AWAY, not blended out of. Its contents
              // describe a load that is gone; averaging them against the new
              // one is what produced the long crawl. Pushing v into an empty
              // window makes the mean equal to v, so the reading arrives at the
              // new level on this sample and then re-settles as the window
              // refills at whatever averaging is selected.
              window_[i].clear();
              stepRun_[i] = 0;
              Serial.printf("scale: cell %c step %+ld counts -- filter restarted\n",
                            'A' + i, (long)(v - m));
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
// This is the ToF bring-up harness's scanBus() brought back, because the
// failure it was written for turns out to be the one this branch actually has:
// cell A shares GPIO47/48 with the touch controller, and if that bus goes down
// BOTH stop answering. Without this you see two unrelated-looking symptoms --
// a dead cell and a screen that will not respond -- and no reason to connect
// them.
void scanBus(int port, const char *name, int sda, int scl, uint32_t hz) {
  Serial.printf("  %s (SDA=%d SCL=%d, i2c port %d)\n", name, sda, scl, port);

  uint8_t found = 0;
  for (uint8_t addr = 0x08; addr < 0x78; addr++) {
    uint8_t b = 0;
    if (!lgfx::i2c::transactionRead(port, addr, &b, 1, hz).has_value()) continue;
    const char *known = "";
    if (addr == 0x15) known = "  <- CST816D touch";
    else if (addr == 0x6A || addr == 0x6B) known = "  <- QMI8658 IMU";
    else if (addr == board::NAU7802_ADDR) known = "  <- NAU7802 load cell";
    Serial.printf("    0x%02X ack%s\n", addr, known);
    found++;
  }
  if (found == 0) Serial.println("    (nothing acknowledged)");
}

void begin() {
  mutex_ = xSemaphoreCreateMutex();
  published_ = Snapshot{};

  Serial.println("\n--- load cells ---");

  // Bus A is already open: LovyanGFX initialised i2c port 0 when the touch
  // controller came up. Opening it again here would release and re-create the
  // bus underneath a live driver, for no gain.
  Serial.printf("  bus A  i2c port %d (hardware)  SDA=%d SCL=%d  -- shared with touch+IMU\n",
                PORT_A, board::CELL_A_SDA, board::CELL_A_SCL);

  // Bus B has no peripheral behind it, so it does have to be opened.
  const bool bOk = lgfx::i2c::init(PORT_B, board::CELL_B_SDA, board::CELL_B_SCL).has_value();
  Serial.printf("  bus B  i2c port %d (bit-banged)  SDA=%d SCL=%d  %s\n", PORT_B,
                board::CELL_B_SDA, board::CELL_B_SCL, bOk ? "" : "-- INIT FAILED");
  Serial.println("         no pull-ups on this pair: fit 4.7k to 3V3 on both lines.");

  Serial.println("\n  bus scan:");
  scanBus(PORT_A, "bus A -- touch + IMU + cell A", board::CELL_A_SDA, board::CELL_A_SCL, HZ_A);
  Serial.println("    expect 0x15 (touch), 0x6A or 0x6B (IMU), 0x2A (cell A).");
  Serial.println("    0x15 MISSING means the bus is down, and the SCREEN goes with it --");
  Serial.println("    every failed touch read costs ~13 ms of I2C timeout inside the");
  Serial.println("    render loop, which shows up as a collapsed frame rate.");
  scanBus(PORT_B, "bus B -- cell B", board::CELL_B_SDA, board::CELL_B_SCL, HZ_B);

  cell_[0].configure(PORT_A, HZ_A, "cell A");
  cell_[1].configure(PORT_B, HZ_B, "cell B");

  uint8_t up = 0;
  for (uint8_t i = 0; i < CELLS; i++) {
    if (cell_[i].begin()) up++;
  }
  Serial.printf("  %u/%u converters answering at 0x%02X\n", up, CELLS, board::NAU7802_ADDR);
  if (up == 0) {
    // Expected on a board with nothing wired, and said so rather than left to
    // read as a fault -- the same call the ToF bring-up makes.
    Serial.println("  none responded - expected with no cells attached.");
  }

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
    case CalResult::NotTared: return "tare both cells first";
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
