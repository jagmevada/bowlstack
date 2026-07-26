#include "device_status.h"

#include <WiFi.h>  // for the MAC only; no radio is started here
#include <math.h>

#include "battery_soc.h"
#include "version.h"

namespace device_status {

void begin() {
  // 11 dB attenuation puts full scale near 3.3 V, which covers a single Li-ion
  // cell once the divider has halved it.
  analogSetPinAttenuation(config::PIN_BATTERY_ADC, ADC_11db);
  // Plain INPUT in the active-high case: the divider's lower resistor already
  // holds the pin at 0 V when no charger is present, and an internal pull-down
  // fighting the divider would shift the sensed level.
  pinMode(config::PIN_CHARGING, config::CHARGING_ACTIVE_LOW ? INPUT_PULLUP : INPUT);
}

static uint16_t readBatteryMv() {
  // Oversample. A single ESP32 ADC conversion carries tens of millivolts of
  // noise, which the steep end of the discharge curve turns into several
  // percent of apparent charge -- enough to flap a battery band on nothing but
  // sampling noise. Averaging 16 reads costs microseconds and cuts that ~4x.
  //
  // analogReadMilliVolts applies the chip's factory calibration, which matters
  // because the raw ADC is markedly non-linear near its rails.
  const uint8_t samples = 16;
  uint32_t sum = 0;
  for (uint8_t i = 0; i < samples; i++) {
    sum += analogReadMilliVolts(config::PIN_BATTERY_ADC);
  }
  const uint32_t atPin = sum / samples;
  return (uint16_t)(atPin * config::BATTERY_DIVIDER);
}

static int8_t batteryPercent(uint16_t mv) {
  // No cell wired, so the input floats. Report unknown rather than inventing a
  // plausible-looking 0%, which would be indistinguishable upstream from a
  // genuinely flat battery.
  if (mv < config::BATTERY_ABSENT_BELOW_MV) return -1;

  // Interpolate the MEASURED discharge curve rather than assuming a straight
  // line between 3.0 V and 4.2 V. Li-ion is markedly non-linear: the real cell
  // sits above 3.6 V for the first ~60% of its capacity and then falls away
  // sharply, so a linear fit reads roughly 20 points optimistic in mid-range
  // and collapses without warning near the end.
  return (int8_t)lroundf(battery::socFromMillivolts(mv));
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
    Serial.printf("  battery: %u mV (%d%%, %s)  charging=%s\n", s.batteryMv,
                  s.batteryPercent,
                  battery::levelName(battery::levelFromSoc(s.batteryPercent)),
                  s.charging ? "yes" : "no");
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
