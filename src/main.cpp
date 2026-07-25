// ---------------------------------------------------------------------------
// Bowlstack -- counts how many food bowls are stacked on a serving station.
//
// Four VL53L0X ToF sensors down a 6 ft pipe, one per bowl level at a 1.2 ft
// pitch, split across two hardware I2C buses. See README.md for the geometry,
// the mounting constraint, and the stack-contiguity rule.
//
// Layering, so the phases still to come drop in without disturbing each other:
//
//   config        pins and tuning only; depends on nothing
//   trimmed_window  pure statistics; no hardware
//   sensor_array  owns the drivers, exposes Reading values
//   debug_plot    test harness, compiled out unless BOWLSTACK_DEBUG_PLOT=1
//
// Still to come: bowl_logic (presence thresholds + contiguity), fault
// detection driving SensorArray::recover(), wifi, and the Supabase uplink.
// ---------------------------------------------------------------------------

#include <Arduino.h>

#include "config.h"
#include "debug_plot.h"
#include "sensor_array.h"

static SensorArray sensors;

void setup() {
  Serial.begin(config::SERIAL_BAUD);
  delay(200);  // let the USB-serial link settle before the first banner

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
  debug_plot::update(sensors);
}
