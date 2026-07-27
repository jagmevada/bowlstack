#include "fleet_sim.h"

#include <WiFi.h>

#include "battery_soc.h"
#include "net.h"
#include "version.h"

namespace fleet_sim {
namespace {

struct Node {
  telemetry::Channel ch;
  DeviceStatus st;

  // The REAL presence hysteresis, contiguity rule, count and status. One per
  // node, because the hysteresis carries state. The simulator synthesises the
  // sensor layer beneath this and nothing above it.
  BowlLogic logic;

  char id[12];       // "BWL-002" -- ch.deviceId and st.deviceId point here
  Scenario scenario;
  uint32_t seq;      // allocated when a change is OBSERVED, as on a real device
  uint32_t nextTickMs;
  uint32_t ticks;

  uint8_t stack;
  bool sensorOk[config::SENSOR_COUNT];
  bool stuck[config::SENSOR_COUNT];  // returns a frozen distance, see below
  float stuckMm[config::SENSOR_COUNT];
  uint16_t cellMv;
  battery::Level band;
  bool charging;
  bool quiet;        // GoesQuiet: stops reporting to trigger `offline`
};

Node nodes_[NODE_COUNT];
uint8_t cursor_ = 0;  // round-robin index for uplink servicing
uint32_t reportAtMs_ = 0;

// Alternates the event flush with the status round, so neither can starve the
// other. See the note in loop().
bool flushTurn_ = true;

// Deterministic per-node PRNG. Seeded from the index rather than esp_random() so
// a given node behaves the same across reboots -- a front-end developer
// comparing two screenshots should not be fighting fresh randomness.
uint32_t rnd(Node &n) {
  static uint32_t mix = 0;
  mix = mix * 1664525u + 1013904223u + n.seq + (uint32_t)(&n - nodes_) * 2654435761u;
  return mix ^ (mix >> 13);
}

bool chance(Node &n, uint8_t percent) { return (rnd(n) % 100u) < percent; }

// --- band, via the SAME curve and hysteresis the real firmware uses ---------
// Not an approximation. The front-end consumes bands, so a simulated band is
// only useful if it moves the way a real one does -- including refusing to
// oscillate on a boundary, which is the whole reason battery::Monitor exists.
battery::Level bandFor(Node &n) {
  if (n.scenario == Scenario::BatteryAbsent ||
      n.scenario == Scenario::BatteryImplausible) {
    return battery::Level::Unknown;
  }
  const float soc = battery::socFromMillivolts(n.cellMv);
  const bool fresh = (n.band == battery::Level::Unknown);
  bool oc = fresh || n.band == battery::Level::Low ||
            n.band == battery::Level::Medium || n.band == battery::Level::Good;
  bool ol = fresh || n.band == battery::Level::Medium ||
            n.band == battery::Level::Good;
  bool om = fresh || n.band == battery::Level::Good;

  oc = oc ? soc >= config::BAT_LOW_TO_CRITICAL_DOWN
          : soc >= config::BAT_CRITICAL_TO_LOW_UP;
  ol = ol ? soc >= config::BAT_MEDIUM_TO_LOW_DOWN
          : soc >= config::BAT_LOW_TO_MEDIUM_UP;
  om = om ? soc >= config::BAT_GOOD_TO_MEDIUM_DOWN
          : soc >= config::BAT_MEDIUM_TO_GOOD_UP;

  if (om) return battery::Level::Good;
  if (ol) return battery::Level::Medium;
  if (oc) return battery::Level::Low;
  return battery::Level::Critical;
}

// --- the dummy sensor layer -------------------------------------------------
// Everything below this line is what a real unit would MEASURE. Above it,
// nothing knows the difference.
void buildStatus(Node &n) {
  DeviceStatus &s = n.st;
  s.deviceId = n.id;
  s.firmware = BOWLSTACK_FW_VERSION;
  s.uptimeSec = millis() / 1000;

  // Synthetic MAC: the real board's, with the node index in the last octet, so
  // the front-end sees 31 distinct boards rather than one repeated.
  const uint8_t idx = (uint8_t)(&n - nodes_);
  snprintf(s.mac, sizeof(s.mac), "%s", WiFi.macAddress().c_str());
  static const char *hex = "0123456789ABCDEF";
  s.mac[15] = hex[(idx >> 4) & 0xF];
  s.mac[16] = hex[idx & 0xF];

  s.batteryMv = n.cellMv;
  s.batteryPinMv = (uint16_t)(n.cellMv / config::BATTERY_DIVIDER);
  s.batteryLevel = n.band;
  s.batteryPercent = (n.band == battery::Level::Unknown)
                         ? -1
                         : (int8_t)lroundf(battery::socFromMillivolts(n.cellMv));
  s.charging = n.charging;

  uint8_t online = 0;
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    s.sensorOnline[i] = n.sensorOk[i];
    if (n.sensorOk[i]) online++;
  }
  s.sensorsOnline = online;

