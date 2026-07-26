#include "telemetry.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "battery_soc.h"
#include "bowl_logic.h"
#include "net.h"
#include "secret.h"
#include "version.h"

namespace telemetry {
namespace {

// Events held while offline. Small on purpose: the current state is re-sent on
// every reconnect and every boot, so the buffer preserves only the TIMING of
// intermediate changes, not the truth of the present.
//
// POST_MIN_INTERVAL_MS below also parks up to 5 s of changes here even while
// ONLINE, which is new -- the queue used to drain on the next loop() pass. In
// normal service that is a handful of entries: bowl changes are seconds apart,
// and the battery band and charger state are both hysteretic now. A sensor
// flapping at tick rate could still overflow it, which drops the OLDEST and
// leaves a gap in `seq` -- visible server-side as exactly what it is, rather
// than silently pretending the history is complete. 32 is kept rather than
// raised because the batch is serialised into one JsonDocument, and doubling the
// queue doubles a ~8 KB heap allocation on a device that also holds a TLS
// session.
const uint8_t QUEUE_LEN = config::TELEMETRY_QUEUE_LEN;

// Heartbeat. Changes post immediately, so this only proves liveness. At 10 s
// across 30 devices the fleet would make ~7.8M requests/month, and TLS
// handshakes alone would dominate the egress budget.
const uint32_t STATUS_PERIOD_MS = 60000;

// Backoff after a failed post, so a dead uplink cannot spin the loop.
const uint32_t RETRY_PERIOD_MS = 15000;

// Hard floor between telemetry rounds for this device. A genuinely unstable
// input -- a misaligned sensor, something intermittently clipping a beam, a
// battery resting on a band boundary -- can flap state several times a second,
// and each flap would otherwise be its own HTTP request. Observed on the bench:
// eight OK/DISCONTIGUOUS transitions in 45 s, and separately dozens of battery
// band flips per minute.
//
// This throttles the WAKE-UP, not the data. Every change is still enqueued the
// moment it happens with its own timestamp, so history keeps full resolution;
// changes occurring inside one window are simply clubbed into the next round --
// the whole batch in one POST, and the current-state PATCH carrying the latest
// values by construction.
//
// This is the ONLY rate limiter on the uplink, deliberately. There used to be a
// second one inside requestImmediateUpsert(), which gated only the PATCH and
// left the event POST completely unthrottled -- so a flapping input still made
// one request per transition, which is exactly what the limiter was there to
// prevent.
const uint32_t POST_MIN_INTERVAL_MS = 5000;

// Backoff once the server says this device is not registered. Retrying cannot
// fix that; only a human inserting a `devices` row can.
const uint32_t UNPROVISIONED_RETRY_MS = 300000;

// Matches the server-side clamp in tg_status_events_stamp().
const uint32_t AGE_MAX_MS = 604800000;

const uint32_t HTTP_TIMEOUT_MS = 8000;

// The one channel a real unit owns. QueuedEvent and Channel now live in the
// header, because the fleet simulator holds an array of them -- see the note
// there on why there is one implementation rather than two.
Channel default_;

// Per-PROCESS, deliberately shared by every channel: one TLS session for the
// whole device, since re-handshaking per request is what dominates egress.
WiFiClientSecure *tls_ = nullptr;

// PostgREST answers 409 for BOTH a duplicate key (23505) and a foreign-key
// violation (23503), which mean opposite things here: the first says the rows
// are already stored and the buffer should be dropped, the second says the
// device is not registered and the buffer must be KEPT. Only the SQLSTATE
// separates them.
char lastPgCode_[8] = {0};

const char *reasonName(Reason r) {
  switch (r) {
    case Reason::Boot:   return "boot";
    case Reason::Change: return "change";
    default:             return "periodic";
  }
}

// Fields common to both payloads. device_id is deliberately NOT written here:
// the PATCH carries it in the URL filter and has no UPDATE privilege on that
// column, while the event INSERT adds it separately.
void writeCommon(JsonObject o, uint8_t stackCount, StackStatus stackStatus,
                 const LevelState *levels, const bool *sensorOk,
                 uint8_t sensorsOnline, battery::Level batteryLevel,
                 bool charging) {
  o["stack_count"] = stackCount;
  o["stack_status"] = BowlLogic::wireName(stackStatus);

  JsonArray lv = o["levels"].to<JsonArray>();
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    lv.add(BowlLogic::wireName(levels[i]));
  }

