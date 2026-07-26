// ---------------------------------------------------------------------------
// Bowlstack -- counts how many food bowls are stacked on a serving station.
//
// Four VL53L0X ToF sensors down a 6 ft pipe, one per bowl level at a 1.2 ft
// pitch, split across two hardware I2C buses. See README.md for the geometry,
// the mounting constraint, and the stack-contiguity rule.
//
// Layering:
//
//   config          pins and tuning only; depends on nothing
//   version         device identity and firmware version
//   trimmed_window  pure statistics; no hardware
//   sensor_array    owns the drivers, exposes Reading values
//   bowl_logic      Reading -> presence -> stack count
//   device_status   the full report: identity, power, health, count
//   net             WiFi join, captive portal, reconnection
//   telemetry       Supabase uplink (production)
//   debug_plot      test harness, compiled out unless BOWLSTACK_DEBUG_PLOT=1
//   tasks           the FreeRTOS fabric that runs all of the above
//
// setup() creates the tasks and returns. Each task initialises its own
// subsystem, so nothing here blocks -- notably net::begin(), whose scan and
// joins used to delay the first sensor reading by seconds.
// ---------------------------------------------------------------------------

#include <Arduino.h>

#include "config.h"
#include "tasks.h"
#include "version.h"

// How often to report task stack headroom. A task running out of stack on
// ESP32 manifests as a corrupt-looking crash far from the real cause, so the
// margin is worth watching while the task layout is still settling.
static const uint32_t STACK_REPORT_MS = 60000;

void setup() {
  Serial.begin(config::SERIAL_BAUD);
  delay(200);  // let the USB-serial link settle before the first banner

  Serial.printf("\nBowlstack %s  device=%s\n", BOWLSTACK_FW_VERSION,
                BOWLSTACK_DEVICE_ID);

  tasks::start();
}

void loop() {
  // The Arduino loop task has nothing left to do -- all work runs in the tasks
  // created above. It stays alive only to report stack headroom; deleting it
  // would save a few hundred bytes but lose a cheap early warning.
  tasks::printStackHeadroom();
  vTaskDelay(pdMS_TO_TICKS(STACK_REPORT_MS));
}
