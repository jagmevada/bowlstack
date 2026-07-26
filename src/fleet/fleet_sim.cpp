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

  char id[12];       // "BWL-002" -- ch.deviceId and st.deviceId point here
  Scenario scenario;
  uint32_t seq;      // allocated when a change is OBSERVED, as on a real device
  uint32_t nextTickMs;
  uint32_t ticks;

  uint8_t stack;
  bool sensorOk[config::SENSOR_COUNT];
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

  // Levels are bottom-up, and a real stack is contiguous from f1 because bowls
  // rest on each other and cannot float. Discontiguous deliberately violates
  // that, which is the point of the scenario.
  if (n.scenario == Scenario::Discontiguous) {
    for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
      s.levels[i] = LevelState::Absent;
    }
    s.levels[1] = LevelState::Present;  // a bowl above a gap
    s.stackCount = 0;                   // a fault reports no count, not a guess
    s.stackStatus = StackStatus::Discontiguous;
    return;
  }

  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    if (!n.sensorOk[i]) {
      s.levels[i] = LevelState::Unknown;  // no data is not "no bowl"
    } else {
      s.levels[i] = (i < n.stack) ? LevelState::Present : LevelState::Absent;
    }
  }

  // The count is DERIVED from the levels, never copied from the simulated stack.
  //
  // Copying it emits states no real device can produce -- observed on the bench
  // as "cnt=4 degraded 0/4", i.e. four bowls counted with every sensor dead. A
  // front-end built against that learns to trust a number the hardware cannot
  // justify, which is the one habit this whole design exists to prevent. The
  // simulator has to be bound by the same rule as the firmware: report what the
  // sensors support, not what the fiction says is there.
  //
  // Mirrors BowlLogic. A bowl cannot float, so the count is the height of the
  // highest observed bowl, and an Unknown BELOW one is inferred present -- a dead
  // sensor under a bowl does not make the bowl disappear. An Unknown at or above
  // the top cannot be resolved either way, so it is not counted, which makes the
  // reported count a LOWER BOUND. That is precisely what `degraded` tells the UI.
  // TRANSCRIBED FROM BowlLogic::recompute(), not re-derived. Keep the two in
  // step: any divergence is a state the front-end will be built against and the
  // hardware cannot produce.
  int8_t topPresent = -1;
  for (int8_t i = 0; i < (int8_t)config::SENSOR_COUNT; i++) {
    if (s.levels[i] == LevelState::Present) topPresent = i;
  }

  // An Absent BELOW the highest bowl is a floating bowl -- physically impossible,
  // so a fault rather than a count.
  for (int8_t i = 0; i < topPresent; i++) {
    if (s.levels[i] == LevelState::Absent) {
      s.stackCount = 0;
      s.stackStatus = StackStatus::Discontiguous;
      return;
    }
  }

  bool ambiguous = false;
  for (int8_t i = topPresent + 1; i < (int8_t)config::SENSOR_COUNT; i++) {
    // BREAK at the first observed Absent. Nothing can rest above an empty level,
    // so contiguity has already settled everything beyond it and an Unknown up
    // there proves nothing.
    //
    // Omitting this break is a bug bowl_logic.cpp had already found and fixed --
    // it flagged 12 of the 81 level combinations as ambiguous when the count was
    // exact -- and the simulator reintroduced it. Reachable and permanent: the
    // Degraded node kills f4 and drains, so at an empty stack it published
    // levels [absent,absent,absent,unknown] as `degraded` where the firmware
    // publishes `ok`. An adversarial review found 23 of 80 states disagreeing.
    if (s.levels[i] == LevelState::Absent) break;
    if (s.levels[i] == LevelState::Unknown) ambiguous = true;
  }

  s.stackCount = (uint8_t)(topPresent + 1);

  // AMBIGUITY decides this, never sensor health. A dead sensor BELOW the top bowl
  // costs nothing, because contiguity proves what it cannot see -- so `ok` with
  // sensors_online < 4 and an `unknown` level is ordinary production output. The
  // simulator previously degraded on `online < SENSOR_COUNT` and so could never
  // emit it, teaching a front-end the false invariant "ok implies 4/4".
  s.stackStatus = ambiguous ? StackStatus::Degraded : StackStatus::Ok;
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

    n.stack = config::SENSOR_COUNT;
    for (uint8_t k = 0; k < config::SENSOR_COUNT; k++) n.sensorOk[k] = true;

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
