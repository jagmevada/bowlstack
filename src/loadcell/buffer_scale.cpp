#include "buffer_scale.h"

#include <Arduino.h>
#include <Preferences.h>

// The same bus layer as the counter's converters, for the same reason: one call
// set reaches a hardware peripheral and a bit-banged pair alike. Here it is the
// bit-banged kind. See board_waveshare_s3.h section 10.
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "board_waveshare_s3.h"

// The reference mass the console's calibrate key weighs against, in grams. A
// build constant rather than a typed number, for the counter's reason: calibration
// happens once per assembly, against a mass somebody physically owns. 20 kg is a
// plausible thing to have to hand for a 200 kg cell and is ~10% of its span, which
// is far above the noise and far below anything that stresses the cell. Change it
// to match the mass on your bench.
#ifndef BOWLSTACK_BUF_CAL_MASS_G
#define BOWLSTACK_BUF_CAL_MASS_G 20000
#endif

namespace bufscale {
namespace {

const int PORT = board::BUF_PORT;

// 100 kHz, not the trunk's 400. This pair is software, the lead is a USB-C
// pigtail of unknown length, and the original bit-banged cell only became stable
// at this rate -- 400 kHz produced NAKs that read as a flaky converter. One
// converter at 10 SPS needs nothing faster, so there is nothing to buy with the
// risk.
const uint32_t HZ = 100000;

// NVS: its own namespace, so nothing here can touch the counter's keys, and
// written only from the buffer task. NVS keys are capped at 15 characters.
const char *NVS_NS = "bufscale";
const char *KEY_ZERO = "zero";   // the empty-platform reading, in counts
const char *KEY_ZSET = "zset";   // whether `zero` has ever been set -- 0 is a legal reading
const char *KEY_CPG = "cpg";     // counts per gram; 0 means uncalibrated

// --- the window ---------------------------------------------------------------
// 32 samples = 3.2 s at 10 SPS. Longer than the counter's default of 8 on purpose:
// the counter weighs a bowl being set down and has to follow it, this weighs a
// shelf of stock that changes over minutes, and a steadier number is worth more
// than a quick one. Trimmed by one at each end so a single knock on the shelf is
// kept out of the reading rather than smeared through it.
const uint8_t WINDOW = 32;
const uint8_t TRIM = 1;

// Enough to show a figure at all, and it is THREE because that is what a step
// restart seeds the window with (see addSample). At 8, the reading dropped out of
// kilograms into counts for half a second after every step -- the display would
// have flickered "1.1 kg" -> "2300 cts" -> "1.1 kg" each time something was put on.
// Zeroing and calibrating demand the FULL window instead, because those get stored.
const uint8_t MIN_SAMPLES_SHOWN = 3;

// --- step detection: the counter's answer to a window that crawls ---------------
// A 32-sample trimmed mean is 3.2 s long, so without this a mass put on the shelf
// CRAWLED to its true value over a full window, and taking it off crawled back to
// zero -- the old samples are not noise to average away, they measure a load that
// is gone. The counter solved this in scale.cpp and the reasoning carries over
// unchanged: WATCH FOR A STEP. When consecutive samples land far outside the
// current average that is not noise, it is the load changing, so throw the window
// away and restart from the new level. The reading jumps within ~0.3 s and then
// re-settles at the chosen averaging; quiet when nothing is happening.
//
// WHAT DOES NOT CARRY OVER IS THE THRESHOLD. The counter uses 5 g, which is ~4x
// its own 1.4 g of noise. This cell is a 200 kg cell at ~22 counts/g, so its
// still-shelf peak-to-peak of 100-350 counts is 5-16 g -- a 5 g threshold would sit
// INSIDE the noise and restart the window constantly. 80 g is ~5x that peak-to-peak
// and far below anything anybody puts on a stock shelf on purpose; a slow drift
// of a few tens of grams never triggers, which is what the average is for.
const float STEP_G = 80.0f;
const int32_t STEP_COUNTS_UNCAL = 1500;  // before calibration, ~70 g at the expected sensitivity
// A floor in counts, so a calibration that comes out at a very low counts/g cannot
// shrink 80 g to something inside the noise.
const int32_t STEP_COUNTS_MIN = 1000;
// Consecutive samples beyond the threshold before the window is discarded. THREE:
// one sample outside the band is what noise looks like, and 300 ms of detection
// latency at 10 SPS is well inside the time a hand takes to let go.
const uint8_t STEP_CONFIRM = 3;
// The fewest samples a step can be judged against. NOT the full window: detection
// clears the window, so gating on a full one would switch the detector off for
// 3.2 s after every step it caught -- which is the bug the counter's notes record.
const uint8_t STEP_MIN_SAMPLES = 4;

// 95% of 2^23, the same line the counter draws.
const int32_t OVER_RANGE_COUNTS = 8000000;

// --- calibration limits ---------------------------------------------------------
// What a derived factor has to look like to be believed. At 3.0 V excitation and
// gain 128 the converter's full scale is +/-11.7 mV, so a 200 kg cell of 1-3 mV/V
// lands at roughly 11-32 counts/g. The band is wide on purpose -- it exists to
// catch a mass typed in the wrong unit or a deflection that is not the cell's,
// which are orders of magnitude out, not to second-guess the cell's rating.
// Both minimums were 2000 g / 20000 counts and were lowered for a bench where the
// heaviest thing to hand was a ~1 kg boxed spool. The COUNT limit is the one that
// matters -- it is what stops a calibration against noise -- and 10000 counts is
// still ~30-70x the window's peak-to-peak on a still shelf (~150-350), so the
// deflection is far above anything the noise can fake. What a light mass costs is
// not safety but ACCURACY: the mass is known to a few per cent and that error is
// multiplied up to the cell's whole span. Calibrate with 20 kg or more when there
// is one.
const float MIN_CAL_G = 1000.0f;
const int32_t MIN_CAL_COUNTS = 10000;
const float CPG_MIN = 2.0f;
const float CPG_MAX = 200.0f;

// A zero or a factor is STORED, so it must be taken from a reading that has
// stopped moving. The window is 3.2 s long: press the key a second after a mass
// lands and the trimmed mean is an average of two plateaus -- a calibration 30%
// low that sits comfortably inside the plausibility band above and is then
// persisted, making every later kilogram read ~40% high. Peak-to-peak across the
// window is what shows it: a step of even 1 kg is ~20,000 counts of p-p, against
// ~150-350 for a shelf standing still. 3000 is generous for a noisy room and
// still catches anything that is actually moving.
const int32_t PP_MAX_COMMIT = 3000;

const uint32_t POLL_MS = 20;       // the part makes 10 conversions a second; 50 polls is plenty
const uint32_t PUBLISH_MS = 100;  // the counter's rate; at 200 the snapshot alone added up to 0.2 s of lag

// HOW OFTEN THE MUX CHANNEL IS RE-ASSERTED, and the number has a ceiling.
//
// The counter recovers a mux that browned out and dropped its channel because
// every failed read invalidates the mux cache, so the very next poll re-selects.
// This module cannot lean on that -- its mux is not the counter's singleton -- so
// it re-opens the channel on a timer instead. The converter driver declares a
// cell Offline after 5 consecutive failed reads, which at POLL_MS is ~100 ms, and
// that latch is permanent. A re-assert slower than that only catches the fault
// by luck (1 s caught ~10% of them); this one is under half of it, so a dropped
// channel is back after at most one or two failed polls and the count never
// reaches 5. Costs one byte on the bus, 25 times a second.
const uint32_t MUX_REASSERT_MS = 40;
static_assert(MUX_REASSERT_MS * 2 <= 5 * POLL_MS,
              "the mux re-assert must beat the converter driver's 5-failure Offline latch");

Nau7802 cell_;

SemaphoreHandle_t mutex_ = nullptr;
TaskHandle_t task_ = nullptr;
Snapshot published_{};
uint32_t seq_ = 0;

bool fitted_ = false;
bool muxOk_ = false;

int32_t ring_[WINDOW];
uint8_t ringCount_ = 0;
uint8_t ringHead_ = 0;

bool zeroed_ = false;
int32_t zero_ = 0;
float cpg_ = 0.0f;

// Flags raised from the console and read on the task. volatile, because the writer
// and the reader are different tasks -- and the mass is written BEFORE its flag, so
// a reader that sees the flag sees the mass.
volatile bool wantZero_ = false;
volatile bool wantCal_ = false;
volatile bool wantClear_ = false;
volatile float wantCalG_ = 0.0f;

void push(int32_t v) {
  ring_[ringHead_] = v;
  ringHead_ = (uint8_t)((ringHead_ + 1) % WINDOW);
  if (ringCount_ < WINDOW) ringCount_++;
}

// Trimmed mean and peak-to-peak of what is in the window. Order is irrelevant to
// both, and the valid entries are always indices below ringCount_, wrapped or not.
bool windowStats(int32_t *mean, int32_t *pp, bool *over) {
  const uint8_t n = ringCount_;
  if (n == 0) return false;

  int32_t t[WINDOW];
  for (uint8_t i = 0; i < n; i++) t[i] = ring_[i];
  for (uint8_t i = 1; i < n; i++) {  // insertion sort: n <= 32, called at 5 Hz
    const int32_t v = t[i];
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && t[j] > v) {
      t[j + 1] = t[j];
      j--;
    }
    t[j + 1] = v;
  }