  // ---------------------------------------------------------------------
  // Synthesise the SENSOR LAYER, then let production code do everything above
  // it. That is the whole point: the count, the status, the presence hysteresis
  // and the contiguity rule all come from BowlLogic, so the simulator CANNOT
  // disagree with the firmware, because there is only one implementation.
  //
  // Reimplementing them drifted, exactly as you would expect. The hand-rolled
  // copy differed from recompute() on 23 of the 80 reachable states, and had
  // reintroduced a bug bowl_logic.cpp had already found and fixed. Deleting it
  // removes that whole class of defect rather than patching this instance.
  //
  // What is faked now stops at a distance in millimetres and a SensorState --
  // which is precisely what the VL53L0X layer produces.
  // ---------------------------------------------------------------------
  SensorState states[config::SENSOR_COUNT];
  Reading readings[config::SENSOR_COUNT];

  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    states[i] = n.sensorOk[i] ? SensorState::Online : SensorState::Offline;

    Reading &r = readings[i];
    r.samples = (uint8_t)(config::AVG_WINDOW - 2 * config::TRIM);

    // A bowl sits close to its sensor; an empty level returns no target at all.
    // Discontiguous puts a bowl above a gap -- physically impossible, so only
    // reachable through a sensor or mounting fault, which is the scenario.
    bool bowl = (i < n.stack);
    if (n.scenario == Scenario::Discontiguous) bowl = (i == 1);

    if (n.stuck[i]) {
      // A frozen reading. Real ToF output jitters by a millimetre or two even
      // against a motionless target, so a perfectly constant value is itself a
      // fault signature. It is in the bed because the CURRENT firmware does NOT
      // detect it: SensorArray demotes on I/O failures, the 0xFFFF signature and
      // staleness, and a plausible constant trips none of them. The twin
      // reproduces that blind spot rather than papering over it -- a front-end
      // must not assume a stuck sensor will announce itself.
      r.distanceMm = n.stuckMm[i];
      r.stdevMm = 0.0f;
      r.valid = true;
      continue;
    }

    if (bowl) {
      // Bowl face, with the millimetre-scale noise a real sensor shows. The
      // spread is not decoration: it is what the presence hysteresis in
      // BowlLogic exists to absorb, so a bed without it would never exercise
      // that path at all.
      r.distanceMm = 50.0f + (float)((int32_t)(rnd(n) % 7) - 3);
      r.stdevMm = 1.0f + (float)(rnd(n) % 3);
      r.valid = true;
    } else {
      // Nothing in range. BowlLogic treats this as a DIRECT observation of
      // absence rather than a missing measurement, so it drives the trigger low
      // outright instead of going through the distance thresholds.
      r.distanceMm = (float)config::NO_TARGET_MM;
      r.stdevMm = 0.0f;
      r.valid = false;
    }
  }

  n.logic.update(states, readings);   // production logic, unmodified

  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    s.levels[i] = n.logic.level(i);
  }
  s.stackCount = n.logic.count();
  s.stackStatus = n.logic.status();
}

// Exactly the fields device_status::differs() compares, so a simulated change
// means what a real one means.
uint32_t fingerprint(const DeviceStatus &s) {
  uint32_t h = s.stackCount * 31u + (uint32_t)s.stackStatus * 7u + s.sensorsOnline;
  h = h * 31u + (uint32_t)s.batteryLevel;
  h = h * 31u + (s.charging ? 1u : 0u);
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    h = h * 31u + (uint32_t)s.levels[i] * 3u + (s.sensorOnline[i] ? 1u : 0u);
  }
  return h;
}

