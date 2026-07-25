// ---------------------------------------------------------------------------
// Bowlstack -- Phase 1 sensing engine.
//
// Four VL53L0X ToF sensors down a 6 ft pipe, one per stacked-bowl level at a
// 1.2 ft pitch, each aimed horizontally at the mid-height of its bowl. Split
// across two hardware I2C buses so a slave that locks one bus cannot take down
// all four sensors.
//
// This phase only acquires and streams distances for validation on the serial
// plotter. Presence thresholding and the bowl count come next -- see README.md.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <Wire.h>
#include <VL53L0X.h>

// --- bus wiring ------------------------------------------------------------
// Verified against the board: on I2C0, SDA is on 17 and SCL on 16, NOT the
// other way round.
static const uint8_t I2C0_SDA = 17;
static const uint8_t I2C0_SCL = 16;
static const uint8_t I2C1_SDA = 21;
static const uint8_t I2C1_SCL = 22;
static const uint32_t I2C_HZ = 400000;

static const uint8_t SENSOR_COUNT = 4;

struct SensorConfig {
  const char *name;
  TwoWire *bus;
  uint8_t xshutPin;
  uint8_t address;  // assigned during the XSHUT walk; 0x29 is the power-up default
};

// Ordered bottom of the stack upward: f1 watches the lowest bowl, f4 the
// highest. XSHUT must be an output-capable GPIO -- 34-39 are input-only on the
// ESP32 and cannot pull the line low. f3 sits on 23 to keep its harness short.
static const SensorConfig CONFIG[SENSOR_COUNT] = {
    {"f1", &Wire, 32, 0x30},
    {"f2", &Wire, 33, 0x31},
    {"f3", &Wire1, 23, 0x32},
    {"f4", &Wire1, 26, 0x33},
};

// --- measurement tuning ----------------------------------------------------

// Two tunings, because Phase 1 and Phase 2 want opposite things. Watching a
// live trace needs low latency; locking down presence thresholds later needs
// the quietest possible reading. Flip this to 0 for the accuracy preset.
//
//                          RESPONSIVE      ACCURACY
//   timing budget             50 ms          200 ms
//   effective integration    200 ms          800 ms   (4 samples after trim)
//   group delay (lag)       ~137 ms         ~550 ms
//   stdev @ 1 m              ~2.5 mm         ~1.3 mm
//   output rate               10 Hz            5 Hz
//
// Noise scales as 1/sqrt(integration), so the responsive preset costs a factor
// of 2x -- irrelevant for deciding bowl present/absent, where the margin is
// hundreds of mm.
#define TUNING_RESPONSIVE 1

// Ranging noise is shot-noise limited, so what sets accuracy is total
// integration time (budget x samples) -- but spending it inside the sensor
// beats averaging short samples, because each extra measurement re-pays a
// fixed VCSEL/pre-range overhead. 200000 is ST's "high accuracy" preset.
#if TUNING_RESPONSIVE
static const uint32_t TIMING_BUDGET_US = 50000;
#else
static const uint32_t TIMING_BUDGET_US = 200000;
#endif

// Minimum return signal to accept, MCPS. 0.25 is the driver default. Raising
// it rejects low-confidence returns, which helps against a cooperative target
// -- but the bowl wall is ~50% specular and reflects most light away at 23.5
// degrees, leaving only the ~25% diffuse component. Too high a threshold here
// reports a present bowl as "no target", which is the worse failure.
static const float SIGNAL_RATE_LIMIT = 0.25f;

// Samples in the moving average. Noise falls as 1/sqrt(N), but so does
// responsiveness: the average trails a change by ~(N-1)/2 samples.
//
// This must be read together with TRIM. Trimming discards 2*TRIM samples, so
// only (AVG_WINDOW - 2*TRIM) actually contribute -- a window of 4 with TRIM 1
// averages just 2 samples, halving the integration AND making the reported
// stdev a 2-point estimate, which is far too noisy to judge anything by. 6
// keeps 4 contributing samples, so the responsive preset really does deliver
// 4 x 50 ms = 200 ms, ST's high-accuracy figure.
static const uint8_t AVG_WINDOW = 6;