  JsonArray sk = o["sensors_ok"].to<JsonArray>();
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) sk.add(sensorOk[i]);

  o["sensors_online"] = sensorsOnline;

  // The BAND, not the percentage. A resting-voltage SoC estimate moves several
  // points with load, temperature, cell age and per-unit ADC calibration, so
  // publishing a number would invite the UI to render precision that is not
  // there. null when no cell is detected -- never a fabricated "critical",
  // which would be indistinguishable from a genuinely flat battery.
  //
  // Taken as decided by the hysteresis upstream, never recomputed here: the
  // band is the output of a state machine, and re-deriving it from a raw
  // percentage would reintroduce the boundary oscillation it exists to stop.
  if (batteryLevel == battery::Level::Unknown) {
    o["battery_level"] = nullptr;
  } else {
    o["battery_level"] = battery::levelName(batteryLevel);
  }

  o["charging"] = charging;
  o["firmware"] = BOWLSTACK_FW_VERSION;
}

// Supabase presents several URLs in its dashboard and only one of them is the
// API origin. Normalising here means a unit configured with the REST endpoint
// (".../rest/v1/") or a trailing slash still works, instead of silently
// building ".../rest/v1//rest/v1/device_status" and failing every request --
// a mistake worth tolerating once it is being repeated across 30 devices.
String apiBase() {
  String u = String(SUPABASE_URL);
  while (u.endsWith("/")) u.remove(u.length() - 1);
  if (u.endsWith("/rest/v1")) u.remove(u.length() - 8);
  while (u.endsWith("/")) u.remove(u.length() - 1);
  return u;
}

// Returns the HTTP status, or a negative HTTPClient error code.
// `rangeOut`, when non-null, receives the Content-Range header, which is how a
// PATCH reports how many rows it actually matched.
int request(const char *method, const char *path, const char *query,
            const char *prefer, const String &body, String *rangeOut = nullptr) {
  if (!net::connected()) return -1000;

  HTTPClient http;
  String url = apiBase() + path;
  if (query != nullptr && query[0] != '\0') {
    url += "?";
    url += query;
  }

  if (!http.begin(*tls_, url)) return -1001;

  // Reuse keeps the TLS session alive across posts. Without it each request
  // re-downloads the certificate chain, which dominates egress far more than
  // the payloads themselves.
  http.setReuse(true);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", prefer);

  static const char *kHeaders[] = {"Content-Range"};
  http.collectHeaders(kHeaders, 1);

  const int code = http.sendRequest(method, (uint8_t *)body.c_str(), body.length());

  if (rangeOut != nullptr) *rangeOut = http.header("Content-Range");

  lastPgCode_[0] = '\0';

  // Only read the body on failure -- on success it is empty anyway thanks to
  // return=minimal, and the error text is what we want for diagnosis.
  if (code < 200 || code >= 300) {
    const String err = http.getString();
    Serial.printf("telemetry: %s %s -> %d %s\n", method, path, code,
                  err.substring(0, 180).c_str());

    // Extract the SQLSTATE so the caller can tell 23505 from 23503.
    //
    // Extracted here but ACTED ON by the caller. This function is per-process
    // and knows nothing about channels, while "this device is not registered" is
    // a fact about one installation -- and a batch carrying rows for several
    // installations cannot attribute a foreign-key violation from the SQLSTATE
    // alone. flushChannels() resolves that by re-sending one channel at a time.
    const int at = err.indexOf("\"code\":\"");
    if (at >= 0 && (int)err.length() >= at + 13) {
      err.substring(at + 8, at + 13).toCharArray(lastPgCode_, sizeof(lastPgCode_));
    }
  }

  http.end();
  return code;
}

