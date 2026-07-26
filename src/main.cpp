// ---------------------------------------------------------------------------
// Bowlstack -- counts how many food bowls are stacked on a serving station.
//
// Four VL53L0X ToF sensors down a 6 ft pipe, one per bowl level at a 1.2 ft
// pitch, split across two hardware I2C buses. See README.md for the geometry,
// the mounting constraint, and the stack-contiguity rule.
//
// Layering, so each phase stays independent:
//
//   config          pins and tuning only; depends on nothing
//   version         device identity and firmware version
//   trimmed_window  pure statistics; no hardware
//   sensor_array    owns the drivers, exposes Reading values
//   bowl_logic      Reading -> presence -> stack count
//   device_status   the full report: identity, power, health, count
//   net             WiFi bring-up and upkeep
//   telemetry       Supabase uplink (production)
//   debug_plot      test harness, compiled out unless BOWLSTACK_DEBUG_PLOT=1
//
// Measurement never waits on the network: sensors.poll() and logic.update()
// run every iteration regardless of link state, and telemetry buffers.
// ---------------------------------------------------------------------------

#include <Arduino.h>

#include "bowl_logic.h"
#include "config.h"
#include "debug_plot.h"
#include "device_status.h"
#include "net.h"
#include "sensor_array.h"
#include "telemetry.h"
#include "version.h"

// How long to let the sensors settle before the first report. Reporting on the
// very first loop iteration publishes an all-UNKNOWN / DEGRADED snapshot that
// is superseded within a second -- noise in the history, and actively
// misleading if a coordinator reads it. Sensors normally conclude in a few
// hundred ms, so this ceiling is only reached when one is genuinely absent.
static const uint32_t BOOT_SETTLE_MS = 5000;

static SensorArray sensors;
static BowlLogic logic;
static DeviceStatus lastReported;
static bool everReported = false;
static uint32_t bootSettleDeadline = 0;

void setup() {
  Serial.begin(config::SERIAL_BAUD);
  delay(200);  // let the USB-serial link settle before the first banner

  Serial.printf("\nBowlstack %s  device=%s\n", BOWLSTACK_FW_VERSION,
                BOWLSTACK_DEVICE_ID);

  device_status::begin();
  sensors.begin();

  // initialisedCount, not onlineCount: begin() leaves every healthy sensor in
  // Warming until it concludes its first measurement, so onlineCount() is
  // legitimately 0 here. Testing it would print a bus fault -- and the
  // "XSHUT is not wired" diagnosis that follows -- for a perfectly working
  // array that had just reported all four ready.
  if (sensors.initialisedCount() == 0) {
    Serial.println("no sensors responded - check I2C0 SDA=17/SCL=16, "
                   "I2C1 SDA=21/SCL=22, XSHUT wiring, 3V3");
    sensors.printDiagnostics();
  }

  net::begin();
  telemetry::begin();

  debug_plot::begin(sensors);

  // Started here, not at sensors.begin(): net::begin() still blocks briefly on
  // its join attempts, and that time is not sensor settling time.
  bootSettleDeadline = millis() + BOOT_SETTLE_MS;
}

void loop() {
  // Measurement path. Runs unconditionally -- a network outage must never
  // stall or corrupt the bowl count.
  sensors.poll();
  logic.update(sensors);
  debug_plot::update(sensors, logic);

  net::loop();

  const DeviceStatus now = device_status::sample(sensors, logic);

  // History is written only on a real change, which is what keeps the events
  // table bounded; the heartbeat upsert refreshes current state without
  // appending anything.
  if (!everReported) {
    // Report once every sensor has reached a conclusion, or once the grace
    // period expires -- whichever comes first, so a dead sensor cannot hold
    // the first report back indefinitely.
    const bool settled = (sensors.onlineCount() == config::SENSOR_COUNT);
    const bool graceOver = (int32_t)(millis() - bootSettleDeadline) >= 0;
    if (settled || graceOver) {
      telemetry::enqueue(now, telemetry::Reason::Boot);
      Serial.printf("--- report (boot%s) ---\n", settled ? "" : ", not settled");
      device_status::print(now);
      lastReported = now;
      everReported = true;
    }
  } else if (device_status::differs(now, lastReported)) {
    telemetry::enqueue(now, telemetry::Reason::Change);
    telemetry::requestImmediateUpsert();
    Serial.println("--- report (change) ---");
    device_status::print(now);
    lastReported = now;
  }

  telemetry::loop(now);
}
