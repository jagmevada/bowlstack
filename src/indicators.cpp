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
