#include "ui_demo.h"

#include "ui_screens.h"

namespace ui {
namespace {

// Long enough to read the screen, short enough that a person watching the
// device does not conclude it has frozen.
const uint32_t DWELL_MS = 3000;

uint8_t idx_ = 0;
uint32_t nextAt_ = 0;
bool armed_ = false;

State base() {
  State s = unknownState();
  for (uint8_t i = 0; i < LEVELS; i++) s.sensorOnline[i] = true;
  s.sensorsOnline = LEVELS;
  s.stack = Stack::Ok;
  s.battery = Battery::Good;
  s.batteryMv = 4102;
  s.chargingKnown = false;
  s.wifiConnected = true;
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
  return s;
}

State sCritical() {
  State s = stacked(1);
  s.battery = Battery::Critical;
  s.batteryMv = 3312;
  s.wifiConnected = false;
  return s;
}

State sCharging() {
  State s = stacked(4);
  s.battery = Battery::Medium;
  s.batteryMv = 3821;
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
};

}  // namespace

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
  if ((int32_t)(nowMs - nextAt_) < 0) return false;

  nextAt_ = nowMs + DWELL_MS;
  const State s = demoState(idx_);
  update(s);
  idx_ = (uint8_t)((idx_ + 1) % demoCount());
  return true;
}

}  // namespace ui
