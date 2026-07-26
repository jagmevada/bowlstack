#include "tasks.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "debug_plot.h"
#include "indicators.h"
#include "net.h"
#include "telemetry.h"
#include "version.h"

namespace tasks {
namespace {

// Core 1 carries measurement; the WiFi driver, LwIP and the TLS stacks live on
// core 0, so a portal scan, a TLS handshake or a WiFiManager save cannot preempt
// a ranging poll -- they are not even on the same processor.
//
// One caveat, since the isolation is not absolute: the Arduino WiFi EVENT task
// is pinned to core 1 by this core's defaults, at priority 19 -- far above
// sensorTask. It does preempt ranging. That is tolerable because it only
// dispatches connection events and returns in microseconds, unlike the driver
// and TLS work it hands off; but it does mean "nothing on core 0 can touch us"
// is the accurate claim, not "nothing WiFi-related runs on core 1".
const BaseType_t CORE_MEASURE = 1;
const BaseType_t CORE_NETWORK = 0;

// Priorities. The Arduino loopTask runs at 1, so the sensor task sits above
// everything that could delay it, and the debug task below.
const UBaseType_t PRIO_SENSOR = 3;
const UBaseType_t PRIO_NET = 2;
const UBaseType_t PRIO_TELEMETRY = 2;
const UBaseType_t PRIO_DEBUG = 1;
const UBaseType_t PRIO_INDICATOR = 1;

// Sized from measured high-water marks, not guesses -- see printStackHeadroom().
// Observed free after a TLS post and a full plot/heartbeat cycle:
//   sensor 2044/4096   net 6460/8192   telemetry 4632/10240   debug 876/3072
//
// debug was raised because 876 bytes is too thin a margin: its heartbeat uses
// %f, and float formatting drags in a deep and stack-hungry path, on top of the
// 320-byte line buffer it builds. telemetry was raised because TLS alone
// consumed ~5 KB the first time it ran, and a full handshake against a longer
// certificate chain can spike further.
//
// net's figure is the optimistic one: that boot joined directly, so the captive
// portal -- a web server plus a DNS server -- never ran. Treat 8 KB as
// unvalidated until a portal session has been measured.
const uint32_t STACK_SENSOR = 4096;
const uint32_t STACK_NET = 8192;
const uint32_t STACK_TELEMETRY = 12288;
const uint32_t STACK_DEBUG = 4096;
const uint32_t STACK_INDICATOR = 2560;

// Sensor cadence. Sensors conclude a measurement every TIMING_BUDGET_US, so
// polling far faster than that is wasted -- but the delay also YIELDS, which a
// priority-3 task on a shared core must do or it starves the debug task.
const uint32_t SENSOR_TICK_MS = 2;

// How long to let sensors settle before the first report. Reporting on the very
// first pass publishes an all-UNKNOWN snapshot that is superseded within a
// second -- noise in the history, and misleading if read.
const uint32_t BOOT_SETTLE_MS = 5000;

// Owned exclusively by sensorTask. Nothing else may touch these.
SensorArray sensors;
BowlLogic logic;

SemaphoreHandle_t stateMutex = nullptr;
DeviceStatus latestStatus;
PlotFrame latestFrame;
bool haveState = false;

// sensorTask -> telemetryTask. A queue rather than calling telemetry directly,
// so telemetry's ring buffer and HTTP client stay single-threaded and need no
// locking of their own.
QueueHandle_t changeQueue = nullptr;

struct QueuedChange {
  DeviceStatus status;
  telemetry::Reason reason;
  uint32_t seq;
};

// Allocated when a change is OBSERVED, not when it reaches telemetry's buffer.
// A change dropped between those two points must still consume a seq, or the
// server sees a contiguous sequence and has no evidence anything was lost --
// which is the sole purpose of seq.
uint32_t nextSeq_ = 0;

TaskHandle_t hSensor = nullptr;
TaskHandle_t hNet = nullptr;
TaskHandle_t hTelemetry = nullptr;
TaskHandle_t hDebug = nullptr;
TaskHandle_t hIndicator = nullptr;

void publish(const DeviceStatus &s, const PlotFrame &f) {
  if (xSemaphoreTake(stateMutex, portMAX_DELAY) != pdTRUE) return;
  latestStatus = s;
  latestFrame = f;
  haveState = true;
  xSemaphoreGive(stateMutex);
}

void postChange(const DeviceStatus &s, telemetry::Reason reason) {
  QueuedChange c;
  c.status = s;
  c.reason = reason;
  c.seq = nextSeq_++;

  // Non-blocking: the measurement task must never wait on the network, even for
  // queue space.
  if (xQueueSend(changeQueue, &c, 0) == pdTRUE) return;

  // Full. Drop the OLDEST and keep the newest -- xQueueSend on its own discards
  // what you are sending, which is exactly backwards here: the newest change is
  // the current state of the stack, and the stale one is the expendable entry.
  //
  // The discarded seq is logged and, more importantly, is simply never sent, so
  // the server sees a gap and knows history is incomplete. Silently discarding
  // the new one would have advanced lastReported past a state that was never
  // transmitted, making a transient 3 -> 4 -> 3 vanish without trace.
  QueuedChange dropped;
  if (xQueueReceive(changeQueue, &dropped, 0) == pdTRUE) {
    Serial.printf("tasks: change queue full - dropped seq %u (telemetry is "
                  "not keeping up)\n",
                  dropped.seq);
  }
  xQueueSend(changeQueue, &c, 0);
}

// --- sensor task -----------------------------------------------------------

void sensorTask(void *) {
  device_status::begin();
  sensors.begin();

  if (sensors.initialisedCount() == 0) {
    Serial.println("no sensors responded - check I2C0 SDA=17/SCL=16, "
                   "I2C1 SDA=21/SCL=22, XSHUT wiring, 3V3");
    sensors.printDiagnostics();
  }
  debug_plot::begin(sensors.initialisedCount());

  DeviceStatus lastReported;
  bool everReported = false;
  const uint32_t settleDeadline = millis() + BOOT_SETTLE_MS;

  for (;;) {
    sensors.poll();
    logic.update(sensors);

    const DeviceStatus now = device_status::sample(sensors, logic);

    PlotFrame frame;
    for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
      const Reading r = sensors.reading(i);
      frame.distanceMm[i] = r.distanceMm;
      frame.stdevMm[i] = r.stdevMm;
      frame.samples[i] = r.samples;
      frame.valid[i] = r.valid;
      frame.level[i] = logic.level(i);
      frame.state[i] = sensors.state(i);
    }
    frame.stackCount = logic.count();
    frame.stackStatus = logic.status();

    publish(now, frame);

    if (!everReported) {
      // Report once every sensor has concluded, or once the grace period
      // expires -- whichever first, so a dead sensor cannot hold the first
      // report back indefinitely.
      const bool settled = (sensors.onlineCount() == config::SENSOR_COUNT);
      if (settled || (int32_t)(millis() - settleDeadline) >= 0) {
        postChange(now, telemetry::Reason::Boot);
        Serial.printf("--- report (boot%s) ---\n", settled ? "" : ", not settled");
        device_status::print(now);
        lastReported = now;
        everReported = true;
      }
    } else if (device_status::differs(now, lastReported)) {
      postChange(now, telemetry::Reason::Change);
      Serial.println("--- report (change) ---");
      device_status::print(now);
      lastReported = now;
    }

    vTaskDelay(pdMS_TO_TICKS(SENSOR_TICK_MS));
  }
}

// --- network task ----------------------------------------------------------

void netTask(void *) {
  // begin() scans and makes blocking joins, and may open the captive portal.
  // All of it happens here, on core 0, while sensors keep ranging on core 1.
  net::begin();

  for (;;) {
    net::loop();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// --- telemetry task --------------------------------------------------------

void telemetryTask(void *) {
  telemetry::begin();

  for (;;) {
    // Block on the queue rather than spinning: this task should consume no CPU
    // while nothing changes. The timeout is what paces the periodic heartbeat.
    QueuedChange c;
    while (xQueueReceive(changeQueue, &c, 0) == pdTRUE) {
      telemetry::enqueue(c.status, c.reason, c.seq);
      telemetry::requestImmediateUpsert();
    }

    if (ready()) telemetry::loop(snapshot());

    // A short wait keeps an incoming change responsive while leaving the core
    // free; telemetry::loop() does its own rate limiting.
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// --- debug task ------------------------------------------------------------

void debugTask(void *) {
  for (;;) {
    if (ready()) debug_plot::update(plotFrame());
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// --- indicator task --------------------------------------------------------
// Separate from debugTask because the LEDs are PRODUCTION behaviour: debug_plot
// compiles to nothing in the shipping image, and the front-panel status must
// not vanish with it. Blinking also needs steady timing, which a task provides
// and a best-effort call from a busier loop would not.

void indicatorTask(void *) {
  indicators::begin();
  indicators::selfTest();

  for (;;) {
    if (ready()) {
      const tasks::PlotFrame f = plotFrame();

      // "Misbehaving" is anything that makes the count untrustworthy: a sensor
      // not producing readings, or a stack shape the physics says is
      // impossible. Both are conditions a person at the station can act on.
      bool sensorFault = (f.stackStatus != StackStatus::Ok);
      for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
        if (f.state[i] != SensorState::Online) sensorFault = true;
      }

      indicators::update(f, sensorFault, !net::connected());
    }
    // 50 ms is well inside the 250 ms half-period of the fastest blink, so the
    // pattern stays even without a tighter loop.
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

}  // namespace

void start() {
  stateMutex = xSemaphoreCreateMutex();
  changeQueue = xQueueCreate(16, sizeof(QueuedChange));

  if (stateMutex == nullptr || changeQueue == nullptr) {
    Serial.println("tasks: FATAL - could not allocate mutex/queue");
    return;
  }

  xTaskCreatePinnedToCore(sensorTask, "sensor", STACK_SENSOR, nullptr,
                          PRIO_SENSOR, &hSensor, CORE_MEASURE);
  xTaskCreatePinnedToCore(netTask, "net", STACK_NET, nullptr, PRIO_NET, &hNet,
                          CORE_NETWORK);
  xTaskCreatePinnedToCore(telemetryTask, "telemetry", STACK_TELEMETRY, nullptr,
                          PRIO_TELEMETRY, &hTelemetry, CORE_NETWORK);
  xTaskCreatePinnedToCore(debugTask, "debug", STACK_DEBUG, nullptr, PRIO_DEBUG,
                          &hDebug, CORE_MEASURE);
  xTaskCreatePinnedToCore(indicatorTask, "leds", STACK_INDICATOR, nullptr,
                          PRIO_INDICATOR, &hIndicator, CORE_MEASURE);

  Serial.println("tasks: sensor+debug+leds(core1) net+telemetry(core0)");
}

DeviceStatus snapshot() {
  DeviceStatus s{};
  if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
    s = latestStatus;
    xSemaphoreGive(stateMutex);
  }
  return s;
}

PlotFrame plotFrame() {
  PlotFrame f{};
  if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
    f = latestFrame;
    xSemaphoreGive(stateMutex);
  }
  return f;
}

bool ready() {
  bool r = false;
  if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
    r = haveState;
    xSemaphoreGive(stateMutex);
  }
  return r;
}

void printStackHeadroom() {
  // Minimum free stack ever observed. NOT multiplied by 4: ESP-IDF defines
  // StackType_t as uint8_t, so this is already in bytes -- scaling it the way
  // vanilla FreeRTOS requires reported 12 KB free on a 4 KB stack, which would
  // have hidden a task heading for overflow behind a comfortable-looking
  // number. Overflow on ESP32 surfaces as a corrupt-looking crash far from the
  // cause, so this figure has to be right to be worth printing.
  Serial.printf("tasks: stack free  sensor=%u/%u net=%u/%u telemetry=%u/%u "
                "debug=%u/%u leds=%u/%u bytes\n",
                hSensor ? uxTaskGetStackHighWaterMark(hSensor) : 0, STACK_SENSOR,
                hNet ? uxTaskGetStackHighWaterMark(hNet) : 0, STACK_NET,
                hTelemetry ? uxTaskGetStackHighWaterMark(hTelemetry) : 0, STACK_TELEMETRY,
                hDebug ? uxTaskGetStackHighWaterMark(hDebug) : 0, STACK_DEBUG,
                hIndicator ? uxTaskGetStackHighWaterMark(hIndicator) : 0, STACK_INDICATOR);
}

}  // namespace tasks
