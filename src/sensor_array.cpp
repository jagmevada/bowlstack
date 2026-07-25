#include "sensor_array.h"

namespace {

// The library's readRangeContinuousMillimeters() spins until its own sensor is
// ready, which would stall the whole round-robin on whichever sensor is
// slowest. Splitting the poll from the fetch means we only ever touch a sensor
// that already has a result waiting.

bool dataReady(VL53L0X &dev) {
  return (dev.readReg(VL53L0X::RESULT_INTERRUPT_STATUS) & 0x07) != 0;
}

uint16_t fetchRange(VL53L0X &dev) {
  const uint16_t mm = dev.readReg16Bit(VL53L0X::RESULT_RANGE_STATUS + 10);
  dev.writeReg(VL53L0X::SYSTEM_INTERRUPT_CLEAR, 0x01);
  return mm;
}

void scanBus(TwoWire &bus, const char *label) {
  Serial.printf("I2C scan %s:", label);
  bool found = false;
  for (uint8_t addr = 1; addr < 127; addr++) {
    bus.beginTransmission(addr);
    if (bus.endTransmission() == 0) {
      Serial.printf(" 0x%02X", addr);
      found = true;
    }
  }
  Serial.println(found ? "" : " nothing found");
}

}  // namespace

// XSHUT is pulled up to 3.3 V on every breakout, so enabling means releasing
// the line to high-Z and letting the pull-up do it. Never drive it high: the
// bare sensor's XSHUT is a 2.8 V input.

void SensorArray::shutdown(uint8_t level) {
  pinMode(config::SENSORS[level].xshutPin, OUTPUT);
  digitalWrite(config::SENSORS[level].xshutPin, LOW);
}

void SensorArray::enable(uint8_t level) {
  pinMode(config::SENSORS[level].xshutPin, INPUT);  // pull-up drives it high
}

bool SensorArray::initSensor(uint8_t level) {
  const config::SensorConfig &cfg = config::SENSORS[level];
  Channel &c = ch_[level];

  enable(level);
  delay(10);  // datasheet t_boot is 1.2 ms; 10 ms is comfortable

  // Rebuild the driver object before every init. The library caches the
  // address it last assigned in a private member, and init() probes
  // IDENTIFICATION_MODEL_ID through that cached value. An XSHUT reset reverts
  // the hardware to 0x29, so re-initialising a sensor that had already been
  // moved to 0x30..0x33 would have the driver interrogating an address nothing
  // answers on -- init() fails, the sensor gets parked, and recover() would
  // permanently kill exactly the channels it exists to restore. The library
  // exposes no way to reset that member, so construct a fresh one.
  c.dev = VL53L0X();
  c.dev.setBus(cfg.bus);
  c.dev.setTimeout(500);

  if (!c.dev.init()) {
    // Park the failed sensor back in reset before the caller releases the
    // next one. Leaving it awake would strand it at 0x29, and the next sensor
    // on the same bus also answers at 0x29 -- two devices, one address.
    shutdown(level);
    c.state = SensorState::Offline;
    c.window.clear();
    Serial.printf("%-3s: init failed, parked in reset\n", cfg.name);
    return false;
  }

  c.dev.setAddress(cfg.address);
  c.dev.setSignalRateLimit(config::SIGNAL_RATE_LIMIT);
  if (!c.dev.setMeasurementTimingBudget(config::TIMING_BUDGET_US)) {
    Serial.printf("%-3s: timing budget rejected\n", cfg.name);
  }
  c.dev.startContinuous();

  c.state = SensorState::Online;
  c.consecutiveInvalid = 0;
  c.window.clear();
  Serial.printf("%-3s: ready at 0x%02X\n", cfg.name, c.dev.getAddress());
  return true;
}

void SensorArray::begin() {
  Wire.begin(config::I2C0_SDA, config::I2C0_SCL, config::I2C_HZ);
  Wire1.begin(config::I2C1_SDA, config::I2C1_SCL, config::I2C_HZ);

  // Force every sensor into reset FIRST. An ESP32-only reset (watchdog, soft
  // reboot, serial upload) does not power-cycle the sensors, so they would
  // still hold their assigned addresses from the previous run while this code
  // goes looking for 0x29. Driving all XSHUT low reverts them unconditionally,
  // making this sequence identical on cold boot and soft reset.
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) shutdown(i);
  delay(10);

  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) initSensor(i);
}

bool SensorArray::recover(uint8_t level) {
  shutdown(level);
  delay(10);
  return initSensor(level);
}

void SensorArray::poll() {
  // Round-robin: service whichever sensors already have a result waiting.
  // Because all four range concurrently, this is a fast poll rather than a 4x
  // serialisation -- each still produces a fresh sample every timing budget.
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    Channel &c = ch_[i];
    if (c.state != SensorState::Online || !dataReady(c.dev)) continue;

    const uint16_t mm = fetchRange(c.dev);
    if (mm < config::OUT_OF_RANGE_MM) {
      c.consecutiveInvalid = 0;
      c.window.push(mm);
      c.lastSampleMs = millis();
    } else if (c.consecutiveInvalid < config::DROPOUT_MISSES) {
      // Require several consecutive misses before accepting that the target is
      // gone. Dropping the window on the first miss would throw the whole
      // integration away over one bad ping.
      if (++c.consecutiveInvalid == config::DROPOUT_MISSES) c.window.clear();
    }
  }
}

Reading SensorArray::reading(uint8_t level) const {
  const Channel &c = ch_[level];
  const WindowStats w = c.window.stats();
  const bool valid = (c.state == SensorState::Online) && (w.held > 0);

  Reading r;
  r.valid = valid;
  r.stdevMm = w.stdev;
  r.samples = w.used;
  r.distanceMm =
      valid ? w.mean + config::OFFSET_MM[level] : (float)config::NO_TARGET_MM;
  return r;
}

uint8_t SensorArray::onlineCount() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++)
    if (ch_[i].state == SensorState::Online) n++;
  return n;
}

void SensorArray::printDiagnostics() const {
  scanBus(Wire, "I2C0");
  scanBus(Wire1, "I2C1");

  // Tell "absent" apart from "already addressed". If a sensor's XSHUT is not
  // actually wired to its GPIO, we cannot force it back to 0x29, so after a
  // soft reset it still answers on its assigned address and the walk finds
  // nothing at 0x29 -- indistinguishable from a dead bus unless we say so.
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    const config::SensorConfig &cfg = config::SENSORS[i];
    cfg.bus->beginTransmission(cfg.address);
    if (cfg.bus->endTransmission() == 0) {
      Serial.printf("  0x%02X alive: '%s' kept its address from a previous run. "
                    "Its XSHUT (GPIO%u) is not wired, so a soft reset cannot "
                    "clear it -- power-cycle the sensor.\n",
                    cfg.address, cfg.name, cfg.xshutPin);
    }
  }
}