void evolve(Node &n) {
  n.ticks++;

  switch (n.scenario) {
    case Scenario::DepletesFast:
      if (n.stack > 0 && chance(n, 70)) n.stack--;
      break;

    case Scenario::Restocked:
      if (n.stack == 0) n.stack = config::SENSOR_COUNT;
      else if (chance(n, 45)) n.stack--;
      break;

    case Scenario::Flapping:
      // Deliberately unstable: proves the 5 s floor coalesces writes while
      // status_events keeps every transition with its own recorded_at.
      n.stack = (uint8_t)(n.ticks % (config::SENSOR_COUNT + 1));
      break;

    case Scenario::Degraded:
      n.sensorOk[config::SENSOR_COUNT - 1] = false;  // the TOP sensor
      if (n.stack > 0 && chance(n, 20)) n.stack--;
      break;

    case Scenario::DeadSensorLow:
      // f1 is dead, but the stack is held at least 2 high so a bowl is always
      // observed ABOVE the dead sensor. Contiguity then proves f1 is occupied,
      // making the count exact and the status `ok` despite only 3 sensors online.
      n.sensorOk[0] = false;
      if (n.stack > 2 && chance(n, 25)) n.stack--;
      else if (n.stack < 2) n.stack = config::SENSOR_COUNT;
      break;

    case Scenario::AllSensorsDead:
      for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) n.sensorOk[i] = false;
      break;

    case Scenario::Discontiguous:
      break;  // held, so the fault is always visible somewhere in the fleet

    case Scenario::StuckSensor:
      // f3 froze reading a bowl face, and the stack drains beneath it. Nothing
      // in the firmware can tell a frozen reading from a real one, so the node
      // passes through a window where it over-reports by one with status `ok`
      // and 4/4 sensors online -- and only once the stack falls below the frozen
      // level does contiguity see a floating bowl and raise `discontiguous`.
      // See the enum comment for the full sequence.
      n.stuck[2] = true;
      n.stuckMm[2] = 52.0f;
      if (n.stack > 0 && chance(n, 30)) n.stack--;
      else if (n.stack == 0 && chance(n, 25)) n.stack = config::SENSOR_COUNT;
      break;

    case Scenario::GoesQuiet:
      // Reports normally for a while, then stops. `offline` in device_overview
      // is service-hour aware, so this is what makes the alarm testable.
      if (n.stack > 0 && chance(n, 20)) n.stack--;
      if (n.ticks > 15) n.quiet = true;
      break;

    default:
      if (n.stack > 0 && chance(n, 22)) n.stack--;
      else if (n.stack == 0 && chance(n, 30)) n.stack = config::SENSOR_COUNT;
      break;
  }

  // Battery. Rates are per tick and chosen so a band change is visible within a
  // service rather than a week.
  switch (n.scenario) {
    case Scenario::BatteryAbsent:
      n.cellMv = 40;  // open input: near zero, and NOT a flat cell
      break;
    case Scenario::BatteryImplausible:
      n.cellMv = 6365;  // the value actually measured on a floating divider
      break;
    case Scenario::BatteryCritical:
      if (n.cellMv > 3080) n.cellMv -= 12;
      break;
    case Scenario::BatteryLow:
      if (n.cellMv > 3400) n.cellMv -= 6;
      break;
    case Scenario::Charging:
      n.charging = true;
      if (n.cellMv < 4150) n.cellMv += 4;
      break;
    default:
      if (n.cellMv > 3500) n.cellMv -= 1;
      break;
  }
  n.band = bandFor(n);
}

}  // namespace

