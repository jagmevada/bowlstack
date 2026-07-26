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
}

// Raw millivolts at the ADC pin, before the divider is undone. Exposed
// separately because it is the number to compare against a multimeter when
// deriving BATTERY_DIVIDER, and because a divider fault shows up here as an
// implausible pin voltage while the scaled figure still looks like a battery.
static uint16_t readPinMv() {
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
  return (uint16_t)(sum / samples);
}

static int8_t batteryPercent(uint16_t mv) {
  // No cell wired, so the input floats. Report unknown rather than inventing a
  // plausible-looking 0%, which would be indistinguishable upstream from a
  // genuinely flat battery.
  if (mv < config::BATTERY_ABSENT_BELOW_MV) return -1;

  // Bound the OTHER end too. The SoC curve clamps anything above its top point
  // to 100%, so without this a floating pin -- which swings high as readily as
  // low -- reports a healthy full battery. A reading no lithium cell can
  // produce means the measurement is broken, and saying so is the only honest
  // answer available.
  if (mv > config::BATTERY_IMPLAUSIBLE_ABOVE_MV) return -1;

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

  s.batteryPinMv = readPinMv();
  s.batteryMv = (uint16_t)(s.batteryPinMv * config::BATTERY_DIVIDER);
  s.batteryPercent = batteryPercent(s.batteryMv);

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
  if (a.sensorsOnline != b.sensorsOnline) return true;
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    if (a.levels[i] != b.levels[i]) return true;
    if (a.sensorOnline[i] != b.sensorOnline[i]) return true;
  }
  return false;
}

void printBattery(const DeviceStatus &s) {
  // The pin voltage is printed alongside the cell voltage on purpose: it is
  // what you compare against a multimeter to derive BATTERY_DIVIDER, and an
  // implausible value there identifies a divider fault that the scaled figure
  // would disguise as a merely flat battery.
  if (s.batteryPercent < 0) {
    // Distinguish the two ways a reading can be invalid: too low is an open or
    // unconnected input, too high means the divider is wrong or the pin is
    // floating high. Lumping them together as "no cell" would have sent
    // someone hunting for a dead battery when the fault was in the wiring.
    const char *why = (s.batteryMv > config::BATTERY_IMPLAUSIBLE_ABOVE_MV)
                          ? "IMPLAUSIBLE - check divider / floating pin"
                          : "no cell detected";
    Serial.printf("battery: pin %u mV -> cell %u mV : %s\n", s.batteryPinMv,
                  s.batteryMv, why);
    return;
  }
  Serial.printf("battery: pin %u mV -> cell %u mV : %d%% (%s)\n",
                s.batteryPinMv, s.batteryMv, s.batteryPercent,
                battery::levelName(battery::levelFromSoc(s.batteryPercent)));
}

void print(const DeviceStatus &s) {
  Serial.printf("[%s] fw=%s mac=%s up=%us\n", s.deviceId, s.firmware, s.mac,
                s.uptimeSec);

  printBattery(s);

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
