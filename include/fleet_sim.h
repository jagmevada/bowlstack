// Fleet simulator -- one bare ESP32 standing in for up to 31 Bowlstack nodes.
//
// PURPOSE
// -------
// A TEST BED FOR THE FRONT-END. The front-end has to render a state space that
// one prototype on a bench cannot produce: an empty station next to a full one,
// a device whose count must not be trusted, a battery that is absent rather than
// flat, a unit that has gone quiet during service, another that was never
// installed. Building those screens against a single healthy device means
// discovering the edge cases in production.
//
// So the states here are ASSIGNED, not simulated and hoped for. A random walk
// over a 2.5 h dinner will not reliably produce a `discontiguous` fault or a
// missing cell, and a test bed that only usually covers a case is not a test
// bed. Every scenario in the enum below is guaranteed present as long as enough
// nodes are configured, and each maps to a specific thing the UI must get right.
//
// WHAT IS REAL AND WHAT IS FAKE
// -----------------------------
// Fake: the sensor layer, the battery ADC, the charger pin. Nothing else.
//
// Real, and shared line-for-line with the production firmware: WiFi bring-up and
// reconnection (net.cpp, unchanged), and the entire Supabase uplink
// (telemetry.cpp, unchanged) -- the same endpoints, headers, auth, Prefer flags,
// payload vocabulary, clock-free age_ms timestamps, per-device 5 s rate limit,
// offline buffering, and SQLSTATE handling for 23503/23505.
//
// That sharing is the point. telemetry.cpp was refactored so its per-device
// state is a `telemetry::Channel` and a real unit simply owns one; the simulator
// owns an array. Giving the simulator its own copy of the uplink would have been
// less work and worth nothing, because the two would drift and the copy that
// stayed correct would be the one nobody ships.
//
// DEVICE IDENTITY
// ---------------
// Nodes take consecutive ids starting at SIM_FIRST_DEVICE (BWL-002 by default),
// leaving BWL-001 to the real prototype. Every id must already exist in the
// `devices` table or the server answers 23503 and the node backs off -- which is
// itself one of the states worth seeing in the UI.
//
// Deployment metadata (area, item_slot, label, location) is NOT set from here.
// `devices` is human-managed and anon has no grant on it, correctly. Use
// supabase/deploy_devices.sql.

#pragma once

#include <Arduino.h>

#include "device_status.h"
#include "telemetry.h"

namespace fleet_sim {

// How many virtual nodes. Override with -DBOWLSTACK_FLEET_SIM=<n>; see
// platformio.ini for why the default is 14 rather than 31.
#ifndef BOWLSTACK_FLEET_SIM
#define BOWLSTACK_FLEET_SIM 14
#endif
static const uint8_t NODE_COUNT = BOWLSTACK_FLEET_SIM;

// First simulated installation. BWL-001 is the real prototype, so the fleet
// starts at 2 and runs to 1 + NODE_COUNT (BWL-032 at the default 31).
#ifndef BOWLSTACK_FLEET_FIRST
#define BOWLSTACK_FLEET_FIRST 2
#endif
static const uint8_t FIRST_DEVICE = BOWLSTACK_FLEET_FIRST;

// How often a node's dummy state is allowed to evolve. Slow on purpose: real
// bowls are consumed over a meal, not a second, and the front-end should be
// looking at something that moves at a believable pace.
static const uint32_t TICK_MS = 20000;

// ---------------------------------------------------------------------------
// One scenario per front-end behaviour that needs proving. The comment on each
// is the UI branch it exists to exercise.
// ---------------------------------------------------------------------------
enum class Scenario : uint8_t {
  Normal,             // the ordinary case: 4 bowls draining over service
  DepletesFast,       // reaches 0 early -- the shortage warning path
  Restocked,          // 0 -> 4 mid-service, so the count must be seen to RISE
  Degraded,           // dead TOP sensor: the stack may extend into it, so the
                      //   count is a LOWER BOUND -- status degraded, shown with a
                      //   warning
  DeadSensorLow,      // dead sensor BELOW the top bowl. Contiguity proves what it
                      //   cannot see, so the count is EXACT and the status is
                      //   `ok` -- with sensors_online 3 and an `unknown` level.
                      //   Ordinary production output, and without this node the
                      //   bed can never produce it, teaching the front-end the
                      //   false invariant "ok implies 4/4, no unknown"
  AllSensorsDead,     // sensors_online 0, every level `unknown`
  Discontiguous,      // a bowl above a gap: physically impossible, so the UI
                      //   must show a FAULT and refuse to show a count
  BatteryLow,         // band walks good -> medium -> low
  BatteryCritical,    // reaches critical -- the "charge or swap" alert
  BatteryAbsent,      // no cell: battery_level NULL, which is NOT a flat battery
  BatteryImplausible, // floating divider: battery_mv absurd, level NULL --
                      //   a WIRING fault the band alone would disguise
  Charging,           // charging = true throughout
  GoesQuiet,          // reports, then stops -- `offline` must raise the alarm
  Flapping,           // changes fast; exercises the 5 s per-device rate limit
                      //   and proves history keeps its resolution
  COUNT
};

const char *scenarioName(Scenario s);

// Brings up all nodes. Call after net::begin() and telemetry::begin().
void begin();

// Call from loop(). Evolves node state on TICK_MS, then services the uplink --
// one node per call, so 31 nodes never turn into 31 concurrent HTTP requests.
void loop();

// One line per node, for the serial console.
void report();

}  // namespace fleet_sim