// Consecutive out-of-range reads before the window is discarded. This used to
// be tied to AVG_WINDOW, which meant a removed target took a full window
// (~1.25 s) to register as gone -- the single largest contributor to the
// perceived lag, and much worse than the averaging delay itself. Keep it small
// enough to feel instant, but above 1 so one bad ping cannot drop the window.
static const uint8_t DROPOUT_MISSES = 3;

// Drop this many highest AND lowest samples before averaging. Aged steel gives
// localized glints and dead spots; a plain mean would smear those into the
// result, while trimming discards them outright.
static const uint8_t TRIM = 1;

// Output pacing, all sensors reported on one line. At ~88 bytes per line this
// is nowhere near the UART limit -- 10 Hz is ~4% of a 921600 baud link -- so
// the rate is set by what looks smooth, not by bandwidth.
#if TUNING_RESPONSIVE
static const uint32_t OUTPUT_PERIOD_MS = 100;
#else
static const uint32_t OUTPUT_PERIOD_MS = 200;
#endif

// Per-part systematic offset, applied after averaging, in config order.
// Averaging kills random noise but cannot touch bias, and offset error is
// individual to each part. To calibrate one: leave it 0, put a target at a
// known distance, read the reported value, enter (actual - reported).
static const float OFFSET_MM[SENSOR_COUNT] = {0.0f, 0.0f, 0.0f, 0.0f};

// With no target in view the sensor reports ~8190 mm. These must never reach
// the average -- a single one would swamp a whole window of real readings.
static const uint16_t OUT_OF_RANGE_MM = 8000;

// What to plot when nothing is in view. It has to sit at the FAR end of the
// scale: "no object" means nothing within range, so reporting 0 would claim a
// target pressed against the sensor -- the exact opposite of the truth.
static const uint16_t NO_TARGET_MM = 2000;

// --- per-sensor state ------------------------------------------------------

struct Channel {
  VL53L0X dev;
  bool present;  // false if init failed; that sensor is parked in reset
  uint16_t ring[AVG_WINDOW];
  uint8_t count;
  uint8_t next;
  uint8_t consecutiveInvalid;
};

static Channel ch[SENSOR_COUNT];

struct Stats {
  float mean;
  float stdev;   // spread of the kept samples: the direct read on noise
  uint8_t n;     // samples held in the window
  uint8_t used;  // samples actually averaged, after trimming
};

// --- XSHUT -----------------------------------------------------------------
// XSHUT is pulled up to 3.3 V on every breakout, so enabling means releasing
// the line to high-Z and letting the pull-up do it. Never drive it high: the
// bare sensor's XSHUT is a 2.8 V input.

static void shutdownSensor(uint8_t i) {
  pinMode(CONFIG[i].xshutPin, OUTPUT);
  digitalWrite(CONFIG[i].xshutPin, LOW);
}

static void enableSensor(uint8_t i) {
  pinMode(CONFIG[i].xshutPin, INPUT);  // release; the breakout pull-up drives it
}

// --- non-blocking range access ---------------------------------------------
// The library's readRangeContinuousMillimeters() spins until its own sensor is
// ready, which would stall the whole round-robin on whichever sensor is
// slowest. Splitting the poll from the fetch means we only ever touch a sensor
// that already has a result waiting.

static bool dataReady(VL53L0X &dev) {
  return (dev.readReg(VL53L0X::RESULT_INTERRUPT_STATUS) & 0x07) != 0;
}

static uint16_t fetchRange(VL53L0X &dev) {
  uint16_t mm = dev.readReg16Bit(VL53L0X::RESULT_RANGE_STATUS + 10);
  dev.writeReg(VL53L0X::SYSTEM_INTERRUPT_CLEAR, 0x01);
  return mm;
}

// --- windowing -------------------------------------------------------------

static void pushSample(Channel &c, uint16_t mm) {
  c.ring[c.next] = mm;
  c.next = (c.next + 1) % AVG_WINDOW;
  if (c.count < AVG_WINDOW) c.count++;
}

static void clearWindow(Channel &c) {
  c.count = 0;
  c.next = 0;
}

