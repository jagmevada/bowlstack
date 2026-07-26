// FreeRTOS task fabric.
//
// Every stall this project has suffered came from one loop doing measurement
// and networking in turn: 138 s parked in the captive portal, 62 s in
// WiFiManager's save path, 2-10 s in a portal scan, 8 s per HTTP timeout.
// During each, no sensor was polled. Cooperative scheduling could not fix that,
// because the blocking lives inside third-party libraries.
//
// Splitting into tasks makes it structurally impossible: measurement runs on
// its own core at its own priority and is preempted by nothing the network
// does.
//
//   sensorTask     core 1, prio 3  I2C, filtering, presence, stack count
//   netTask        core 0, prio 2  WiFi join, portal, reconnection
//   telemetryTask  core 0, prio 2  TLS, PATCH, event batches
//   debugTask      core 1, prio 1  serial plotter / heartbeat
//
// Tasks share nothing directly. The sensor task OWNS SensorArray and BowlLogic
// -- no other task may touch a driver -- and publishes immutable snapshots
// under a mutex; changes travel to telemetry through a queue.

#pragma once

#include <Arduino.h>

#include "bowl_logic.h"
#include "config.h"
#include "device_status.h"
#include "sensor_array.h"

namespace tasks {

// Everything the debug harness needs to render a frame, copied out of the
// sensor task so no other task ever reads a live driver. SensorArray is not
// thread-safe and must not become so: keeping it single-owner is what makes
// the concurrency reviewable.
struct PlotFrame {
  float distanceMm[config::SENSOR_COUNT];
  float stdevMm[config::SENSOR_COUNT];
  uint8_t samples[config::SENSOR_COUNT];
  bool valid[config::SENSOR_COUNT];
  LevelState level[config::SENSOR_COUNT];
  SensorState state[config::SENSOR_COUNT];
  uint8_t stackCount;
  StackStatus stackStatus;
};

// Creates the tasks and returns immediately. Each task performs its own
// subsystem initialisation, so setup() never blocks -- notably net::begin(),
// whose scan and joins used to delay every sensor by seconds.
void start();

// Latest published state. Both return a COPY taken under the mutex; callers
// must not hold references into shared state.
DeviceStatus snapshot();
PlotFrame plotFrame();

// True once the sensor task has published at least once.
bool ready();

// Diagnostics: high-water marks, in bytes of stack still unused. A value
// approaching zero means that task is close to overflowing.
void printStackHeadroom();

}  // namespace tasks