const char *scenarioName(Scenario s) {
  switch (s) {
    case Scenario::Normal:             return "normal";
    case Scenario::DepletesFast:       return "depletes-fast";
    case Scenario::Restocked:          return "restocked";
    case Scenario::Degraded:           return "degraded-top";
    case Scenario::DeadSensorLow:      return "dead-sensor-low";
    case Scenario::AllSensorsDead:     return "all-sensors-dead";
    case Scenario::Discontiguous:      return "discontiguous";
    case Scenario::BatteryLow:         return "battery-low";
    case Scenario::BatteryCritical:    return "battery-critical";
    case Scenario::BatteryAbsent:      return "battery-absent";
    case Scenario::BatteryImplausible: return "battery-implausible";
    case Scenario::Charging:           return "charging";
    case Scenario::GoesQuiet:          return "goes-quiet";
    case Scenario::Flapping:           return "flapping";
    case Scenario::StuckSensor:        return "stuck-sensor";
    default:                           return "?";
  }
}

void begin() {
  const uint8_t scenarioCount = (uint8_t)Scenario::COUNT;

  for (uint8_t i = 0; i < NODE_COUNT; i++) {
    Node &n = nodes_[i];
    snprintf(n.id, sizeof(n.id), "BWL-%03u", (unsigned)(FIRST_DEVICE + i));

    // Assign scenarios by index, so the FIRST nodes cover the whole enum. With
    // fewer nodes than scenarios some cases are missing -- reported below -- but
    // whatever is present is never left to chance.
    n.scenario = (i < scenarioCount) ? (Scenario)i
                                     : (Scenario)(i % scenarioCount);

    // Sensors default to HEALTHY. A fault appears only where a scenario asks for
    // one, so an unrelated node never quietly drifts into a degraded state and
    // muddies what the front-end is looking at.
    n.stack = config::SENSOR_COUNT;
    for (uint8_t k = 0; k < config::SENSOR_COUNT; k++) {
      n.sensorOk[k] = true;
      n.stuck[k] = false;
      n.stuckMm[k] = 0.0f;
    }

    // Spread the healthy cells so the bands are not uniform...
    n.cellMv = 4050 - (uint16_t)(i * 7);

    // ...then start the battery scenarios ALREADY IN the band they exist to
    // demonstrate. Left on the default spread they all read `good` and drift
    // down at a few mV per tick, so `low` would not appear for ~20 minutes and
    // `critical` for longer. A test bed whose states show up eventually is not
    // much use to someone building a screen right now: every band must be on
    // screen from the first report.
    //
    // Voltages are picked off the measured curve in battery_soc.h, not guessed.
    switch (n.scenario) {
      case Scenario::BatteryLow:      n.cellMv = 3520; break;  // ~29% -> low
      case Scenario::BatteryCritical: n.cellMv = 3150; break;  // ~9%  -> critical
      case Scenario::BatteryAbsent:   n.cellMv = 40;   break;  // open input
      case Scenario::BatteryImplausible: n.cellMv = 6365; break;  // floating pin
      // Starts mid-range and charges upward, so the front-end sees a band
      // RISING as well as falling -- and it is the only node that supplies
      // `medium`, which the healthy spread above never reaches.
      case Scenario::Charging:        n.cellMv = 3700; break;  // ~52% -> medium
      default: break;
    }

    n.band = battery::Level::Unknown;     // first look uses the nominal edges
    n.charging = false;
    n.quiet = false;
    n.seq = 0;
    n.ticks = 0;
    // Stagger the first tick so 31 nodes do not all change on the same instant.
    n.nextTickMs = millis() + (uint32_t)i * 700;

    n.band = bandFor(n);
    telemetry::openChannel(n.ch, n.id);
    buildStatus(n);

    // Boot event, exactly as a real unit emits on power-up.
    telemetry::enqueue(n.ch, n.st, telemetry::Reason::Boot, n.seq++);
    telemetry::requestImmediateUpsert(n.ch);
  }

  Serial.printf("\nfleet_sim: %u nodes, BWL-%03u..BWL-%03u\n", NODE_COUNT,
                (unsigned)FIRST_DEVICE, (unsigned)(FIRST_DEVICE + NODE_COUNT - 1));
  if (NODE_COUNT < scenarioCount) {
    Serial.printf("fleet_sim: WARNING only %u of %u scenarios covered -- "
                  "raise -DBOWLSTACK_FLEET_SIM to %u for full front-end "
                  "coverage\n",
                  NODE_COUNT, scenarioCount, scenarioCount);
  }
  Serial.println("fleet_sim: every id must exist in `devices` "
                 "(supabase/register_devices.sql) or the node backs off on 23503");
}

