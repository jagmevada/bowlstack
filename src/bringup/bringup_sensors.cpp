// The sensor half of the bring-up harness: the real SensorArray and BowlLogic
// from the discrete build, running on the Waveshare pins, feeding the UI.
//
// NOTHING HERE IS NEW LOGIC. sensor_array, bowl_logic and trimmed_window are
// the files from main, unmodified; only config.cpp's pin table gained a branch.
// That is the point of the port -- the measurement behaviour that was verified
// on hardware (four sensors at 10 Hz, stdev 1-5 mm at ~1 m, the contiguity rule
// checked exhaustively against all 81 level combinations) carries over intact,
// and what changes is which GPIOs it talks to.
//
// WITH NO SENSORS ATTACHED THIS REPORTS EVERY LEVEL OFFLINE AND UNKNOWN, and
// that is the correct output rather than a failure. It also proves the whole
// chain compiles, links, addresses and polls on the new board -- which is
// everything except the part that needs hardware in hand.
//
// The adapter to ui::State is the interesting bit: it is where the firmware's
// vocabulary meets the screen's, and it is deliberately a translation rather
// than a shared type. See ui_state.h.

#include <Arduino.h>

#include "bowl_logic.h"
#include "bringup_sensors.h"
#include "sensor_array.h"
#include "ui_demo.h"
#include "ui_state.h"
#include "version.h"

namespace bringup_sensors {
namespace {

SensorArray sensors_;
BowlLogic logic_;
bool ready_ = false;
uint32_t nextPublishMs_ = 0;

ui::Level toUiLevel(LevelState s) {
  switch (s) {
    case LevelState::Present: return ui::Level::Present;
    case LevelState::Absent: return ui::Level::Absent;
    default: return ui::Level::Unknown;
  }
}

ui::Stack toUiStack(StackStatus s) {
  switch (s) {
    case StackStatus::Ok: return ui::Stack::Ok;
    case StackStatus::Discontiguous: return ui::Stack::Discontiguous;
    default: return ui::Stack::Degraded;
  }
}

}  // namespace

void begin() {
  Serial.println("\n--- sensors ---");
  sensors_.begin();

  const uint8_t init = sensors_.initialisedCount();
  Serial.printf("  %u/%u initialised on Wire1 (SDA=%u SCL=%u)\n", init,
                config::SENSOR_COUNT, config::I2C1_SDA, config::I2C1_SCL);

  if (init == 0) {
    // Expected with nothing wired, so it says so rather than reading as a
    // fault. The XSHUT walk having run at all is the useful signal here.
    Serial.println("  none responded - expected with no sensors attached.");
    Serial.printf("  when wiring: SDA=%u SCL=%u, XSHUT f1..f4 = %u/%u/%u/%u,\n",
                  config::I2C1_SDA, config::I2C1_SCL, config::SENSORS[0].xshutPin,
                  config::SENSORS[1].xshutPin, config::SENSORS[2].xshutPin,
                  config::SENSORS[3].xshutPin);
    Serial.println("  3V3 and GND from header P2 (P1 carries only GND and 5V).");
  } else {
    sensors_.printDiagnostics();
  }
  ready_ = true;
}

void loop(uint32_t nowMs) {
  if (!ready_) return;

  // Polled every iteration, exactly as the discrete build's sensorTask does.
  // poll() is non-blocking: it services whichever sensors already have a result
  // waiting and returns, so it costs nothing when none do.
  sensors_.poll();
  logic_.update(sensors_);

  // Published at 5 Hz rather than per iteration. The UI's change detection
  // would discard most of it anyway, and the count only moves when a bowl does.
  if ((int32_t)(nowMs - nextPublishMs_) < 0) return;
  nextPublishMs_ = nowMs + 200;

  ui::State s = ui::demoLatest(nowMs);

  uint8_t online = 0;
  for (uint8_t i = 0; i < ui::LEVELS; i++) {
    s.levels[i] = toUiLevel(logic_.level(i));
    s.sensorOnline[i] = (sensors_.state(i) == SensorState::Online);
    if (s.sensorOnline[i]) online++;
  }
  s.sensorsOnline = online;
  s.stackCount = logic_.count();
  s.stack = toUiStack(logic_.status());
  s.deviceId = BOWLSTACK_DEVICE_ID;
  s.firmware = BOWLSTACK_FW_VERSION;

  // NO CLOCK ON THIS BOARD. There is no RTC and nothing has supplied a time, so
  // the status bar shows "--:--" rather than a number. docs/supabase.md already
  // builds the whole telemetry design around the device having no reliable
  // time; the screen should not be the one place that pretends otherwise.
  s.timeKnown = false;

  ui::demoOverrideState(s);
}

const SensorArray &array() { return sensors_; }

}  // namespace bringup_sensors
