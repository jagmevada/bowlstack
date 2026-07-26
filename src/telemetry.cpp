#include "telemetry.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "bowl_logic.h"
#include "net.h"
#include "secret.h"
#include "version.h"

namespace telemetry {
namespace {

// Events held while offline. Small on purpose: the current state is re-sent on
// every reconnect and every boot, so the buffer preserves only the TIMING of
// intermediate changes, not the truth of the present.
const uint8_t QUEUE_LEN = 32;

// Heartbeat. Changes post immediately, so this only proves liveness. At 10 s
// across 30 devices the fleet would make ~7.8M requests/month, and TLS
// handshakes alone would dominate the egress budget.
const uint32_t STATUS_PERIOD_MS = 60000;

// Backoff after a failed post, so a dead uplink cannot spin the loop.
const uint32_t RETRY_PERIOD_MS = 15000;

// Backoff once the server says this device is not registered. Retrying cannot
// fix that; only a human inserting a `devices` row can.
const uint32_t UNPROVISIONED_RETRY_MS = 300000;

// Matches the server-side clamp in tg_status_events_stamp().
const uint32_t AGE_MAX_MS = 604800000;

const uint32_t HTTP_TIMEOUT_MS = 8000;

struct QueuedEvent {
  uint32_t atMs;  // millis() when queued; converted to age at send time
  uint32_t seq;
  Reason reason;
  uint8_t stackCount;
  StackStatus stackStatus;
  LevelState levels[config::SENSOR_COUNT];
  bool sensorOk[config::SENSOR_COUNT];
  uint8_t sensorsOnline;
  int8_t batteryPercent;
  bool charging;
};

QueuedEvent queue_[QUEUE_LEN];
uint8_t head_ = 0;  // oldest
uint8_t count_ = 0;
uint32_t nextSeq_ = 0;

uint32_t bootId_ = 0;
bool lastOk_ = false;
bool unprovisioned_ = false;
uint32_t nextStatusMs_ = 0;
uint32_t backoffUntilMs_ = 0;
bool statusPending_ = true;  // always report once at boot

WiFiClientSecure *tls_ = nullptr;

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
                 uint8_t sensorsOnline, int8_t batteryPercent, bool charging) {
  o["stack_count"] = stackCount;
  o["stack_status"] = BowlLogic::wireName(stackStatus);

  JsonArray lv = o["levels"].to<JsonArray>();
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    lv.add(BowlLogic::wireName(levels[i]));
  }

  JsonArray sk = o["sensors_ok"].to<JsonArray>();
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) sk.add(sensorOk[i]);

  o["sensors_online"] = sensorsOnline;

  // null, never -1. The server column is CHECKed 0..100, and a sentinel number
  // would silently poison any average taken over it.
  if (batteryPercent < 0) {
    o["battery_pct"] = nullptr;
  } else {
    o["battery_pct"] = batteryPercent;
  }

  o["charging"] = charging;
  o["firmware"] = BOWLSTACK_FW_VERSION;
}