// History. A plain INSERT -- no ON CONFLICT, because upserts require the anon
// role to hold full-table SELECT plus an RLS SELECT policy, which would let any
// device read every installation's telemetry. Idempotency instead comes from
// the (device_id, boot_id, seq) unique constraint: a retried batch raises
// 23505, which means the rows are already recorded and the batch can be
// dropped.
void appendEvents(JsonArray arr, const Channel &ch, uint32_t now) {
  for (uint8_t i = 0; i < ch.count; i++) {
    const QueuedEvent &e = ch.queue[(ch.head + i) % QUEUE_LEN];
    JsonObject o = arr.add<JsonObject>();

    o["device_id"] = ch.deviceId;
    o["boot_id"] = ch.bootId;
    o["seq"] = e.seq;

    // The clock-free timestamp. The device has no RTC, so it reports how long
    // ago the event happened and the server subtracts that from now().
    // Unsigned arithmetic makes this correct across the millis() wrap.
    uint32_t age = now - e.atMs;
    if (age > AGE_MAX_MS) age = AGE_MAX_MS;
    o["age_ms"] = age;

    o["reason"] = reasonName(e.reason);
    writeCommon(o, e.stackCount, e.stackStatus, e.levels, e.sensorOk,
                e.sensorsOnline, e.batteryLevel, e.charging);
  }
}

void clearQueue(Channel &ch) {
  ch.head = 0;
  ch.count = 0;
}

// Flushes the queued history of `n` channels in ONE POST. n == 1 is the
// production path; the fleet simulator passes many.
bool flushChannels(Channel **chs, uint8_t n) {
  uint16_t total = 0;
  for (uint8_t i = 0; i < n; i++) total += chs[i]->count;
  if (total == 0) return true;

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  const uint32_t now = millis();
  for (uint8_t i = 0; i < n; i++) appendEvents(arr, *chs[i], now);

  String body;
  serializeJson(doc, body);

  const int code = request("POST", "/rest/v1/status_events", nullptr,
                           "return=minimal", body);

  if (code >= 200 && code < 300) {
    for (uint8_t i = 0; i < n; i++) clearQueue(*chs[i]);
    return true;
  }

  // 23505, a duplicate key: these events are already stored, so the batch has
  // done its job and must be dropped rather than retried forever.
  if (strcmp(lastPgCode_, "23505") == 0) {
    Serial.println("telemetry: events already recorded (23505), clearing buffer");
    for (uint8_t i = 0; i < n; i++) clearQueue(*chs[i]);
    return true;
  }

  // 23503, a foreign-key violation: some device in this batch is not registered.
  // PostgREST reports this as 409 too, so keying off the status alone would
  // discard the buffer -- losing real history over a provisioning mistake that
  // is about to be fixed. Keep it.
  if (strcmp(lastPgCode_, "23503") == 0) {
    if (n == 1) {
      chs[0]->unprovisioned = true;
      Serial.printf("telemetry: %s not registered - keeping buffered events\n",
                    chs[0]->deviceId);
      return false;
    }
    // A shared batch cannot say WHICH device_id was rejected, and marking all of
    // them would hide a single provisioning gap behind 30 healthy devices. Re-send
    // one at a time to identify the offender exactly. Expensive, but only in a
    // failure case that needs a human anyway.
    Serial.printf("telemetry: 23503 in a %u-device batch - isolating\n", n);
    bool allOk = true;
    for (uint8_t i = 0; i < n; i++) {
      if (!flushChannels(&chs[i], 1)) allOk = false;
    }
    return allOk;
  }

  // Any other 4xx means the batch is malformed and will never be accepted;
  // keeping it would block the queue permanently.
  if (code >= 400 && code < 500 && code != 401 && code != 403) {
    Serial.println("telemetry: dropping unacceptable batch");
    for (uint8_t i = 0; i < n; i++) clearQueue(*chs[i]);
  }
  return false;
}