  // Trim only once there is enough to spare: at a handful of samples, discarding
  // two of them is most of the data.
  const uint8_t trim = (n >= 8) ? TRIM : 0;
  int64_t acc = 0;
  for (uint8_t i = trim; i < (uint8_t)(n - trim); i++) acc += t[i];
  *mean = (int32_t)(acc / (int32_t)(n - 2 * trim));
  *pp = t[n - 1] - t[0];
  *over = (t[0] <= -OVER_RANGE_COUNTS) || (t[n - 1] >= OVER_RANGE_COUNTS);
  return true;
}

// --- step detection, ported from scale.cpp --------------------------------------
uint8_t stepRun_ = 0;
int32_t stepBuf_[STEP_CONFIRM] = {0, 0, 0};
uint32_t nextStepSayMs_ = 0;

int32_t stepCounts() {
  if (cpg_ > 0.0f) {
    const int32_t c = (int32_t)(STEP_G * cpg_);
    return c > STEP_COUNTS_MIN ? c : STEP_COUNTS_MIN;
  }
  return STEP_COUNTS_UNCAL;
}

void clearWindow() {
  ringCount_ = 0;
  ringHead_ = 0;
}

// Every conversion goes through here instead of straight into the ring.
void addSample(int32_t v, uint32_t now) {
  if (ringCount_ >= STEP_MIN_SAMPLES) {
    int32_t m = 0, pp = 0;
    bool over = false;
    windowStats(&m, &pp, &over);
    const int32_t d = (v > m) ? (v - m) : (m - v);
    if (d > stepCounts()) {
      // Kept so the restart can be SEEDED with the samples that triggered it rather
      // than with one raw conversion: all three already measure the new load, and
      // a three-sample mean is ~1.7x quieter than a single conversion at the instant
      // somebody is looking hardest.
      if (stepRun_ < STEP_CONFIRM) stepBuf_[stepRun_] = v;
      if (++stepRun_ >= STEP_CONFIRM) {
        // THROWN AWAY, not blended out of: the window describes a load that is gone.
        clearWindow();
        for (uint8_t k = 0; k < STEP_CONFIRM; k++) push(stepBuf_[k]);
        stepRun_ = 0;
        // Rate limited: a slow pour is a continuous step and would otherwise print
        // three lines a second on a console somebody is trying to read.
        if ((int32_t)(now - nextStepSayMs_) >= 0) {
          nextStepSayMs_ = now + 1000;
          Serial.printf("  buffer: step %+ld counts -- filter restarted\n", (long)(v - m));
        }
        return;  // `v` is already the third seed; pushing it again would count it twice
      }
    } else {
      // A run has to be CONSECUTIVE: one sample back inside the band means the
      // excursion was noise, and the count starts again.
      stepRun_ = 0;
    }
  }
  push(v);
}

// Selects channel 0 of the module's own mux. Sets the whole control register, so
// it is also what guarantees no other channel is open.
bool openChannel() {
  const uint8_t v = (uint8_t)(1u << board::BUF_MUX_CH);
  return lgfx::i2c::transactionWrite(PORT, board::BUF_MUX_ADDR, &v, 1, HZ).has_value();
}

// --- NVS ----------------------------------------------------------------------
// NVS WRITES BLOCK THIS TASK, and briefly the other core with it: a flash erase
// stalls whatever is executing from flash. They happen only on a commissioning
// action, never from the poll loop and never from publish().
void loadPersisted() {
  Preferences p;
  // Read-write even though this only reads: a read-only open of a namespace that
  // has never been written fails and logs an error on every first boot of a unit.
  p.begin(NVS_NS, false);
  zeroed_ = p.getBool(KEY_ZSET, false);
  zero_ = p.getInt(KEY_ZERO, 0);
  // isKey() first: getFloat() on a key that was never written logs an ERROR-level
  // "nvs_get_blob len fail ... NOT_FOUND" line, and an uncalibrated unit would
  // print it on every boot -- a red line in a field capture that means nothing.
  cpg_ = p.isKey(KEY_CPG) ? p.getFloat(KEY_CPG, 0.0f) : 0.0f;
  p.end();

  // A factor outside the band is treated as no factor, rather than trusted: NaN,
  // a corrupted float, or a value from some other firmware must not become kilograms.
  if (!(cpg_ >= CPG_MIN && cpg_ <= CPG_MAX)) cpg_ = 0.0f;
}

void storeZero() {
  Preferences p;
  p.begin(NVS_NS, false);
  p.putInt(KEY_ZERO, zero_);
  p.putBool(KEY_ZSET, zeroed_);
  p.end();
}

void storeFactor() {
  Preferences p;
  p.begin(NVS_NS, false);
  p.putFloat(KEY_CPG, cpg_);
  p.end();
}

// --- snapshot -----------------------------------------------------------------
void publish() {
  Snapshot s{};
  s.fitted = fitted_;
  s.state = cell_.state();
  s.revision = cell_.revision();
  s.zeroed = zeroed_;
  s.zero = zero_;
  s.calibrated = cpg_ > 0.0f;
  s.countsPerGram = cpg_;

  // FIGURES ONLY FROM A CELL THAT IS CONVERTING. Left at zero otherwise, so a
  // cell that has dropped out cannot leave its last number sitting beside a state
  // that says it stopped.
  if (s.state == CellState::Online) {
    s.rawCounts = cell_.counts();
    s.sps = cell_.sps();
    s.samples = ringCount_;
    bool over = false;
    if (windowStats(&s.counts, &s.pp, &over)) {
      s.overRange = over;
      // Kilograms need all three: a zero, a factor, and a window that has had time
      // to settle. A saturated cell reports its ceiling, not a mass.
      if (s.zeroed && s.calibrated && !over && ringCount_ >= MIN_SAMPLES_SHOWN) {
        s.kgKnown = true;
        s.grams = (float)(s.counts - zero_) / cpg_;
      }
    }
  }
  s.seq = ++seq_;

  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    published_ = s;
    xSemaphoreGive(mutex_);
  }
}

