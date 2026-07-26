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
#include "device_status.h"
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
  // The Arduino loop task has no measurement work -- that all runs in the tasks
  // created above. It stays alive as the reporting task: stack headroom as a
  // cheap early warning, and the power line, which is deliberately here
  // rather than in debug_plot so it survives in the PRODUCTION build where the
  // plotter compiles to nothing.
  static bool armed = false;
  static uint32_t nextPower = 0;
  static uint32_t nextStack = 0;

  const uint32_t now = millis();
  if (!armed) {
    armed = true;
    nextPower = now;
    nextStack = now;
  }

  if ((int32_t)(now - nextPower) >= 0) {
    nextPower = now + config::POWER_REPORT_MS;
    if (tasks::ready()) device_status::printPower(tasks::snapshot());
  }

  if ((int32_t)(now - nextStack) >= 0) {
    nextStack = now + STACK_REPORT_MS;
    tasks::printStackHeadroom();
  }

  vTaskDelay(pdMS_TO_TICKS(100));
}
