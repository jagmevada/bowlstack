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
  Serial.printf("presence: below %.0f mm = present, above %.0f mm = absent, "
                "hold between\n",
                config::PRESENT_BELOW_MM, config::ABSENT_ABOVE_MM);
}

void update(const SensorArray &sensors, const BowlLogic &logic) {
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

  // One plotter line carrying everything: <name> distance, <name>_ok sensor
  // validity, <name>_p the thresholded presence, and the stack count. Plotting
  // distance against presence together is what makes the hysteresis visible --
  // you can watch a reading cross a threshold and see whether the state
  // follows or holds.
  Serial.print(">");
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    Serial.printf("%s:%.2f,", config::SENSORS[i].name, r[i].distanceMm);
  }
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    Serial.printf("%s_ok:%u,", config::SENSORS[i].name, r[i].valid ? 1 : 0);
  }
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    Serial.printf("%s_p:%u,", config::SENSORS[i].name,
                  logic.level(i) == LevelState::Present ? 1 : 0);
  }
  Serial.printf("count:%u\r\n", logic.count());

  static uint32_t lastStatus = 0;
  if (now - lastStatus >= 1000) {
    lastStatus = now;
    for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
      if (sensors.state(i) != SensorState::Online) continue;
      if (r[i].valid) {
        Serial.printf("%-3s %8.2f mm  stdev=%5.2f  %u samples  %s\n",
                      config::SENSORS[i].name, r[i].distanceMm, r[i].stdevMm,
                      r[i].samples, BowlLogic::stateName(logic.level(i)));
      } else {
        Serial.printf("%-3s   no target                        %s\n",
                      config::SENSORS[i].name,
                      BowlLogic::stateName(logic.level(i)));
      }
    }
  }
}

}  // namespace debug_plot

#endif  // BOWLSTACK_DEBUG_PLOT
