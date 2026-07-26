// Assembles the full device report: identity, power, health and the bowl
// count. This is the payload the Phase 3 Supabase uplink will serialise, kept
// separate from transport so the same snapshot can be printed, posted, or
// compared against the last one to trigger an immediate send.

#pragma once

#include <Arduino.h>

#include "bowl_logic.h"
#include "config.h"
#include "sensor_array.h"

struct DeviceStatus {
  // --- identity ---
  const char *deviceId;    // installation slot, survives a board swap
  const char *firmware;
  char mac[18];            // the board currently in that slot

  // --- liveness ---
  uint32_t uptimeSec;

  // --- power ---
  uint16_t batteryPinMv;   // raw at the ADC pin, before the divider
  uint16_t batteryMv;      // at the cell
  int8_t batteryPercent;   // -1 when unknown. LOCAL ONLY -- see below.
  bool charging;

  // batteryPercent is kept for the console and for deriving the band, but only
  // the BAND is sent upstream. A percentage from a resting-voltage curve is not
  // worth its own precision -- load, temperature, cell age and per-unit ADC
  // calibration all move it several points -- so publishing a number would
  // invite the UI to render a confidence the measurement does not have.

  // --- sensor health ---
  bool sensorOnline[config::SENSOR_COUNT];
  uint8_t sensorsOnline;

  // --- the payload that matters ---
  LevelState levels[config::SENSOR_COUNT];
  uint8_t stackCount;
  StackStatus stackStatus;
};

namespace device_status {

// Configures the battery ADC and charger input. Call once from setup().
void begin();

DeviceStatus sample(const SensorArray &sensors, const BowlLogic &logic);

// True when anything a server would care about has changed since the previous
// snapshot -- bowl count, stack status, a level, sensor health, or charging.
// Deliberately ignores battery millivolts and uptime, which drift constantly
// and would otherwise make every comparison report a change.
bool differs(const DeviceStatus &a, const DeviceStatus &b);

void print(const DeviceStatus &s);

// One line: pin voltage, cell voltage, band and charge state. Printed on its
// own cadence -- in the PRODUCTION build too, since it is the only power
// telemetry visible without a network -- and included in print().
void printPower(const DeviceStatus &s);

}  // namespace device_status
