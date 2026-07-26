// TEST HARNESS ONLY -- not production behaviour.
//
// Streams readings in the Serial Plotter extension's format for bring-up and
// validation. Enabled only when BOWLSTACK_DEBUG_PLOT=1 (the esp32dev-debug
// build environment); otherwise every entry point below compiles to an empty
// inline, so a production image carries no formatting cost and emits no plot
// traffic. Callers never need an #ifdef.
//
// Renders from a tasks::PlotFrame copied out of the sensor task, never from a
// live SensorArray: the driver is single-owner by design, and reading it from
// the debug task would race the ranging loop.
//
// Not to be confused with the telemetry uplink, which posts to Supabase and IS
// production behaviour.

#pragma once

#include <Arduino.h>

#include "config.h"
#include "tasks.h"

namespace debug_plot {

#if BOWLSTACK_DEBUG_PLOT

// Prints the tuning and threshold banner. Call once after the sensors are up.
void begin(uint8_t liveSensors);

// Emits one plotter line per OUTPUT_PERIOD_MS and a once-a-second heartbeat.
// Call often; it paces itself.
void update(const tasks::PlotFrame &f);

#else

inline void begin(uint8_t) {}
inline void update(const tasks::PlotFrame &) {}

#endif

}  // namespace debug_plot