// --- commands -----------------------------------------------------------------
bool windowReady(const char *what) {
  if (cell_.state() != CellState::Online) {
    Serial.printf("  buffer: %s refused -- the cell is not converting\n", what);
    return false;
  }
  if (ringCount_ < WINDOW) {
    Serial.printf("  buffer: %s refused -- still settling (%u of %u samples), try again in a few seconds\n",
                  what, ringCount_, WINDOW);
    return false;
  }
  return true;
}

void doZero() {
  if (!windowReady("zero")) return;
  int32_t mean = 0, pp = 0;
  bool over = false;
  windowStats(&mean, &pp, &over);
  if (over) {
    Serial.println("  buffer: zero refused -- the reading is at the end of the converter's range");
    return;
  }
  if (pp > PP_MAX_COMMIT) {
    Serial.printf("  buffer: zero refused -- the reading is not steady (p-p %ld counts, limit %ld). "
                  "Let it settle and try again\n",
                  (long)pp, (long)PP_MAX_COMMIT);
    return;
  }
  zero_ = mean;
  zeroed_ = true;
  storeZero();
  Serial.printf("  buffer: EMPTY-platform zero stored: %ld counts (p-p %ld). The platform must have been empty.\n",
                (long)zero_, (long)pp);
}

void doCalibrate(float knownG) {
  if (!windowReady("calibrate")) return;
  if (!zeroed_) {
    Serial.println("  buffer: calibrate refused -- no zero yet. Empty the platform and press 'z' first");
    return;
  }
  if (knownG < MIN_CAL_G) {
    Serial.printf("  buffer: calibrate refused -- %.0f g is below the %.0f g minimum\n", knownG,
                  MIN_CAL_G);
    return;
  }
  int32_t mean = 0, pp = 0;
  bool over = false;
  windowStats(&mean, &pp, &over);
  if (over) {
    Serial.println("  buffer: calibrate refused -- the reading is at the end of the converter's range");
    return;
  }
  if (pp > PP_MAX_COMMIT) {
    // THE MOST LIKELY CAUSE IS PRESSING THE KEY TOO SOON after the mass landed,
    // while the window still holds the empty platform and the loaded one.
    Serial.printf("  buffer: calibrate refused -- the reading is not steady (p-p %ld counts, limit %ld). "
                  "Wait for it to settle after the mass lands and try again\n",
                  (long)pp, (long)PP_MAX_COMMIT);
    return;
  }

  const int32_t defl = mean - zero_;
  if (defl < MIN_CAL_COUNTS) {
    // The REAL deflection is printed, and the sentence does not name a cause it
    // cannot know: a negative figure means the load went the other way (A+/A-
    // swapped, or the cell mounted upside down); a small one means little or no
    // mass is on it.
    Serial.printf("  buffer: calibrate refused -- deflection %ld counts is below the %ld minimum%s\n",
                  (long)defl, (long)MIN_CAL_COUNTS,
                  defl < 0 ? " (NEGATIVE: the reading fell when mass was added -- check A+/A-)" : "");
    return;
  }

  const float cpg = (float)defl / knownG;
  if (!(cpg >= CPG_MIN && cpg <= CPG_MAX)) {
    Serial.printf("  buffer: calibrate refused -- %.2f counts/g is implausible for this hardware (%.0f-%.0f). "
                  "Is %.0f g really what is on it?\n",
                  cpg, CPG_MIN, CPG_MAX, knownG);
    return;
  }

  cpg_ = cpg;
  storeFactor();
  Serial.printf("  buffer: calibrated -- %.3f counts/g from %.0f g (deflection %ld counts)\n", cpg_,
                knownG, (long)defl);
}