bool flushEvents(Channel &ch) {
  Channel *one = &ch;
  return flushChannels(&one, 1);
}

// Current state. A plain UPDATE via PostgREST PATCH; the row is created
// server-side when the device is registered, so no insert path is needed.
bool patchStatus(Channel &ch, const DeviceStatus &s) {
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();

  // No device_id in the body: it is the URL filter, and anon holds no UPDATE
  // privilege on that column.
  o["boot_id"] = ch.bootId;
  o["uptime_s"] = s.uptimeSec;
  writeCommon(o, s.stackCount, s.stackStatus, s.levels, s.sensorOnline,
              s.sensorsOnline, s.batteryLevel, s.charging);

  // Clamped to the schema's CHECK bound. A floating ADC pin reads far above any
  // real cell -- 6365 mV was measured on this hardware -- and the raw value
  // would violate `check (battery_mv between 0 and 6000)`, which PostgREST
  // reports as 400. patchStatus() would then fail on every attempt for the life
  // of the fault, so a broken battery divider would silently stop the BOWL COUNT
  // from ever reaching Supabase. The clamped value is still impossible for a
  // Li-ion cell, so it remains the wiring diagnostic it exists to be.
  o["battery_mv"] = s.batteryMv > config::BATTERY_PUBLISH_MAX_MV
                        ? config::BATTERY_PUBLISH_MAX_MV
                        : s.batteryMv;
  o["mac"] = s.mac;

  String body;
  serializeJson(doc, body);

  const String query = String("device_id=eq.") + ch.deviceId;

  // count=exact is what makes an unregistered device loud. A PATCH matching no
  // rows is a perfectly successful 204 -- without the count we could not tell
  // "reported" from "wrote nothing at all, forever".
  String range;
  const int code = request("PATCH", "/rest/v1/device_status", query.c_str(),
                           "return=minimal,count=exact", body, &range);

  if (code < 200 || code >= 300) return false;

  // Content-Range looks like "0-0/1" on success, "*/0" when nothing matched.
  if (range.endsWith("/0")) {
    Serial.printf("telemetry: no device_status row for '%s' - register it in "
                  "the devices table\n",
                  ch.deviceId);
    ch.unprovisioned = true;
    return false;
  }

  return true;
}

}  // namespace

void openChannel(Channel &ch, const char *deviceId) {
  ch.deviceId = deviceId;
  // Per channel, not per process. A virtual device must look like its own
  // installation, and boot_id is half of the idempotency key -- sharing one
  // across 31 nodes would make (device_id, boot_id, seq) collide the moment two
  // nodes reached the same seq.
  ch.bootId = esp_random();
}

void begin() {
  openChannel(default_, BOWLSTACK_DEVICE_ID);

  tls_ = new WiFiClientSecure();
  // No certificate pinning: the anon key grants write-only access with no read
  // path, so an intercepted session yields nothing readable and can at worst
  // inject telemetry. Pin a root CA here if that ever changes.
  tls_->setInsecure();

  // Print the EFFECTIVE base, not the raw macro: if normalisation changed it,
  // that difference is the first thing worth seeing when requests fail.
  Serial.printf("telemetry: boot_id=%u -> %s\n", default_.bootId,
                apiBase().c_str());
}

void enqueue(Channel &ch, const DeviceStatus &s, Reason reason, uint32_t seq) {
  QueuedEvent e;
  e.atMs = millis();
  e.seq = seq;
  e.reason = reason;
  e.stackCount = s.stackCount;
  e.stackStatus = s.stackStatus;
  e.sensorsOnline = s.sensorsOnline;
  e.batteryLevel = s.batteryLevel;
  e.charging = s.charging;
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    e.levels[i] = s.levels[i];
    e.sensorOk[i] = s.sensorOnline[i];
  }

  if (ch.count == QUEUE_LEN) {
    ch.head = (ch.head + 1) % QUEUE_LEN;  // drop oldest
    ch.count--;
  }
  ch.queue[(ch.head + ch.count) % QUEUE_LEN] = e;
  ch.count++;

  // statusPending is deliberately NOT set here: enqueueing history and asking
  // for the state row to be refreshed are separate decisions, and the caller
  // makes both explicitly. loop() posts a queued batch on its own anyway, so an
  // event is never stranded by the flag being clear.
}

