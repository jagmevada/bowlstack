// Owns the VL53L0X hardware: XSHUT addressing, concurrent ranging, and the
// round-robin acquisition loop. Everything above this layer sees only
// Reading values and never touches a driver.

#pragma once

#include <Arduino.h>
#include <VL53L0X.h>

#include "config.h"
#include "trimmed_window.h"

enum class SensorState : uint8_t {
  Offline,  // init failed, or failed at runtime; produces no readings
  Warming,  // configured, but has not yet completed its first measurement
  Online,   // ranging and returning results
};

struct Reading {
  float distanceMm;  // trimmed mean plus the per-part offset
  float stdevMm;
  uint8_t samples;   // samples contributing after trimming
  bool valid;        // sensor online AND holding a usable measurement
};

class SensorArray {
 public:
  // Runs the full XSHUT walk and starts continuous ranging. Safe to call once
  // from setup(); begins both I2C buses itself.
  void begin();

  // Non-blocking. Services every sensor that already has a result waiting and
  // returns immediately. Call as often as possible from loop().
  void poll();

  // Re-runs reset, init and addressing for a single sensor. Safe while the
  // others are live: they sit at 0x30..0x33, so a sensor briefly back at 0x29
  // collides with nothing. This is the hook for fault restoration.
  bool recover(uint8_t level);

  Reading reading(uint8_t level) const;

  SensorState state(uint8_t level) const { return ch_[level].state; }

  // Sensors producing readings. Excludes Warming by design.
  uint8_t onlineCount() const;

  // Sensors that initialised, whether or not they have concluded a measurement
  // yet. This -- not onlineCount() -- is the test for "did the bus work":
  // immediately after begin() every healthy sensor is still Warming, so
  // onlineCount() is legitimately 0 and would misreport a perfect bus as dead.
  uint8_t initialisedCount() const;

  // millis() of the last accepted in-range sample, for staleness detection.
  // Zero until the sensor has ever produced one.
  uint32_t lastSampleMs(uint8_t level) const { return ch_[level].lastSampleMs; }

  // Prints per-bus scans and flags sensors still answering on an assigned
  // address, which means their XSHUT is not actually wired.
  void printDiagnostics() const;

 private:
  struct Channel {
    VL53L0X dev;
    SensorState state = SensorState::Offline;
    TrimmedWindow window;
    uint8_t consecutiveInvalid = 0;
    uint8_t ioFailures = 0;
    uint32_t lastSampleMs = 0;    // last in-range sample
    uint32_t lastActivityMs = 0;  // last completed measurement, in range or not
    uint32_t nextRecoverMs = 0;
  };

  bool initSensor(uint8_t level);
  void demote(uint8_t level, const char *why);
  void maybeRecover(uint8_t level, uint32_t now);
  static void shutdown(uint8_t level);
  static void enable(uint8_t level);

  Channel ch_[config::SENSOR_COUNT];
};