void doClear() {
  cpg_ = 0.0f;
  storeFactor();
  Serial.println("  buffer: calibration cleared -- reading counts again (zero kept)");
}

void serviceCommands() {
  if (wantClear_) {
    wantClear_ = false;
    doClear();
  }
  if (wantZero_) {
    wantZero_ = false;
    doZero();
  }
  if (wantCal_) {
    wantCal_ = false;
    doCalibrate(wantCalG_);
  }
}

// ONE TASK OWNS THE CONVERTER, and its bring-up happens here rather than in
// begin() for the counter's reason: Nau7802::begin() resets the part, waits for
// ready and runs its offset calibration, ~0.4 s at 10 SPS, and nothing in setup()
// needs the answer. Until it concludes the snapshot says Warming-or-Offline.
//
// ONCE OFFLINE IT STAYS OFFLINE until a power cycle, and the bus goes quiet.
//
// THIS IS NOT THE COUNTER'S POLICY, which is easy to assume and wrong. The counter
// declines to retry only ONE cell dropping while the others still answer. If its
// whole lead goes dark it probes the mux at 1 Hz, re-initialises the converters
// and carries on (scale.cpp, serviceLink()). A buffer with a single cell has only
// the "whole lead" case, so a lead that is pulled and re-seated stays dead here
// where the counter would have come back. That is a deliberately conservative
// first cut -- a converter that quietly revives could report a weight against a
// zero measured before it dropped -- and whether the buffer should get the
// counter's recovery is a decision for its owner, not something to fold into a
// bring-up.
//
// "THE BUS GOES QUIET" IS LOAD-BEARING: the mux re-assert below stops with it.
// With SCL stuck low every transaction burns ~26 ms inside lgfx waiting for the
// line, and re-asserting at 25 Hz into that would be most of core 1, at a
// priority above the UI loop.
void bufferTask(void *) {
  // BOUNDED RETRY, AT BRING-UP ONLY -- and that is not the retry the counter's
  // owner declined. That one revives a cell that dropped MID-RUN, which can report
  // a weight against a state it no longer matches. This is the boot sequence: no
  // reading has been given to anybody yet, so trying again costs nothing and
  // misleads nobody.
  //
  // It exists because of a measured failure: a reset of the ESP -- a flash, a
  // watchdog, the counter's restart-when-the-cable-appears -- can land mid-
  // transaction and leave the converter out of step with the bus. The first writes
  // of the next bring-up then fail, begin() returns false at some silent step
  // (the revision read had already succeeded: rev 0x0F), and with no retry the
  // cell stayed Offline until a power cycle -- on a unit about to go to the field.
  // Each attempt's own transactions end in STOPs and clock the bus back into step,
  // so a second try normally just works.
  const uint8_t ATTEMPTS = 5;
  bool up = false;
  for (uint8_t attempt = 1; attempt <= ATTEMPTS && !up; attempt++) {
    openChannel();  // a mux that kept its channel across the reset is fine; one that did not needs this
    up = cell_.begin();
    if (up) {
      if (attempt > 1) Serial.printf("  buffer: came up on attempt %u of %u\n", attempt, ATTEMPTS);
    } else if (attempt < ATTEMPTS) {
      Serial.printf("  buffer: bring-up attempt %u of %u failed, retrying\n", attempt, ATTEMPTS);
      vTaskDelay(pdMS_TO_TICKS(300));
    }
  }
  if (!up) {
    Serial.println("  buffer: converter did not come up after 5 attempts -- reading Offline until a power cycle");
  }

  uint32_t nextPublish = 0;
  uint32_t nextMux = millis() + MUX_REASSERT_MS;
  bool muxWasOk = true;

  for (;;) {
    const uint32_t now = millis();

    if (cell_.state() != CellState::Offline && (int32_t)(now - nextMux) >= 0) {
      nextMux = now + MUX_REASSERT_MS;
      muxOk_ = openChannel();
      // Said on the CHANGE only. A mux that stops answering is the likely story
      // behind a cell that then goes Offline, and the line is what tells the two
      // apart in a serial capture.
      if (muxOk_ != muxWasOk) {
        Serial.printf("  buffer: mux at 0x%02X %s\n", board::BUF_MUX_ADDR,
                      muxOk_ ? "answering again" : "STOPPED ANSWERING");
        muxWasOk = muxOk_;
      }
    }

    if (cell_.poll(now)) addSample(cell_.counts(), now);

    serviceCommands();

    if ((int32_t)(now - nextPublish) >= 0) {
      nextPublish = now + PUBLISH_MS;
      publish();
    }

    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
  }
}

}  // namespace