static Stats windowStats(const Channel &c) {
  Stats s = {0.0f, 0.0f, c.count, 0};
  if (c.count == 0) return s;

  uint16_t sorted[AVG_WINDOW];
  memcpy(sorted, c.ring, c.count * sizeof(uint16_t));

  // Insertion sort. AVG_WINDOW is single digits, so this beats anything
  // cleverer and costs microseconds against a 200 ms measurement.
  for (uint8_t i = 1; i < c.count; i++) {
    uint16_t v = sorted[i];
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && sorted[j] > v) {
      sorted[j + 1] = sorted[j];
      j--;
    }
    sorted[j + 1] = v;
  }

  uint8_t trim = (c.count >= (uint8_t)(2 * TRIM + 2)) ? TRIM : 0;
  uint8_t lo = trim;
  uint8_t hi = c.count - trim;
  uint8_t n = hi - lo;

  uint32_t sum = 0;
  for (uint8_t i = lo; i < hi; i++) sum += sorted[i];
  s.mean = (float)sum / n;

  float var = 0.0f;
  for (uint8_t i = lo; i < hi; i++) {
    float d = (float)sorted[i] - s.mean;
    var += d * d;
  }
  s.stdev = (n > 1) ? sqrtf(var / (n - 1)) : 0.0f;
  s.used = n;
  return s;
}

// --- bring-up --------------------------------------------------------------

static void scanBus(TwoWire &bus, const char *label) {
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

static void bringUpSensors() {
  // Force every sensor into reset FIRST. An ESP32-only reset (watchdog, soft
  // reboot, serial upload) does not power-cycle the sensors, so they would
  // still hold their assigned addresses from the previous run while this code
  // goes looking for 0x29. Driving all XSHUT low reverts them unconditionally,
  // making this sequence identical on cold boot and soft reset.
  for (uint8_t i = 0; i < SENSOR_COUNT; i++) shutdownSensor(i);
  delay(10);

  for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
    const SensorConfig &cfg = CONFIG[i];
    Channel &c = ch[i];

    enableSensor(i);
    delay(10);  // datasheet t_boot is 1.2 ms; 10 ms is comfortable

    c.dev.setBus(cfg.bus);
    c.dev.setTimeout(500);

    if (!c.dev.init()) {
      // Park the failed sensor back in reset before releasing the next one.
      // Leaving it awake would strand it at 0x29, and the next sensor on the
      // same bus also answers at 0x29 -- two devices, one address.
      shutdownSensor(i);
      c.present = false;
      Serial.printf("%-5s: init failed, parked in reset\n", cfg.name);
      continue;
    }

    c.dev.setAddress(cfg.address);
    c.dev.setSignalRateLimit(SIGNAL_RATE_LIMIT);
    if (!c.dev.setMeasurementTimingBudget(TIMING_BUDGET_US)) {
      Serial.printf("%-5s: timing budget rejected\n", cfg.name);
    }
    c.dev.startContinuous();
    c.present = true;
    Serial.printf("%-5s: ready at 0x%02X\n", cfg.name, c.dev.getAddress());
  }
}

// Tell "absent" apart from "already addressed". If a sensor's XSHUT is not
// actually wired to its GPIO, we cannot force it back to 0x29, so after a soft
// reset it still answers on its assigned address and the walk finds nothing at
// 0x29. That is indistinguishable from a dead bus unless we say so.
static void diagnoseSilentBus() {
  scanBus(Wire, "I2C0");
  scanBus(Wire1, "I2C1");
  for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
    const SensorConfig &cfg = CONFIG[i];
    cfg.bus->beginTransmission(cfg.address);
    if (cfg.bus->endTransmission() == 0) {
      Serial.printf("  0x%02X alive: '%s' kept its address from a previous run. "
                    "Its XSHUT (GPIO%u) is not wired, so a soft reset cannot "
                    "clear it -- power-cycle the sensor.\n",
                    cfg.address, cfg.name, cfg.xshutPin);
    }
  }
}

