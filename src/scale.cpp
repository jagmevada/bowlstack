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
const char *KEY_TARED = "tared";

// Below this a calibration is refused. At gain 128 the assembly produces of the
// order of a hundred counts per gram, so 200 g is tens of thousands of counts
// -- far above anything noise can fake. Accepting a 5 g "known mass" would let
// a noise excursion set the factor for every reading afterwards.
const float MIN_CAL_GRAMS = 200.0f;

// --- the trimmed window ----------------------------------------------------
class CountWindow {
 public:
  void clear() {
    count_ = 0;
    next_ = 0;
  }
  void push(int32_t v) {
    ring_[next_] = v;
    next_ = (uint8_t)((next_ + 1) % WINDOW);
    if (count_ < WINDOW) count_++;
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
  int32_t mean() const {
    if (count_ == 0) return 0;
    int32_t sorted[WINDOW];
    for (uint8_t i = 0; i < count_; i++) sorted[i] = ring_[i];
    // Insertion sort. count_ is 8; anything cleverer is slower here.
    for (uint8_t i = 1; i < count_; i++) {
      const int32_t v = sorted[i];
      int8_t j = (int8_t)i - 1;
      while (j >= 0 && sorted[j] > v) {
        sorted[j + 1] = sorted[j];
        j--;
      }
      sorted[j + 1] = v;
    }
    uint8_t lo = 0, hi = count_;
    if (count_ >= (uint8_t)(2 * WINDOW_TRIM + 2)) {
      lo = WINDOW_TRIM;
      hi = (uint8_t)(count_ - WINDOW_TRIM);
    }
    int64_t acc = 0;
    for (uint8_t i = lo; i < hi; i++) acc += sorted[i];
    return (int32_t)(acc / (int32_t)(hi - lo));
  }

 private:
  int32_t ring_[WINDOW] = {0};
  uint8_t count_ = 0;
  uint8_t next_ = 0;
};

// --- owned by the task, touched by nothing else -----------------------------
Nau7802 cell_[CELLS];
CountWindow window_[CELLS];
int32_t offset_[CELLS] = {0, 0};
float countsPerGram_ = 0.0f;
bool tared_ = false;

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
volatile bool wantClearCal_ = false;
volatile float wantCalGrams_ = 0.0f;
volatile bool calResult_ = false;
volatile bool calDone_ = true;

void loadPersisted() {
  prefs_.begin(NVS_NS, false);
  offset_[0] = prefs_.getInt(KEY_OFF_A, 0);
  offset_[1] = prefs_.getInt(KEY_OFF_B, 0);
  countsPerGram_ = prefs_.getFloat(KEY_CPG, 0.0f);
  // Stored as its own flag rather than inferred from a non-zero offset. The
  // inference is almost always right and wrong exactly once -- a platform that
  // happened to tare at 0 counts would come back from a reboot claiming it had
  // never been tared.
  tared_ = prefs_.getBool(KEY_TARED, false);
  prefs_.end();

  if (countsPerGram_ > 0.0f) {
    Serial.printf("  restored: %.3f counts/g, tare %ld / %ld\n", countsPerGram_,
                  (long)offset_[0], (long)offset_[1]);
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
  prefs_.putBool(KEY_TARED, tared_);
  prefs_.end();
}

void storeFactor() {
  prefs_.begin(NVS_NS, false);
  prefs_.putFloat(KEY_CPG, countsPerGram_);
  prefs_.end();
}

void publish() {
  Snapshot s{};
  s.countsPerGram = countsPerGram_;
  s.calibrated = countsPerGram_ > 0.0f;
  s.tared = tared_;

  int32_t sumNet = 0;
  for (uint8_t i = 0; i < CELLS; i++) {
    CellSnapshot &c = s.cell[i];
    c.state = cell_[i].state();
    c.revision = cell_[i].revision();
    c.rawCounts = cell_[i].counts();
    c.counts = window_[i].mean();
    c.samples = window_[i].size();
    c.pp = window_[i].pp();
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
  if (wantClearCal_) {
    wantClearCal_ = false;
    countsPerGram_ = 0.0f;
    storeFactor();
    Serial.println("scale: calibration cleared -- back to counts");
  }

  if (wantTare_) {
    wantTare_ = false;
    for (uint8_t i = 0; i < CELLS; i++) {
      // Only a cell that is actually converting can be tared. Zeroing an
      // offline cell against whatever its window last held would bake that
      // stale value in as the platform's weight.
      if (cell_[i].state() == CellState::Online) offset_[i] = window_[i].mean();
    }
    tared_ = true;
    storeOffsets();
    Serial.printf("scale: tared at %ld / %ld counts\n", (long)offset_[0], (long)offset_[1]);
  }

  if (!calDone_) {
    const float known = wantCalGrams_;
    calResult_ = false;
    if (tared_ && known >= MIN_CAL_GRAMS) {
      int32_t sumNet = 0;
      uint8_t online = 0;
      for (uint8_t i = 0; i < CELLS; i++) {
        if (cell_[i].state() != CellState::Online) continue;
        sumNet += window_[i].mean() - offset_[i];
        online++;
      }
      // Both cells, or the factor describes half a platform. And a positive
      // deflection, or the mass is not on it -- a negative sum with a mass
      // supposedly loaded means the cells are wired backwards, which is worth
      // catching here rather than as a scale that counts downward.
      if (online == CELLS && sumNet > 0) {
        countsPerGram_ = (float)sumNet / known;
        storeFactor();
        calResult_ = true;
        Serial.printf("scale: calibrated -- %ld counts for %.0f g = %.3f counts/g\n",
                      (long)sumNet, known, countsPerGram_);
      } else {
        Serial.printf("scale: calibration refused (online %u/%u, net %ld counts)\n", online,
                      CELLS, (long)sumNet);
      }
    } else {
      Serial.printf("scale: calibration refused (tared %d, mass %.0f g, minimum %.0f g)\n",
                    (int)tared_, known, MIN_CAL_GRAMS);
    }
    calDone_ = true;
  }
}

void scaleTask(void *) {
  uint32_t nextPublishMs = 0;
  for (;;) {
    const uint32_t now = millis();

    for (uint8_t i = 0; i < CELLS; i++) {
      if (cell_[i].poll(now)) window_[i].push(cell_[i].counts());
      // A cell that dropped offline must not keep a window that still averages
      // to a plausible weight. Cleared here rather than in publish(), so the
      // sample count on the diagnostics page tells the truth about what is
      // actually in the filter.
      if (cell_[i].state() == CellState::Offline) window_[i].clear();
    }

    serviceCommands();

    // 20 Hz. The panel cannot show more than about fifteen distinct values a
    // second, and every publish costs the UI task a mutex it would rather not
    // contend for -- so anything faster is work whose result nobody sees. The
    // converters keep running at their own 80 SPS underneath, which is what the
    // window averages.
    if ((int32_t)(now - nextPublishMs) >= 0) {
      nextPublishMs = now + 50;
      publish();
    }

    // 2 ms. At 80 SPS a conversion lands every 12.5 ms, so this polls each cell
    // six times per sample -- enough that a completed conversion is picked up
    // within a couple of milliseconds of being ready, which is what keeps the
    // measured rate equal to the configured one rather than some beat frequency
    // between the two.
    vTaskDelay(pdMS_TO_TICKS(2));
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

bool calibrate(float knownGrams) {
  wantCalGrams_ = knownGrams;
  calDone_ = false;
  // Bounded wait for the task to act. The caller is the UI task and a hung wait
  // here would freeze the screen; 500 ms is far longer than one task iteration
  // and short enough that a stalled scale task shows up as a refused
  // calibration rather than a dead panel.
  const uint32_t deadline = millis() + 500;
  while (!calDone_ && (int32_t)(millis() - deadline) < 0) delay(5);
  return calDone_ && calResult_;
}

void clearCalibration() { wantClearCal_ = true; }

uint32_t stackFreeBytes() { return task_ ? uxTaskGetStackHighWaterMark(task_) : 0; }

}  // namespace scale
