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

State sEmpty() { return stacked(0); }
State sTwo() { return stacked(2); }
State sFull() { return stacked(4); }

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
  return s;
}

State sWeakSignal() {
  State s = stacked(2);
  s.wifiRssi = -82;  // associated but marginal: one bar
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
  return s;
}

struct Scenario {
  const char *name;
  State (*make)();
};

const Scenario SCENARIOS[] = {
    {"empty", sEmpty},
    {"2 bowls", sTwo},
    {"full", sFull},
    {"degraded", sDegraded},
    {"DISCONTIGUOUS", sDiscontiguous},
    {"no cell", sNoCell},
    {"critical + offline", sCritical},
    {"charging", sCharging},
    {"weak signal", sWeakSignal},
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