void requestImmediateUpsert(Channel &ch) {
  // Just arms the flag. Rate limiting lives in loop(), in one place, where it
  // covers the event POST as well -- this function used to throttle here and
  // gate only the PATCH, which left the POST path unlimited and made the limit
  // ineffective against exactly the flapping it was written for.
  ch.statusPending = true;
}

bool flushMany(Channel **channels, uint8_t n) {
  return flushChannels(channels, n);
}

void loop(Channel &ch, const DeviceStatus &current) {
  if (!net::connected()) return;

  const uint32_t now = millis();
  if (ch.backoffActive) {
    if ((int32_t)(now - ch.backoffUntilMs) < 0) return;
    ch.backoffActive = false;
  }

  if (ch.unprovisioned) {
    // Latched: only a human inserting a `devices` row fixes this. Back off hard
    // rather than hammering the endpoint for the life of the device.
    ch.backoffUntilMs = now + UNPROVISIONED_RETRY_MS;
    ch.backoffActive = true;
    ch.unprovisioned = false;  // allow one probe per interval
    Serial.printf("telemetry: %s not registered in `devices` - backing off\n",
                  ch.deviceId);
    return;
  }

  const bool due = (int32_t)(now - ch.nextStatusMs) >= 0;

  // Nothing to say. Checked before the rate limiter so an idle device does not
  // consume its window and then delay a change that arrives a moment later.
  if (ch.count == 0 && !ch.statusPending && !due) return;

  // The per-device floor. Unsigned subtraction, so it is correct across the
  // millis() wrap at ~49.7 days; everPosted keeps the very first report
  // immediate instead of making a freshly booted device wait out a window it
  // has no history for.
  if (ch.everPosted && (uint32_t)(now - ch.lastPostMs) < POST_MIN_INTERVAL_MS) {
    return;
  }
  ch.lastPostMs = now;
  ch.everPosted = true;

  // History first, oldest-first, so a reconnect replays what happened while
  // offline before the current-state row is overwritten. Everything queued
  // since the last round goes in ONE batch -- that is the clubbing: the writes
  // are coalesced, the events themselves are not merged or dropped, so each
  // keeps its own recorded_at and the transition history stays intact.
  if (ch.count > 0 && !flushEvents(ch)) {
    ch.backoffUntilMs = now + RETRY_PERIOD_MS;
    ch.backoffActive = true;
    ch.lastOk = false;
    return;
  }

  if (!ch.statusPending && !due) return;

  // `current` is the caller's latest snapshot, so the state row always carries
  // the newest values regardless of how many changes were clubbed into the
  // batch above.
  if (patchStatus(ch, current)) {
    ch.statusPending = false;
    ch.nextStatusMs = now + STATUS_PERIOD_MS;
    ch.lastOk = true;
  } else {
    ch.backoffUntilMs = now + RETRY_PERIOD_MS;
    ch.backoffActive = true;
    ch.lastOk = false;
  }
}

// --- default-channel wrappers ----------------------------------------------
// The production firmware calls only these, so nothing outside this file needs
// to know channels exist.

void enqueue(const DeviceStatus &s, Reason reason, uint32_t seq) {
  enqueue(default_, s, reason, seq);
}
void requestImmediateUpsert() { requestImmediateUpsert(default_); }
void loop(const DeviceStatus &current) { loop(default_, current); }

uint8_t queued() { return default_.count; }
uint32_t bootId() { return default_.bootId; }
bool lastPostOk() { return default_.lastOk; }
bool unprovisioned() { return default_.unprovisioned; }

}  // namespace telemetry
