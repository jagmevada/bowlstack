#include "ui_demo.h"

#include "ui_screens.h"
#include "ui_wifi.h"

namespace ui {
namespace {

// Long enough to read the screen, short enough that a person watching the
// device does not conclude it has frozen.
const uint32_t DWELL_MS = 3000;

uint8_t idx_ = 0;
uint32_t nextAt_ = 0;
bool armed_ = false;

// The latest fabricated state, held rather than pushed straight into widgets.
// That separation is what lets a page stop RENDERING without the data going
// stale underneath it -- see ui_pages.cpp.
State latest_;
bool haveLatest_ = false;

bool stateOverride_ = false;
bool haveWifi_ = false;
bool haveTime_ = false;
bool timeKnown_ = false;
uint8_t timeHH_ = 0, timeMM_ = 0;
bool wifiConnected_ = false;
int16_t wifiRssi_ = 0;
bool battOverride_ = false;
uint16_t ovCellMv_ = 0, ovPinMv_ = 0;
int8_t ovPct_ = -1;
Battery ovBand_ = Battery::Unknown;

State base() {
  State s = unknownState();
  for (uint8_t i = 0; i < LEVELS; i++) s.sensorOnline[i] = true;
  s.sensorsOnline = LEVELS;
  s.stack = Stack::Ok;
  s.battery = Battery::Good;
  s.batteryMv = 4102;
  s.batteryPercent = 95;
  s.batteryPinMv = 1367;  // 4102 / 3.0, the on-board divider ratio
  s.chargingKnown = false;
  s.wifiConnected = true;
  s.wifiRssi = -58;
  s.deviceId = "BWL-001";
  s.firmware = "0.3.0";
  return s;
}

State stacked(uint8_t n) {
  State s = base();
  for (uint8_t i = 0; i < LEVELS; i++)
    s.levels[i] = (i < n) ? Level::Present : Level::Absent;
  s.stackCount = n;
  return s;
}

// --- the weighing fixture --------------------------------------------------
// Grams in, counts derived, rather than the other way round: the scenarios are
// written in the unit a person thinks in, and the counts follow from a plausible
// sensitivity so the UNCALIBRATED display has something realistic to show.
//
// 106.857 counts/g -- the MEASURED sensitivity of the bench assembly, not an
// estimate: 18,700 counts for a 175 g reference mass on two YZC-133 cells. The
// same number platformio.ini hands the firmware as its default factor.
//
// Using the real figure rather than a plausible one is what makes the
// heavy scenario below sit where a real one would. It also agrees with the
// datasheet arithmetic to half a per cent -- see platformio.ini -- which is what
// makes it trustworthy rather than merely fitted.
const float DEMO_COUNTS_PER_G = 106.857f;

// Where a signed 24-bit conversion stops being a measurement. Mirrors
// OVER_RANGE_COUNTS in scale.cpp; restated here because ui_demo may not include
// a driver header, and duplicated deliberately rather than shared through a
// third file that neither target would naturally own.
const int32_t DEMO_OVER_RANGE = 8000000;

void withScale(State &s, float aG, float bG, bool calibrated, bool tared, uint8_t online) {
  s.scale.calibrated = calibrated;
  s.scale.tared = tared;
  s.scale.online = online;

  const float g[CELLS] = {aG, bG};
  int32_t total = 0;
  for (uint8_t i = 0; i < CELLS; i++) {
    CellView &c = s.scale.cell[i];
    c.state = (i < online) ? Cell::Online : Cell::Offline;
    c.counts = (c.state == Cell::Online) ? (int32_t)(g[i] * DEMO_COUNTS_PER_G) : 0;
    c.grams = (c.state == Cell::Online) ? g[i] : 0.0f;
    // 80 SPS configured; 78-79 delivered is what a real bus behind a real loop
    // gives back, and the fixture says so rather than showing a round number
    // the device will never quite reach.
    c.sps = (c.state == Cell::Online) ? (uint16_t)(79 - i) : 0;
    // FALLS OUT OF THE ARITHMETIC rather than being set by hand. At the real
    // sensitivity a 20 kg platform load puts a cell past 2^23 long before the
    // number on the side of the cell is reached, so the heavy scenario becomes
    // an over-range one on its own -- which is the point. A fixture that had to
    // be told to saturate would not have caught that the ceiling is nearer than
    // it looks.
    c.overRange = (c.state == Cell::Online) && (c.counts > DEMO_OVER_RANGE);
    if (c.overRange) s.scale.overRange = true;
    if (c.state == Cell::Online) total += c.counts;

    // The raw figures the Device page shows. Fabricated the way the real ones
    // arrive rather than as round numbers: a TARE of order a hundred thousand
    // counts, because that is the platform's own weight sitting on the cells
    // before anything is put on it; a RAW reading that is the tare plus the
    // load plus a few counts of wander; and a peak-to-peak of a few hundred,
    // which is what a live bridge at gain 128 actually does.
    //
    // A preview that showed a clean zero tare and a raw equal to the net would
    // make the Device page look like it had nothing to say, which is the
    // opposite of true.
    const int32_t fakeTare = (int32_t)(118000 + i * 9000);
    c.platformZero = (c.state == Cell::Online) ? fakeTare : 0;
    c.tare = 0;
    c.pp = (c.state == Cell::Online) ? (int32_t)(430 + i * 87) : 0;
    c.rawCounts = (c.state == Cell::Online) ? (c.counts + fakeTare + (int32_t)(i * 53) - 26) : 0;
  }
  s.scale.totalCounts = total;
  s.scale.totalGrams = calibrated ? (aG + (online > 1 ? bG : 0.0f)) : 0.0f;
  s.scale.countsPerGram = calibrated ? DEMO_COUNTS_PER_G : 0.0f;
  // The default the firmware ships with, so the preview and the panel agree
  // about what the Average row says before anyone touches it.
  s.scale.window = (uint8_t)BOWLSTACK_AVG_WINDOW;
  s.scale.calMassG = (float)BOWLSTACK_CAL_MASS_G;
  s.scale.zeroed = (online == CELLS);
}

State sEmpty() {
  State s = stacked(0);
  // A tared, calibrated, empty platform. Not exactly zero: two 20 kg cells
  // drift a gram or two with temperature within minutes of a tare, and a screen
  // that shows a perfect 0 forever is showing a constant rather than a
  // measurement.
  withScale(s, 1.0f, -2.0f, true, true, CELLS);
  return s;
}

State sTwo() {
  State s = stacked(2);
  // A bowl placed slightly off-centre: the shares differ, the total does not
  // care. That asymmetry is the entire reason both cells are on the screen.
  withScale(s, 612.0f, 638.0f, true, true, CELLS);
  return s;
}

State sFull() {
  State s = stacked(4);
  withScale(s, 4180.0f, 4241.0f, true, true, CELLS);
  return s;
}

State sDegraded() {
  // A dead sensor sandwiched between the top bowl and the first absent level
  // is the ONLY arrangement that leaves the count genuinely ambiguous -- see
  // sensor_logic.md. Every other position is recoverable by contiguity, which
  // is why this is the case worth rendering.
  State s = stacked(2);
  s.levels[2] = Level::Unknown;
  s.sensorOnline[2] = false;
  s.sensorsOnline = 3;
  s.stack = Stack::Degraded;
  // ONE CELL DOWN. The total is not "half the weight", it is not a weight at
  // all -- the missing cell's share is unknown, not zero. The dashboard shows
  // what the surviving cell reports and flags the assembly, rather than adding
  // a number that would read as a light bowl.
  withScale(s, 640.0f, 0.0f, true, true, 1);
  return s;
}

State sDiscontiguous() {
  // A bowl above nothing. Bowls cannot float, so this is a failed sensor, a
  // misaligned mount or an obstruction -- never a count.
  State s = stacked(0);
  s.levels[0] = Level::Absent;
  s.levels[1] = Level::Present;
  s.stack = Stack::Discontiguous;
  s.stackCount = 0;
  // Never calibrated: both cells converting, no known mass ever applied, so
  // there is no gram figure and the page shows counts. This is the state every
  // unit is in the first time it boots.
  withScale(s, 604.0f, 631.0f, false, false, CELLS);
  return s;
}

State sNoCell() {
  State s = stacked(3);
  s.battery = Battery::Unknown;
  s.batteryMv = 0;
  s.batteryPinMv = 0;
  // -1, not 0. "0%" is a claim that the cell is empty; this state is that
  // nothing is known about it, which is a different statement entirely.
  s.batteryPercent = -1;
  withScale(s, 1902.0f, 1874.0f, true, true, CELLS);
  return s;
}

State sCritical() {
  State s = stacked(1);
  s.battery = Battery::Critical;
  s.batteryMv = 3312;
  s.batteryPercent = 8;
  s.batteryPinMv = 1104;
  s.wifiConnected = false;
  s.wifiRssi = 0;
  // NEITHER CONVERTER ANSWERING -- the state of a board with no cells wired,
  // which is what this branch's first flash will actually look like. Dashes,
  // not zero.
  withScale(s, 0.0f, 0.0f, false, false, 0);
  return s;
}

State sWeakSignal() {
  State s = stacked(2);
  s.wifiRssi = -82;  // associated but marginal: one bar
  // Calibrated but never tared: the factor is known, so grams are real, but
  // they include the platform. A number that is right about the change and
  // wrong about the absolute, which is worth being told.
  withScale(s, 1240.0f, 1198.0f, true, false, CELLS);
  return s;
}

State sCharging() {
  State s = stacked(4);
  s.battery = Battery::Medium;
  s.batteryMv = 3821;
  s.batteryPercent = 52;
  s.batteryPinMv = 1274;
  s.chargingKnown = true;
  s.charging = true;
  withScale(s, 9840.0f, 10120.0f, true, true, CELLS);
  return s;
}

struct Scenario {
  const char *name;
  State (*make)();
};

const Scenario SCENARIOS[] = {
    {"empty platform", sEmpty},
    {"one bowl, off-centre", sTwo},
    {"loaded", sFull},
    {"ONE CELL DOWN", sDegraded},
    {"uncalibrated (counts)", sDiscontiguous},
    {"no battery", sNoCell},
    {"NO CELLS + critical", sCritical},
    {"heavy + charging", sCharging},
    {"not tared, weak signal", sWeakSignal},
};

// Fabricated scan results. Real ones arrive when net.cpp joins this image; the
// UI never calls WiFi itself, for the same reason ui_state.h exists.
const Network MOCK_NETS[] = {
    {"Kitchen-2G", -47, true},   {"Mandir_Guest", -61, true},
    {"BSNL-AP-04", -72, true},   {"Seva-Office", -78, true},
    {"OpenServe", -83, false},
};

}  // namespace

void demoInstallWifiMocks() {
  wifiSetNetworks(MOCK_NETS, (uint8_t)(sizeof(MOCK_NETS) / sizeof(MOCK_NETS[0])));
  // Not connected, matching what the harness can actually claim: it links no
  // networking at all. Showing a fabricated association would put a number on
  // screen that no part of this build could have measured.
  wifiSetConnected(nullptr, 0, nullptr);
}

void demoOverrideState(const State &s) {
  latest_ = s;
  // The radio is a separate source from the sensors and arrives at its own
  // rate, so the adapter must not be able to stamp stale link state over it.
  if (haveWifi_) {
    latest_.wifiConnected = wifiConnected_;
    latest_.wifiRssi = wifiRssi_;
  }
  haveLatest_ = true;
  // Stops demoTick() from cycling. Real data outranks fixtures, and the two
  // must not take turns on the same screen.
  stateOverride_ = true;
}

void demoOverrideTime(bool known, uint8_t hh, uint8_t mm) {
  haveTime_ = true;
  timeKnown_ = known;
  timeHH_ = hh;
  timeMM_ = mm;
}

void demoOverrideWifi(bool connected, int16_t rssi) {
  wifiConnected_ = connected;
  wifiRssi_ = rssi;
  haveWifi_ = true;
}

void demoOverrideBattery(uint16_t cellMv, uint16_t pinMv, int8_t pct, Battery band) {
  battOverride_ = true;
  ovCellMv_ = cellMv;
  ovPinMv_ = pinMv;
  ovPct_ = pct;
  ovBand_ = band;
}

uint8_t demoCount() { return (uint8_t)(sizeof(SCENARIOS) / sizeof(SCENARIOS[0])); }
const char *demoName(uint8_t i) { return SCENARIOS[i % demoCount()].name; }
State demoState(uint8_t i) { return SCENARIOS[i % demoCount()].make(); }
uint8_t demoIndex() { return idx_; }

bool demoTick(uint32_t nowMs) {
  // Armed on first call rather than from zero, because the device's millis()
  // is already tens of thousands by the time the UI exists and a fixed
  // starting deadline would fire on every tick until it caught up.
  if (!armed_) {
    armed_ = true;
    nextAt_ = nowMs;
  }
  // Fixtures stop the moment real sensor data arrives.
  if (stateOverride_) return false;
  if ((int32_t)(nowMs - nextAt_) < 0) return false;

  nextAt_ = nowMs + DWELL_MS;
  latest_ = demoState(idx_);
  haveLatest_ = true;
  idx_ = (uint8_t)((idx_ + 1) % demoCount());
  return true;
}

const State &demoLatest(uint32_t nowMs) {
  if (!haveLatest_) {
    latest_ = demoState(0);
    haveLatest_ = true;
  }

  // The fabricated wall clock is a FIXTURE and stops the moment real data
  // arrives. It used to run unconditionally and AFTER demoOverrideState() had
  // stored measured values, so on hardware it clobbered whatever the adapter
  // set and put a clock reading 60x real time on a board that has no RTC at
  // all -- a confident wrong answer, which is the one kind this codebase is
  // built to avoid.
  //
  // With real data the honest value is timeKnown = false, because there is
  // still no RTC and nothing has supplied a time. See ui_state.h.
  //
  // Scoped rather than an early return: the BATTERY override below is a real
  // measurement and must still be applied on hardware, where stateOverride_ is
  // always set. An early return here would have silently disabled it.
  if (!stateOverride_) {
    if (latest_.wifiConnected) {
      const uint32_t mins = (14 * 60 + 32) + (nowMs / 1000);
      latest_.timeKnown = true;
      latest_.hh = (uint8_t)((mins / 60) % 24);
      latest_.mm = (uint8_t)(mins % 60);
    } else {
      latest_.timeKnown = false;
    }
  }

  // A real clock outranks both the fixture and the sensor adapter's
  // timeKnown = false. Applied last so nothing downstream can undo it.
  if (haveTime_) {
    latest_.timeKnown = timeKnown_;
    latest_.hh = timeHH_;
    latest_.mm = timeMM_;
  }

  if (battOverride_) {
    latest_.batteryMv = ovCellMv_;
    latest_.batteryPinMv = ovPinMv_;
    latest_.batteryPercent = ovPct_;
    latest_.battery = ovBand_;
  }

  latest_.uptimeSec = nowMs / 1000;
  return latest_;
}

}  // namespace ui