void begin() {
  const bool busOk = lgfx::i2c::init(PORT, board::BUF_SDA, board::BUF_SCL).has_value();
  muxOk_ = busOk && openChannel();
  fitted_ = muxOk_;
  if (!fitted_) {
    // THE NORMAL STATE OF A UNIT WITH NO BUFFER MODULE, so ONE calm line and not an
    // alarm -- every other LDC unit lands here. Nothing is started: no task, no bus
    // traffic, nothing to publish.
    //
    // It is also, from the bus, exactly what a fitted module with a loose lead
    // looks like -- the two cannot be told apart from here -- so the line says what
    // to check. The counter can be louder about a missing lead only because every
    // unit HAS that lead. And unlike the counter this does not keep probing: a
    // module plugged in later is picked up by a power cycle, not at runtime.
    if (busOk) {
      Serial.printf("\n--- buffer scale (200 kg): not fitted (nothing at 0x%02X on IO%d/IO%d; if a "
                    "module IS fitted, check its lead, 3V3/GND and address straps, then "
                    "power-cycle) ---\n",
                    board::BUF_MUX_ADDR, board::BUF_SDA, board::BUF_SCL);
    } else {
      Serial.println("\n--- buffer scale (200 kg): bus init FAILED ---");
    }
    published_.fitted = false;
    return;
  }

  Serial.println("\n--- buffer scale (200 kg) ---");
  Serial.printf("  bus       i2c port %d (bit-banged)  SDA=%d SCL=%d\n", PORT, board::BUF_SDA,
                board::BUF_SCL);
  Serial.printf("  mux       TCA9548A at 0x%02X, channel %d open\n", board::BUF_MUX_ADDR,
                board::BUF_MUX_CH);

  // A plain bus as far as the converter driver is concerned (muxChannel -1): this
  // module's mux is not the counter's singleton, so the driver is told there is
  // nothing to select and this file holds the one channel open itself.
  cell_.configure(PORT, HZ, "buffer", -1);

  loadPersisted();
  Serial.printf("  zero      %s    factor    %s\n",
                zeroed_ ? "stored" : "not set",
                cpg_ > 0.0f ? "stored" : "not set (reads counts)");
  if (zeroed_) Serial.printf("            zero %ld counts\n", (long)zero_);
  if (cpg_ > 0.0f) Serial.printf("            %.3f counts/g\n", cpg_);

  mutex_ = xSemaphoreCreateMutex();
  // WARMING, NOT THE ZERO VALUE. A zero-initialised snapshot says Offline with
  // revision 0x00, which is a claim -- "this cell is dead" -- about a converter
  // that has not yet been asked. The counter uses Warming for exactly this window
  // (scale.cpp, begin()); nothing reads this before the task's first publish
  // today, but a screen will.
  published_.fitted = true;
  published_.state = CellState::Warming;
  published_.revision = 0xFF;

  // Core 1, priority 2: the measurement core, but BELOW the scale task's 3, so the
  // counter's cells always win a contention. Bit-banged I2C holds this task busy
  // for a fraction of a millisecond per poll; it must never be able to delay the
  // bowl scale for that.
  xTaskCreatePinnedToCore(bufferTask, "buffer", 4096, nullptr, 2, &task_, 1);
  Serial.println("  task started on core 1");
}

