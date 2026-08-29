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

  // INPUT_PULLDOWN, not plain INPUT: with only a 10k series resistor from the
  // charger there is nothing else to define the idle state, and an undriven pin
  // would report charging at random whenever the charger was unplugged. The
  // pull-down's loose tolerance does not matter here because it never sets the
  // charging-state voltage -- the input clamp does.
  pinMode(config::PIN_CHARGING, INPUT_PULLDOWN);
}

// Filtering and hysteresis state. Single instances, owned by this module and
// touched only from sample(), which the sensor task calls -- so no locking.
static battery::Monitor monitor_;
static battery::ChargeDebounce chargeDebounce_;
static uint16_t pinMv_ = 0;
static uint32_t nextSampleMs_ = 0;
static bool everSampled_ = false;

// Majority vote across a few back-to-back reads. This rejects an isolated noise
// spike, and that is ALL it does: the samples are microseconds apart, so a
// contact that is genuinely bouncing gives the same wrong answer to all five.
// The time-domain debounce in battery::ChargeDebounce is what handles that.
static bool readChargingRaw() {
  uint8_t high = 0;
  for (uint8_t i = 0; i < config::CHARGING_SAMPLES; i++) {
    if (digitalRead(config::PIN_CHARGING) == HIGH) high++;
  }
  const bool pinHigh = (high * 2 > config::CHARGING_SAMPLES);
  return config::CHARGING_ACTIVE_LOW ? !pinHigh : pinHigh;
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

// Reads the power inputs at BATTERY_SAMPLE_INTERVAL_MS and feeds the filters.
//
// Rate-limited because sample() is called from the sensor task every 2 ms,
// while readPinMv() costs ~1.6 ms of ADC conversions: measuring on every call
// spent most of the measurement task tracking a quantity that moves over hours.
// The interval is also what makes the EMA a filter rather than decoration --
// averaging samples taken 60 us apart largely averages the same noise
// excursion, whereas 100 ms apart they are independent.
//
// Presence bounds, curve interpolation and band selection all live inside
// battery::Monitor now, so there is exactly one place where a millivolt reading
// becomes a band.
static void servicePower() {
  const uint32_t now = millis();
  if (everSampled_ && (int32_t)(now - nextSampleMs_) < 0) return;
  nextSampleMs_ = now + config::BATTERY_SAMPLE_INTERVAL_MS;
  everSampled_ = true;

  pinMv_ = readPinMv();
  monitor_.update((uint16_t)lroundf((float)pinMv_ * config::BATTERY_DIVIDER), now);
  chargeDebounce_.update(readChargingRaw(), now, config::CHARGING_DEBOUNCE_MS);
}

DeviceStatus sample(const SensorArray &sensors, const BowlLogic &logic) {
  DeviceStatus s;

  s.deviceId = BOWLSTACK_DEVICE_ID;
  s.firmware = BOWLSTACK_FW_VERSION;
  snprintf(s.mac, sizeof(s.mac), "%s", WiFi.macAddress().c_str());

  s.uptimeSec = millis() / 1000;

  servicePower();
  s.batteryMv = monitor_.millivolts();

  // Derived back from the filtered cell figure rather than reported raw, so the
  // two numbers on the console are consistent with each other and with the
  // divider constant. Printing a raw pin voltage beside a filtered cell voltage
  // makes them disagree by more than the rounding, which is confusing in the
  // one situation the pin figure exists for: comparing against a multimeter
  // while deriving BATTERY_DIVIDER.
  s.batteryPinMv =
      (uint16_t)lroundf((float)s.batteryMv / config::BATTERY_DIVIDER);

  s.batteryPercent = monitor_.percent();
  s.batteryLevel = monitor_.level();
  s.charging = chargeDebounce_.state();

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
  if (a.charging != b.charging) return true;

  // The band as the hysteresis DECIDED it -- not levelFromSoc() recomputed from
  // the percentage, which is what this used to do. Recomputing threw away the
  // state that makes the classification stable, so a cell resting on a boundary
  // alternated bands on ADC noise alone and every alternation became a report
  // and a Supabase write. Measured: dozens of change reports per minute from a
  // battery that was simply sitting at 35%.
  if (a.batteryLevel != b.batteryLevel) return true;

  // The PERCENTAGE is deliberately not compared, even after its deadband. Only
  // the band is published, so a percentage-only change produces a byte-identical
  // payload -- it would be a network write that says nothing. The deadbanded
  // percentage still exists for the console, where it is read by a person.
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    if (a.levels[i] != b.levels[i]) return true;
    if (a.sensorOnline[i] != b.sensorOnline[i]) return true;
  }
  return false;
}

void printPower(const DeviceStatus &s) {
  // The pin voltage is printed alongside the cell voltage on purpose: it is
  // what you compare against a multimeter to derive BATTERY_DIVIDER, and an
  // implausible value there identifies a divider fault that the scaled figure
  // would disguise as a merely flat battery.
  if (s.batteryLevel == battery::Level::Unknown) {
    // Distinguish the two ways a reading can be invalid: too low is an open or
    // unconnected input, too high means the divider is wrong or the pin is
    // floating high. Lumping them together as "no cell" would have sent
    // someone hunting for a dead battery when the fault was in the wiring.
    //
    // Discriminated against the PRESENT threshold, not the implausible one.
    // Both bounds are hysteretic, so a reading recovering from the high fault
    // sits between 4300 and 4400 while still classified unknown -- testing
    // against 4400 would call that "no cell detected" and point the diagnosis
    // at the opposite end of the circuit.
    const char *why = (s.batteryMv >= config::BATTERY_PRESENT_ABOVE_MV)
                          ? "IMPLAUSIBLE - check divider / floating pin"
                          : "no cell detected";
    Serial.printf("battery: pin %u mV -> cell %u mV : %s  charging=%s\n",
                  s.batteryPinMv, s.batteryMv, why, s.charging ? "yes" : "no");
    return;
  }
  // The percentage is shown HERE but not published: locally it is the number
  // you calibrate against, upstream it would imply precision the measurement
  // does not have. The band beside it is the hysteresed one, so this line shows
  // what was actually sent -- a percentage a point or two past a boundary while
  // the band has not yet flipped is the hysteresis working, not a discrepancy.
  Serial.printf("battery: pin %u mV -> cell %u mV : %d%% -> %s  charging=%s\n",
                s.batteryPinMv, s.batteryMv, s.batteryPercent,
                battery::levelName(s.batteryLevel), s.charging ? "yes" : "no");
}

void print(const DeviceStatus &s) {
  Serial.printf("[%s] fw=%s mac=%s up=%us\n", s.deviceId, s.firmware, s.mac,
                s.uptimeSec);

  printPower(s);

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
