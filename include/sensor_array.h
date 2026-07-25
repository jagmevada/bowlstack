// Owns the VL53L0X hardware: XSHUT addressing, concurrent ranging, and the
// round-robin acquisition loop. Everything above this layer sees only
// Reading values and never touches a driver.

#pragma once

#include <Arduino.h>
#include <VL53L0X.h>

#include "config.h"
#include "trimmed_window.h"

enum class SensorState : uint8_t {
  Offline,  // init failed, or parked in reset; produces no readings
  Online,   // configured and ranging continuously
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
  uint8_t onlineCount() const;

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
    uint32_t lastSampleMs = 0;
  };

  bool initSensor(uint8_t level);
  static void shutdown(uint8_t level);
  static void enable(uint8_t level);

  Channel ch_[config::SENSOR_COUNT];
};