// The plotter extension offers 9600/19200/38400/57600/115200/460800/921600 --
// select this value in its dropdown to match. 115200 is deliberate: at ~88
// bytes per line and 10 Hz the payload is ~8% of the link, and measurement
// showed UART contributed 7.6 ms against a ~2 s latency budget. Raising it
// changes nothing that matters; latency lives in AVG_WINDOW and
// DROPOUT_MISSES.
static const uint32_t SERIAL_BAUD = 115200;

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);  // let the USB-serial link settle before the first banner

  Wire.begin(I2C0_SDA, I2C0_SCL, I2C_HZ);
  Wire1.begin(I2C1_SDA, I2C1_SCL, I2C_HZ);

  bringUpSensors();

  uint8_t live = 0;
  for (uint8_t i = 0; i < SENSOR_COUNT; i++)
    if (ch[i].present) live++;

  // Report integration from the samples that actually survive trimming, not
  // the raw window -- the two discarded samples contribute nothing, and
  // quoting the window length overstates it by AVG_WINDOW/(AVG_WINDOW-2*TRIM).
  const uint32_t effectiveSamples = AVG_WINDOW - 2 * TRIM;
  Serial.printf("Bowlstack phase 1: %u/%u sensors live, %lu ms integration "
                "(%lu x %lu ms), %.1f Hz\n",
                live, SENSOR_COUNT,
                (TIMING_BUDGET_US * effectiveSamples) / 1000, effectiveSamples,
                TIMING_BUDGET_US / 1000, 1000.0f / OUTPUT_PERIOD_MS);

  if (live == 0) {
    Serial.println("no sensors responded - check I2C0 SDA=17/SCL=16, "
                   "I2C1 SDA=21/SCL=22, XSHUT wiring, 3V3");
    diagnoseSilentBus();
  }
}

void loop() {
  // Round-robin: service whichever sensors already have a result waiting.
  // Because all four range concurrently, this is a fast poll rather than a 4x
  // serialisation -- each sensor still produces a fresh sample every ~200 ms.
  for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
    Channel &c = ch[i];
    if (!c.present || !dataReady(c.dev)) continue;

    uint16_t mm = fetchRange(c.dev);
    if (mm < OUT_OF_RANGE_MM) {
      c.consecutiveInvalid = 0;
      pushSample(c, mm);
    } else if (c.consecutiveInvalid < DROPOUT_MISSES) {
      // Require a few consecutive misses before accepting that the target is
      // gone. Dropping the window on the first miss would throw the whole
      // integration away over one bad ping.
      if (++c.consecutiveInvalid == DROPOUT_MISSES) clearWindow(c);
    }
  }

  // Emit on a fixed grid rather than "at least OUTPUT_PERIOD_MS since the last
  // output". The latter quantises to whole sample periods and would silently
  // settle at half the intended rate.
  static uint32_t nextOutput = 0;
  uint32_t now = millis();
  if ((int32_t)(now - nextOutput) < 0) return;
  nextOutput += OUTPUT_PERIOD_MS;
  if ((int32_t)(now - nextOutput) > 0) {
    nextOutput = now + OUTPUT_PERIOD_MS;  // resync after a stall
  }

  Stats s[SENSOR_COUNT];
  bool valid[SENSOR_COUNT];
  for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
    s[i] = windowStats(ch[i]);
    valid[i] = ch[i].present && s[i].n > 0;
  }

  // One plotter line carrying every sensor. <name> is distance, <name>_ok is
  // validity -- the _ok channel is the one to trust, since the distance is
  // pegged to NO_TARGET_MM when it reads 0, which is a floor and not a
  // measurement.
  Serial.print(">");
  for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
    Serial.printf("%s:%.2f,", CONFIG[i].name,
                  valid[i] ? s[i].mean + OFFSET_MM[i] : (float)NO_TARGET_MM);
  }
  for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
    Serial.printf("%s_ok:%u%s", CONFIG[i].name, valid[i] ? 1 : 0,
                  (i == SENSOR_COUNT - 1) ? "\r\n" : ",");
  }

  static uint32_t lastStatus = 0;
  if (now - lastStatus >= 1000) {
    lastStatus = now;
    for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
      if (!ch[i].present) continue;
      if (valid[i]) {
        Serial.printf("%-5s %8.2f mm  stdev=%5.2f  n=%u/%u (%u used)\n",
                      CONFIG[i].name, s[i].mean + OFFSET_MM[i], s[i].stdev,
                      s[i].n, AVG_WINDOW, s[i].used);
      } else {
        Serial.printf("%-5s   no target\n", CONFIG[i].name);
      }
    }
  }
}