// Returns the HTTP status, or a negative HTTPClient error code.
// `rangeOut`, when non-null, receives the Content-Range header, which is how a
// PATCH reports how many rows it actually matched.
int request(const char *method, const char *path, const char *query,
            const char *prefer, const String &body, String *rangeOut = nullptr) {
  if (!net::connected()) return -1000;

  HTTPClient http;
  String url = String(SUPABASE_URL) + path;
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

  // Only read the body on failure -- on success it is empty anyway thanks to
  // return=minimal, and the error text is what we want for diagnosis.
  if (code < 200 || code >= 300) {
    const String err = http.getString();
    Serial.printf("telemetry: %s %s -> %d %s\n", method, path, code,
                  err.substring(0, 180).c_str());

    // 23503 is a foreign-key violation: this device_id is not in `devices`.
    // No amount of retrying fixes a provisioning gap.
    if (err.indexOf("23503") >= 0) unprovisioned_ = true;
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
bool flushEvents() {
  if (count_ == 0) return true;

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  const uint32_t now = millis();
  for (uint8_t i = 0; i < count_; i++) {
    const QueuedEvent &e = queue_[(head_ + i) % QUEUE_LEN];
    JsonObject o = arr.add<JsonObject>();

    o["device_id"] = BOWLSTACK_DEVICE_ID;
    o["boot_id"] = bootId_;
    o["seq"] = e.seq;

    // The clock-free timestamp. The device has no RTC, so it reports how long
    // ago the event happened and the server subtracts that from now().
    // Unsigned arithmetic makes this correct across the millis() wrap.
    uint32_t age = now - e.atMs;
    if (age > AGE_MAX_MS) age = AGE_MAX_MS;
    o["age_ms"] = age;

    o["reason"] = reasonName(e.reason);
    writeCommon(o, e.stackCount, e.stackStatus, e.levels, e.sensorOk,
                e.sensorsOnline, e.batteryPercent, e.charging);
  }

  String body;
  serializeJson(doc, body);

  const int code = request("POST", "/rest/v1/status_events", nullptr,
                           "return=minimal", body);

  if (code >= 200 && code < 300) {
    head_ = 0;
    count_ = 0;
    return true;
  }

  // 409 is the unique-constraint violation: these events are already on the
  // server, so the batch has done its job and must be dropped rather than
  // retried forever.
  if (code == 409) {
    Serial.println("telemetry: events already recorded (409), clearing buffer");
    head_ = 0;
    count_ = 0;
    return true;
  }

  // Any other 4xx means the batch is malformed and will never be accepted;
  // keeping it would block the queue permanently.
  if (code >= 400 && code < 500 && code != 401 && code != 403) {
    Serial.println("telemetry: dropping unacceptable batch");
    head_ = 0;
    count_ = 0;
  }
  return false;
}

// Current state. A plain UPDATE via PostgREST PATCH; the row is created
// server-side when the device is registered, so no insert path is needed.
bool patchStatus(const DeviceStatus &s) {
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();

  // No device_id in the body: it is the URL filter, and anon holds no UPDATE
  // privilege on that column.
  o["boot_id"] = bootId_;
  o["uptime_s"] = s.uptimeSec;
  writeCommon(o, s.stackCount, s.stackStatus, s.levels, s.sensorOnline,
              s.sensorsOnline, s.batteryPercent, s.charging);
  o["battery_mv"] = s.batteryMv;
  o["mac"] = s.mac;

  String body;
  serializeJson(doc, body);

  const String query = String("device_id=eq.") + BOWLSTACK_DEVICE_ID;

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
                  BOWLSTACK_DEVICE_ID);
    unprovisioned_ = true;
    return false;
  }

  return true;
}

}  // namespace

void begin() {
  bootId_ = esp_random();

  tls_ = new WiFiClientSecure();
  // No certificate pinning: the anon key grants write-only access with no read
  // path, so an intercepted session yields nothing readable and can at worst
  // inject telemetry. Pin a root CA here if that ever changes.
  tls_->setInsecure();

  Serial.printf("telemetry: boot_id=%u -> %s\n", bootId_, SUPABASE_URL);
}

void enqueue(const DeviceStatus &s, Reason reason) {
  // seq increments on ENQUEUE, not on send, so a dropped entry leaves a visible
  // gap in the server-side sequence instead of vanishing silently.
  QueuedEvent e;
  e.atMs = millis();
  e.seq = nextSeq_++;
  e.reason = reason;
  e.stackCount = s.stackCount;
  e.stackStatus = s.stackStatus;
  e.sensorsOnline = s.sensorsOnline;
  e.batteryPercent = s.batteryPercent;
  e.charging = s.charging;
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    e.levels[i] = s.levels[i];
    e.sensorOk[i] = s.sensorOnline[i];
  }

  if (count_ == QUEUE_LEN) {
    head_ = (head_ + 1) % QUEUE_LEN;  // drop oldest
    count_--;
  }
  queue_[(head_ + count_) % QUEUE_LEN] = e;
  count_++;

  statusPending_ = true;
}

void requestImmediateUpsert() { statusPending_ = true; }

void loop(const DeviceStatus &current) {
  if (!net::connected()) return;

  const uint32_t now = millis();
  if ((int32_t)(now - backoffUntilMs_) < 0) return;

  if (unprovisioned_) {
    // Latched: only a human inserting a `devices` row fixes this. Back off hard
    // rather than hammering the endpoint for the life of the device.
    backoffUntilMs_ = now + UNPROVISIONED_RETRY_MS;
    unprovisioned_ = false;  // allow one probe per interval
    Serial.println("telemetry: device not registered in `devices` - backing off");
    return;
  }

  // History first, oldest-first, so a reconnect replays what happened while
  // offline before the current-state row is overwritten.
  if (count_ > 0 && !flushEvents()) {
    backoffUntilMs_ = now + RETRY_PERIOD_MS;
    lastOk_ = false;
    return;
  }

  const bool due = (int32_t)(now - nextStatusMs_) >= 0;
  if (!statusPending_ && !due) return;

  if (patchStatus(current)) {
    statusPending_ = false;
    nextStatusMs_ = now + STATUS_PERIOD_MS;
    lastOk_ = true;
  } else {
    backoffUntilMs_ = now + RETRY_PERIOD_MS;
    lastOk_ = false;
  }
}

uint8_t queued() { return count_; }
uint32_t bootId() { return bootId_; }
bool lastPostOk() { return lastOk_; }
bool unprovisioned() { return unprovisioned_; }

}  // namespace telemetry