Snapshot snapshot() {
  Snapshot s{};
  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    s = published_;
    xSemaphoreGive(mutex_);
  } else {
    s.fitted = fitted_;
  }
  return s;
}

void setZero() {
  if (fitted_) wantZero_ = true;
}

void calibrate(float knownGrams) {
  if (!fitted_) return;
  wantCalG_ = knownGrams;  // BEFORE the flag: see the declaration
  wantCal_ = true;
}

void clearCalibration() {
  if (fitted_) wantClear_ = true;
}

float calMassG() { return (float)BOWLSTACK_BUF_CAL_MASS_G; }

uint32_t stackFreeBytes() { return task_ ? (uint32_t)uxTaskGetStackHighWaterMark(task_) : 0; }

void printStatus() {
  const Snapshot s = snapshot();
  if (!s.fitted) return;  // said once at boot; repeating it every 5 s on every unit is noise

  // ONE WRITE, NOT THREE. loop() calls this, and a write to the native-USB console
  // can block for ~100 ms when nothing is listening (that is what the reverted
  // serial change in 17797b3 was about). Every extra call is another chance to
  // stall the render loop, on the one unit that carries a module.
  char line[256];
  if (s.state != CellState::Online) {
    snprintf(line, sizeof(line), "  buffer  %-7s rev 0x%02X",
             s.state == CellState::Warming ? "warming" : "OFFLINE", s.revision);
    Serial.println(line);
    return;
  }

  // p-p beside the mean, for the counter's reason: the filtered figure is built to
  // look calm, so a disconnected bridge and a still shelf print the same number.
  size_t n = (size_t)snprintf(line, sizeof(line),
                              "  buffer  online  rev 0x%02X  raw %8ld  filt %8ld  p-p %6ld  zero %8ld  %u/s (%u)",
                              s.revision, (long)s.rawCounts, (long)s.counts, (long)s.pp,
                              (long)s.zero, s.sps, s.samples);
  if (n >= sizeof(line)) n = sizeof(line) - 1;  // truncated, not overrun
  if (s.kgKnown) {
    n += (size_t)snprintf(line + n, sizeof(line) - n, "   = %.3f kg", s.grams / 1000.0f);
  } else if (s.overRange) {
    n += (size_t)snprintf(line + n, sizeof(line) - n, "   OVER-RANGE");
  } else if (!s.zeroed) {
    n += (size_t)snprintf(line + n, sizeof(line) - n, "   not zeroed ('z' with the platform EMPTY)");
  } else if (!s.calibrated) {
    n += (size_t)snprintf(line + n, sizeof(line) - n, "   UNCALIBRATED -- counts only");
  }
  if (n >= sizeof(line)) n = sizeof(line) - 1;
  snprintf(line + n, sizeof(line) - n, "   [stack free %lu B]", (unsigned long)stackFreeBytes());
  Serial.println(line);
}

