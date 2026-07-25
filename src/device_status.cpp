#include "device_status.h"

#include <WiFi.h>  // for the MAC only; no radio is started here

#include "version.h"

namespace device_status {

void begin() {
  // 11 dB attenuation puts full scale near 3.3 V, which covers a single Li-ion
  // cell once the divider has halved it.
  analogSetPinAttenuation(config::PIN_BATTERY_ADC, ADC_11db);
  pinMode(config::PIN_CHARGING, config::CHARGING_ACTIVE_LOW ? INPUT_PULLUP : INPUT);
}

static uint16_t readBatteryMv() {
  // analogReadMilliVolts applies the chip's factory ADC calibration, which
  // matters here: the raw ESP32 ADC is markedly non-linear near its rails.
  const uint32_t atPin = analogReadMilliVolts(config::PIN_BATTERY_ADC);
  return (uint16_t)(atPin * config::BATTERY_DIVIDER);
}

static int8_t batteryPercent(uint16_t mv) {
  // No cell wired yet, so the input floats. Report unknown rather than
  // inventing a plausible-looking 0%, which would be indistinguishable
  // upstream from a genuinely flat battery.
  if (mv < config::BATTERY_ABSENT_BELOW_MV) return -1;

  if (mv <= config::BATTERY_MIN_MV) return 0;
  if (mv >= config::BATTERY_MAX_MV) return 100;

  const uint32_t span = config::BATTERY_MAX_MV - config::BATTERY_MIN_MV;
  return (int8_t)(((uint32_t)(mv - config::BATTERY_MIN_MV) * 100) / span);
}

DeviceStatus sample(const SensorArray &sensors, const BowlLogic &logic) {
  DeviceStatus s;

  s.deviceId = BOWLSTACK_DEVICE_ID;
  s.firmware = BOWLSTACK_FW_VERSION;
  snprintf(s.mac, sizeof(s.mac), "%s", WiFi.macAddress().c_str());

  s.uptimeSec = millis() / 1000;

  s.batteryMv = readBatteryMv();
  s.batteryPercent = batteryPercent(s.batteryMv);
  const int level = digitalRead(config::PIN_CHARGING);
  s.charging = config::CHARGING_ACTIVE_LOW ? (level == LOW) : (level == HIGH);

  s.sensorsOnline = sensors.onlineCount();
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    s.sensorOnline[i] = (sensors.state(i) == SensorState::Online);
    s.levels[i] = logic.level(i);
  }

  s.stackCount = logic.count();
  s.stackStatus = logic.status();
  return s;
}

bool differs(const DeviceStatus &a, const DeviceStatus &b) {
  if (a.stackCount != b.stackCount) return true;
  if (a.stackStatus != b.stackStatus) return true;
  if (a.charging != b.charging) return true;
  if (a.sensorsOnline != b.sensorsOnline) return true;
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    if (a.levels[i] != b.levels[i]) return true;
    if (a.sensorOnline[i] != b.sensorOnline[i]) return true;
  }
  return false;
}

void print(const DeviceStatus &s) {
  Serial.printf("[%s] fw=%s mac=%s up=%us\n", s.deviceId, s.firmware, s.mac,
                s.uptimeSec);

  if (s.batteryPercent < 0) {
    Serial.printf("  battery: no cell detected (%u mV)  charging=%s\n",
                  s.batteryMv, s.charging ? "yes" : "no");
  } else {
    Serial.printf("  battery: %u mV (%d%%)  charging=%s\n", s.batteryMv,
                  s.batteryPercent, s.charging ? "yes" : "no");
  }

  Serial.print("  levels:");
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    Serial.printf(" %s=%s%s", config::SENSORS[i].name,
                  BowlLogic::stateName(s.levels[i]),
                  s.sensorOnline[i] ? "" : "(no data)");
  }
  Serial.println();

  Serial.printf("  STACK COUNT = %u  [%s]  sensors %u/%u\n", s.stackCount,
                BowlLogic::statusName(s.stackStatus), s.sensorsOnline,
                config::SENSOR_COUNT);
}

}  // namespace device_status
