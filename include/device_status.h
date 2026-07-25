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
  uint16_t batteryMv;      // at the cell, after undoing the divider
  int8_t batteryPercent;   // -1 when no cell is detected; never fabricated
  bool charging;

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

}  // namespace device_status
