#include "debug_plot.h"

#if BOWLSTACK_DEBUG_PLOT

#include "bowl_logic.h"

namespace debug_plot {

void begin(uint8_t liveSensors) {
  // Report integration from the samples that actually survive trimming, not
  // the raw window -- the discarded samples contribute nothing, and quoting
  // the window length overstates it by AVG_WINDOW/(AVG_WINDOW-2*TRIM).
  const uint32_t effectiveSamples = config::AVG_WINDOW - 2 * config::TRIM;
  // %u not %lu: uint32_t is 'unsigned int' on this target, so %lu is a
  // mismatched format and formally undefined, even though both are 32 bits.
  Serial.printf("Bowlstack: %u/%u sensors up, %u ms integration "
                "(%u x %u ms), %.1f Hz\n",
                liveSensors, config::SENSOR_COUNT,
                (config::TIMING_BUDGET_US * effectiveSamples) / 1000,
                effectiveSamples, config::TIMING_BUDGET_US / 1000,
                1000.0f / config::OUTPUT_PERIOD_MS);
  Serial.printf("presence: below %.0f mm = present, above %.0f mm = absent, "
                "hold between\n",
                config::PRESENT_BELOW_MM, config::ABSENT_ABOVE_MM);
}

void update(const tasks::PlotFrame &f) {
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

  // Built into one buffer and written once. Now that several tasks share the
  // UART, emitting this line as a dozen separate printf calls would let another
  // task interleave mid-line -- and a broken '>' line is a corrupt sample to
  // the plotter, not merely untidy output.
  char line[320];
  int n = snprintf(line, sizeof(line), ">");

  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    n += snprintf(line + n, sizeof(line) - n, "%s:%.2f,",
                  config::SENSORS[i].name, f.distanceMm[i]);
  }
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    n += snprintf(line + n, sizeof(line) - n, "%s_ok:%u,",
                  config::SENSORS[i].name, f.valid[i] ? 1 : 0);
  }
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    n += snprintf(line + n, sizeof(line) - n, "%s_p:%u,",
                  config::SENSORS[i].name,
                  f.level[i] == LevelState::Present ? 1 : 0);
  }
  snprintf(line + n, sizeof(line) - n, "count:%u\r\n", f.stackCount);
  Serial.print(line);

  static uint32_t lastStatus = 0;
  if (now - lastStatus >= 1000) {
    lastStatus = now;
    for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
      if (f.state[i] != SensorState::Online) continue;
      if (f.valid[i]) {
        Serial.printf("%-3s %8.2f mm  stdev=%5.2f  %u samples  %s\n",
                      config::SENSORS[i].name, f.distanceMm[i], f.stdevMm[i],
                      f.samples[i], BowlLogic::stateName(f.level[i]));
      } else {
        Serial.printf("%-3s   no target                        %s\n",
                      config::SENSORS[i].name,
                      BowlLogic::stateName(f.level[i]));
      }
    }
  }
}

}  // namespace debug_plot

#endif  // BOWLSTACK_DEBUG_PLOT
