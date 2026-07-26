#include "indicators.h"

#include "bowl_logic.h"
#include "config.h"

namespace indicators {
namespace {

// Common anode: the LED sits between VCC and the pin, so pulling the pin LOW
// completes the circuit. Naming the states removes the constant risk of
// getting the polarity backwards at a call site.
const uint8_t LED_ON = LOW;
const uint8_t LED_OFF = HIGH;

void set(uint8_t pin, bool on) { digitalWrite(pin, on ? LED_ON : LED_OFF); }

}  // namespace

void begin() {
  // Drive OFF before configuring as output, so the pin never briefly asserts
  // LOW (LED on) between pinMode and the first write.
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    digitalWrite(config::LED_LEVEL[i], LED_OFF);
    pinMode(config::LED_LEVEL[i], OUTPUT);
    digitalWrite(config::LED_LEVEL[i], LED_OFF);
  }
  digitalWrite(config::LED_HEALTH, LED_OFF);
  pinMode(config::LED_HEALTH, OUTPUT);
  digitalWrite(config::LED_HEALTH, LED_OFF);
}

void selfTest() {
  // Sweep each LED in turn, then flash all together. Worth the second it costs:
  // it proves every indicator is wired and the polarity is right, at the one
  // moment someone is watching the device. Without it a dead LED is
  // indistinguishable from an absent bowl, and an inverted one from a present
  // one -- both silent failures of the only status a person at the station can
  // read.
  Serial.println("leds: self-test (f1 f2 f3 f4 health)");

  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    set(config::LED_LEVEL[i], true);
    vTaskDelay(pdMS_TO_TICKS(200));
    set(config::LED_LEVEL[i], false);
  }
  set(config::LED_HEALTH, true);
  vTaskDelay(pdMS_TO_TICKS(200));
  set(config::LED_HEALTH, false);

  vTaskDelay(pdMS_TO_TICKS(150));
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) set(config::LED_LEVEL[i], true);
  set(config::LED_HEALTH, true);
  vTaskDelay(pdMS_TO_TICKS(400));
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) set(config::LED_LEVEL[i], false);
  set(config::LED_HEALTH, false);
}

void update(const tasks::PlotFrame &f, bool sensorFault, bool wifiDown) {
  // Level LEDs follow the debounced presence decision, not the raw distance --
  // the same value the bowl count is built from. An LED that flickered while
  // the count stayed put would be actively misleading about what the device
  // believes.
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    set(config::LED_LEVEL[i], f.level[i] == LevelState::Present);
  }

  // Health. WiFi wins when both faults are present: a sensor fault still
  // reaches the server tagged as a fault, whereas a dead link means nothing
  // downstream sees anything at all.
  if (!wifiDown && !sensorFault) {
    set(config::LED_HEALTH, true);  // solid
    return;
  }

  const float hz = wifiDown ? config::HEALTH_BLINK_WIFI_HZ
                            : config::HEALTH_BLINK_SENSOR_HZ;

  // A full cycle is one on and one off period, so a half-period toggle gives
  // the stated frequency.
  const uint32_t halfPeriodMs = (uint32_t)(500.0f / hz);
  const bool phase = ((millis() / halfPeriodMs) & 1U) == 0U;
  set(config::LED_HEALTH, phase);
}

}  // namespace indicators
