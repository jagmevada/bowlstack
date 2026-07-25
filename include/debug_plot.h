// TEST HARNESS ONLY -- not production behaviour.
//
// Streams readings in the Serial Plotter extension's format for bring-up and
// validation. Enabled only when BOWLSTACK_DEBUG_PLOT=1 (the esp32dev-debug
// build environment); otherwise every entry point below compiles to an empty
// inline, so a production image carries no formatting cost and emits no plot
// traffic. Callers never need an #ifdef.
//
// Not to be confused with the Phase 3 telemetry uplink, which posts to
// Supabase and IS production behaviour.

#pragma once

#include <Arduino.h>

#include "config.h"
#include "sensor_array.h"

namespace debug_plot {

#if BOWLSTACK_DEBUG_PLOT

// Prints the tuning banner. Call once after SensorArray::begin().
void begin(const SensorArray &sensors);

// Emits one plotter line per OUTPUT_PERIOD_MS and a once-a-second
// human-readable heartbeat. Call every loop; it paces itself.
void update(const SensorArray &sensors);

#else

inline void begin(const SensorArray &) {}
inline void update(const SensorArray &) {}

#endif

}  // namespace debug_plot