void printHelp() {
  // Nothing on a unit with no module: listing keys that do nothing there would be
  // advertising a capability the unit does not have.
  if (!fitted_) return;
  Serial.printf(
      "\n  BUFFER (200 kg cell, IO21/IO16)\n"
      "  z      store the current reading as the EMPTY-platform zero. The platform\n"
      "         must be empty and the reading steady. Persisted; this is NOT a\n"
      "         session tare -- the buffer measures stock already there at power-up\n"
      "  g      calibrate against %.0f g (BOWLSTACK_BUF_CAL_MASS_G). Zero first,\n"
      "         then put exactly that mass on and let the reading settle\n"
      "  k      clear the buffer calibration and go back to counts (zero kept)\n",
      calMassG());
}

// Z, NOT B. 'a' and 'b' were the old per-cell tare keys (see serviceConsole), so a
// stray 'b' from muscle memory would overwrite the persisted empty-platform zero
// with whatever happens to be on the shelf -- and every kilogram after that would
// be wrong with nothing to say so.
bool consoleKey(int c) {
  // A unit with no module neither acts on these keys nor pretends to: the echo
  // would otherwise claim "store zero" and then do nothing.
  if (!fitted_) return false;
  switch (c) {
    case 'z':
    case 'Z':
      Serial.println("\n> buffer: store EMPTY-platform zero");
      setZero();
      return true;
    case 'g':
    case 'G':
      Serial.printf("\n> buffer: calibrate against %.0f g\n", calMassG());
      calibrate(calMassG());
      return true;
    case 'k':
    case 'K':
      Serial.println("\n> buffer: clear calibration");
      clearCalibration();
      return true;
    default:
      return false;
  }
}

}  // namespace bufscale
