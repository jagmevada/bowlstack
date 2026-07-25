// ---------------------------------------------------------------------------
// Bowlstack -- counts how many food bowls are stacked on a serving station.
//
// Four VL53L0X ToF sensors down a 6 ft pipe, one per bowl level at a 1.2 ft
// pitch, split across two hardware I2C buses. See README.md for the geometry,
// the mounting constraint, and the stack-contiguity rule.
//
// Layering, so the phases still to come drop in without disturbing each other:
//
//   config          pins and tuning only; depends on nothing
//   version         device identity and firmware version
//   trimmed_window  pure statistics; no hardware
//   sensor_array    owns the drivers, exposes Reading values
//   bowl_logic      Reading -> presence -> stack count
//   device_status   the full report: identity, power, health, count
//   debug_plot      test harness, compiled out unless BOWLSTACK_DEBUG_PLOT=1
//
// Still to come: wifi and the Supabase uplink (device_status::sample already
// produces its payload, and ::differs already answers "send immediately?"),
// plus a fault policy driving SensorArray::recover().
// ---------------------------------------------------------------------------

#include <Arduino.h>

#include "bowl_logic.h"
#include "config.h"
#include "debug_plot.h"
#include "device_status.h"
#include "sensor_array.h"
#include "version.h"

// Periodic report cadence. Phase 3 posts on this same schedule, plus
// immediately whenever device_status::differs() reports a change.
static const uint32_t REPORT_PERIOD_MS = 10000;

static SensorArray sensors;
static BowlLogic logic;
static DeviceStatus lastReported;
static bool everReported = false;

void setup() {
  Serial.begin(config::SERIAL_BAUD);
  delay(200);  // let the USB-serial link settle before the first banner

  Serial.printf("\nBowlstack %s  device=%s\n", BOWLSTACK_FW_VERSION,
                BOWLSTACK_DEVICE_ID);

  device_status::begin();
  sensors.begin();

  if (sensors.onlineCount() == 0) {
    Serial.println("no sensors responded - check I2C0 SDA=17/SCL=16, "
                   "I2C1 SDA=21/SCL=22, XSHUT wiring, 3V3");
    sensors.printDiagnostics();
  }

  debug_plot::begin(sensors);
}

void loop() {
  sensors.poll();
  logic.update(sensors);
  debug_plot::update(sensors, logic);

  // Report on a fixed cadence, or immediately on any change worth knowing
  // about -- a bowl added or removed must not wait out the period.
  const DeviceStatus now = device_status::sample(sensors, logic);
  const bool changed = everReported && device_status::differs(now, lastReported);

  static uint32_t nextReport = 0;
  const uint32_t ms = millis();
  const bool due = (int32_t)(ms - nextReport) >= 0;

  if (changed || due || !everReported) {
    nextReport = ms + REPORT_PERIOD_MS;
    lastReported = now;
    everReported = true;

    Serial.println(changed ? "--- report (change) ---" : "--- report ---");
    device_status::print(now);
  }
}