void loop() {
  const uint32_t now = millis();

  // --- evolve -------------------------------------------------------------
  for (uint8_t i = 0; i < NODE_COUNT; i++) {
    Node &n = nodes_[i];
    if ((int32_t)(now - n.nextTickMs) < 0) continue;
    n.nextTickMs = now + TICK_MS;

    const uint32_t before = fingerprint(n.st);
    evolve(n);
    buildStatus(n);

    if (n.quiet) continue;  // deliberately silent, to raise `offline`

    if (fingerprint(n.st) != before) {
      // seq is allocated at OBSERVATION, not at send, so anything dropped
      // between the two leaves a gap the server can see.
      telemetry::enqueue(n.ch, n.st, telemetry::Reason::Change, n.seq++);
      telemetry::requestImmediateUpsert(n.ch);
    }
  }

  if (!net::connected()) return;

  // --- history: every node's queued events in ONE POST --------------------
  // status_events carries device_id per row and the endpoint already takes an
  // array, so this is the same batch INSERT the real firmware makes -- just with
  // rows from more than one installation. Without it, 31 buffers would mean 31
  // requests, which is the one way a single ESP32 could not represent 31 nodes.
  //
  // The flush and the status round ALTERNATE rather than the flush taking
  // priority. Priority starved the entire fleet: if any one channel held events
  // it could not send, `np > 0` on every pass and the function returned before
  // reaching the status round, so no node ever got its state row PATCHed.
  //
  // An unregistered id reaches exactly that state and stays there. flushChannels
  // sets `unprovisioned` on a 23503 and KEEPS the queue, but the hard backoff
  // that would pace the retry lives in telemetry::loop() -- which only runs in
  // the status round. One un-provisioned device therefore silenced the other
  // thirteen indefinitely. Such channels are now excluded from the batch so
  // telemetry::loop() can reach them and apply the backoff.
  telemetry::Channel *pending[NODE_COUNT];
  uint8_t np = 0;
  for (uint8_t i = 0; i < NODE_COUNT; i++) {
    telemetry::Channel &c = nodes_[i].ch;
    if (c.count > 0 && !c.backoffActive && !c.unprovisioned) {
      pending[np++] = &c;
    }
  }
  if (np > 0 && flushTurn_) {
    flushTurn_ = false;
    telemetry::flushMany(pending, np);
    return;  // one HTTP transaction per loop pass, as on a real device
  }
  flushTurn_ = true;

  // --- current state: one node per pass, round-robin ----------------------
  // Each node keeps its own 5 s floor and 60 s heartbeat inside
  // telemetry::loop(), so this only decides whose turn it is to be considered.
  Node &n = nodes_[cursor_];
  cursor_ = (uint8_t)((cursor_ + 1) % NODE_COUNT);
  if (!n.quiet) telemetry::loop(n.ch, n.st);

  if ((int32_t)(now - reportAtMs_) >= 0) {
    reportAtMs_ = now + 30000;
    report();
  }
}

void report() {
  uint8_t quiet = 0, unprov = 0, queued = 0;
  Serial.println("\nfleet_sim ------------------------------------------------");
  for (uint8_t i = 0; i < NODE_COUNT; i++) {
    Node &n = nodes_[i];
    queued += n.ch.count;
    if (n.quiet) quiet++;
    if (n.ch.unprovisioned) unprov++;
    Serial.printf("  %s %-19s cnt=%u %-13s %u/%u  %5umV %-8s%s%s%s\n", n.id,
                  scenarioName(n.scenario), n.st.stackCount,
                  BowlLogic::wireName(n.st.stackStatus), n.st.sensorsOnline,
                  config::SENSOR_COUNT, n.st.batteryMv,
                  battery::levelName(n.st.batteryLevel),
                  n.st.charging ? " chg" : "", n.quiet ? " QUIET" : "",
                  n.ch.unprovisioned ? " UNREGISTERED" : "");
  }
  Serial.printf("  %u nodes, %u queued events, %u quiet, %u unregistered, "
                "wifi=%s\n",
                NODE_COUNT, queued, quiet, unprov,
                net::connected() ? "up" : "DOWN");
}

}  // namespace fleet_sim
