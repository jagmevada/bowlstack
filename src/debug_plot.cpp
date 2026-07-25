#include "debug_plot.h"

#if BOWLSTACK_DEBUG_PLOT

namespace debug_plot {

void begin(const SensorArray &sensors) {
  // Report integration from the samples that actually survive trimming, not
  // the raw window -- the discarded samples contribute nothing, and quoting
  // the window length overstates it by AVG_WINDOW/(AVG_WINDOW-2*TRIM).
  const uint32_t effectiveSamples = config::AVG_WINDOW - 2 * config::TRIM;
  // %u not %lu: uint32_t is 'unsigned int' on this target, so %lu is a
  // mismatched format and formally undefined, even though both are 32 bits.
  Serial.printf("Bowlstack: %u/%u sensors live, %u ms integration "
                "(%u x %u ms), %.1f Hz\n",
                sensors.onlineCount(), config::SENSOR_COUNT,
                (config::TIMING_BUDGET_US * effectiveSamples) / 1000,
                effectiveSamples, config::TIMING_BUDGET_US / 1000,
                1000.0f / config::OUTPUT_PERIOD_MS);
}

void update(const SensorArray &sensors) {
  // Emit on a fixed grid rather than "at least OUTPUT_PERIOD_MS since the last
  // output". The latter quantises to whole sample periods and would silently
  // settle at half the intended rate.
  static uint32_t nextOutput = 0;
  const uint32_t now = millis();
  if ((int32_t)(now - nextOutput) < 0) return;
  nextOutput += config::OUTPUT_PERIOD_MS;
  if ((int32_t)(now - nextOutput) > 0) {
    nextOutput = now + config::OUTPUT_PERIOD_MS;  // resync after a stall
  }

  Reading r[config::SENSOR_COUNT];
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) r[i] = sensors.reading(i);

  // One plotter line carrying every sensor. <name> is distance, <name>_ok is
  // validity -- the _ok channel is the one to trust, since the distance is
  // pegged to NO_TARGET_MM when invalid, which is a floor and not a
  // measurement.
  Serial.print(">");
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    Serial.printf("%s:%.2f,", config::SENSORS[i].name, r[i].distanceMm);
  }
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    Serial.printf("%s_ok:%u%s", config::SENSORS[i].name, r[i].valid ? 1 : 0,
                  (i == config::SENSOR_COUNT - 1) ? "\r\n" : ",");
  }

  static uint32_t lastStatus = 0;
  if (now - lastStatus >= 1000) {
    lastStatus = now;
    for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
      if (sensors.state(i) != SensorState::Online) continue;
      if (r[i].valid) {
        Serial.printf("%-3s %8.2f mm  stdev=%5.2f  %u samples\n",
                      config::SENSORS[i].name, r[i].distanceMm, r[i].stdevMm,
                      r[i].samples);
      } else {
        Serial.printf("%-3s   no target\n", config::SENSORS[i].name);
      }
    }
  }
}

}  // namespace debug_plot

#endif  // BOWLSTACK_DEBUG_PLOT
