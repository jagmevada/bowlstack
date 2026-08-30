#include "scale_telemetry.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_mac.h>
#include <math.h>

#include "bringup_wifi.h"
#include "config.h"
#include "inputs.h"
#include "uplink.h"
#include "version.h"

namespace scale_telemetry {
namespace {

// --- cadence ---------------------------------------------------------------
// The same three numbers telemetry.cpp uses, and deliberately not different
// ones: public.offline_after() is a single 40 s threshold for the WHOLE fleet,
// sized as `offline_after() > STATUS_PERIOD_MS + RETRY_PERIOD_MS`. A scale that
// heartbeat more slowly than a stack would alarm in the gap between its own
// posts, and there is no per-kind threshold to widen.
const uint32_t STATUS_PERIOD_MS = 20000;
const uint32_t RETRY_PERIOD_MS = 15000;
const uint32_t POST_MIN_INTERVAL_MS = 5000;
const uint32_t UNPROVISIONED_RETRY_MS = 300000;

// How far the weight must move to be worth an immediate post rather than
// waiting out the heartbeat.
//
// THIS IS THE ONE PLACE A SCALE DIFFERS IN KIND FROM A BOWL COUNTER, and it is
// worth stating plainly. A bowl count is a small integer that changes a few
// times a service, so telemetry.cpp can treat EVERY change as news. A weight is
// continuous and never stops moving -- at ~104 counts/g a still platform
// wanders a gram or two, and somebody leaning on the counter moves it by
// hundreds. Posting on change would mean posting forever.
//
// So the heartbeat carries the weight, and a change only jumps the queue when
// it is big enough to be an EVENT: a bowl going on or coming off. 250 g is
// below any single serving and far above the noise floor, so it catches the
// thing an operator cares about without firing on a draught.
//
// It is measured against the last POSTED weight, not the last sample, so a slow
// drift accumulates into one post rather than being outrun by the comparison.
const int32_t WEIGHT_EVENT_G = 250;

// --- the history --------------------------------------------------------
// How often a sample is appended to weight_samples REGARDLESS of whether the
// weight moved.
//
// CHANGE EVENTS ALONE DO NOT MAKE A RATE. A threshold-triggered series is
// dense where consumption is fast and sparse where it is slow -- which is
// exactly backwards for estimating a slope, because the slow stretches are
// where the estimate needs the most support and the fast ones already speak
// for themselves. Worse, a counter that is not moving emits NOTHING, so
// "steady for twenty minutes" and "the station lost WiFi for twenty minutes"
// produce the same empty gap.
//
// So there is a heartbeat in the history too, on its own slow clock: a regular
// time base for the fit, and a positive statement that the reading was still
// being taken.
//
// 120 s is chosen against storage rather than against statistics. A three-hour
// meal gives ~90 points per station, far more than a slope needs; at 24
// stations over an 8 h service day it is ~5,800 rows/day, the same order as
// the bowl fleet's ~282/day once change events are added. One minute would
// double it for a curve nobody could read differently.
const uint32_t SAMPLE_PERIOD_MS = 120000;

// Samples held while offline. 32 at the periodic cadence is ~64 minutes of
// buffering, which covers a WiFi outage without losing a meal's curve. Not
// raised further because the batch is serialised into ONE JsonDocument and
// doubling the queue doubles a several-kilobyte heap allocation on a board
// that also holds a TLS session -- the same trade telemetry.cpp records.
//
// On overflow the OLDEST is dropped and its seq is never sent, so the gap is
// visible server-side as a jump in seq rather than as a smooth curve that
// quietly omits the busiest ten minutes of the service.
const uint8_t QUEUE_LEN = 32;

struct QueuedSample {
  uint32_t atMs;  // millis() when queued; converted to an age at send time
  uint32_t seq;
  const char *reason;      // "boot" | "change" | "periodic"
  const char *state;       // weight_state; never null
  int32_t weightG;
  bool haveWeight;
  uint8_t cellsOnline;
  int32_t netCounts;
  bool haveNetCounts;
  float countsPerGram;     // 0 = never calibrated -> sent as null
  uint16_t batteryMv;
  battery::Level batteryLevel;
  uint8_t manualFillPct;  // TRIAL: captured with the sample, not at flush
};

QueuedSample queue_[QUEUE_LEN];
uint8_t qHead_ = 0;   // oldest
uint8_t qCount_ = 0;

// Allocated when a sample is OBSERVED, not when it reaches the server. A
// sample dropped between those two points must still consume a seq, or the
// server sees a contiguous sequence and has no evidence anything was lost --
// which is the one thing seq exists to prove.
uint32_t nextSeq_ = 0;

uint32_t nextSampleMs_ = 0;
uint32_t samplesPosted_ = 0;

// WHAT THE HISTORY LAST RECORDED, which is NOT what was last posted, and
// conflating the two put nine rows in the database for one bowl.
//
// The post-urgency test compares against the last POSTED weight -- correctly,
// because its question is "does the server's copy need replacing". But
// enqueueing runs on the 250 ms task tick, so while that comparison stays true
// -- which it does for the whole 5 s until a post succeeds and moves the
// reference -- every single tick enqueued another sample.
//
// Observed on the live device the first evening it recorded anything: a bowl
// placed on the platform produced seq 6..14, nine rows spanning two seconds,
// every one of them the same event mid-settle. It is not merely untidy. Those
// rows are transients, they are what a rate estimator would fit a slope
// through, and thirty-two of them fill the offline buffer in eight seconds.
//
// So the history keeps its OWN reference and its own floor: a sample is
// recorded when the weight has moved since the last RECORDED one, and not
// more often than the floor allows.
int32_t lastEnqWeightG_ = 0;
bool lastEnqHadWeight_ = false;
const char *lastEnqState_ = nullptr;
uint32_t lastEnqMs_ = 0;
bool everEnqueued_ = false;

// The least time between two history rows from the same station. Five seconds
// is long enough that placing a bowl is one row rather than twenty, and short
// enough that two genuinely separate servings are never merged -- nobody
// serves twice from one counter inside five seconds.
//
// A state CHANGE bypasses this: a cell dropping out is not a transient to be
// smoothed, it is the thing the row exists to record.
const uint32_t MIN_SAMPLE_GAP_MS = 5000;

// Clamped server-side too, but doing it here keeps a wrapped millis() from
// producing an age the CHECK rejects -- which would fail the whole batch.
const uint32_t AGE_MAX_MS = 604800000;

uplink::Result lastResult_;
const char *deviceId_ = nullptr;
uint32_t bootId_ = 0;
char mac_[18] = {0};

bool statusPending_ = true;  // always report once at boot
bool lastOk_ = false;

// Two flags for one fact, and they are not redundant.
//
// unprovisioned_ is the WORK item: loop() clears it as soon as it has armed the
// backoff, so that exactly one probe goes out per interval rather than one per
// pass. That makes it useless as a status, because it is false almost always --
// which is what the console line reporting it discovered by never printing.
//
// everUnprovisioned_ is the STATUS: latched, and cleared only by a post that
// actually succeeds. It is what somebody standing at the device needs to see,
// because "this unit is not in the devices table" is a provisioning error that
// no amount of waiting fixes.
bool unprovisioned_ = false;
bool everUnprovisioned_ = false;

bool backoffActive_ = false;
uint32_t backoffUntilMs_ = 0;
uint32_t nextStatusMs_ = 0;

// Armed only once something has actually been posted. Not a bare timestamp
// compared as (int32_t)(now - 0): that inverts once millis() passes 2^31
// (~24.9 days), which would silently halt telemetry on a device that had never
// failed a post.
bool everPosted_ = false;
uint32_t lastPostMs_ = 0;
uint32_t posts_ = 0;

// What the server currently believes, so a change can be recognised. Seeded
// invalid rather than zero -- zero is a real weight.
bool haveReported_ = false;
int32_t reportedWeightG_ = 0;
bool reportedHadWeight_ = false;
const char *reportedState_ = nullptr;
battery::Level reportedBand_ = battery::Level::Unknown;

// --- what the scale honestly knows -----------------------------------------
// THE PRIORITY ORDER IS THE POINT: the fault NEAREST THE HARDWARE wins, because
// that is the one somebody can act on. An uncalibrated scale with a dead cell
// should send somebody to the cell, not to the calibration menu.
//
// Every branch except the last returns a state with NO gram figure, and the
// database enforces that pairing (device_status_weight_agrees_ck). If this
// function and that constraint ever disagree, the PATCH fails with a 400 and
// the station stops reporting entirely -- so the two are deliberately written
// to mirror each other line for line.
// The seven states, as NAMED CONSTANTS rather than bare literals at each
// return.
//
// Change detection compares these by POINTER -- `state != reportedState_` --
// which is the cheap and correct thing to do for a small closed set, but it is
// only correct if one state is always the same pointer. With bare literals
// that holds by compiler literal-pooling rather than by any rule: -Os merges
// identical strings in a translation unit, so it worked, and nothing would
// have told us if it stopped.
//
// It is not hypothetical here. OVER_RANGE is returned from TWO places -- a
// saturated converter, and a gram figure outside what the column accepts --
// and under a build that did not merge them those two would compare as
// different states. The visible effect is a station that posts on every tick
// while flipping between two spellings of the same fault, and enqueues a
// history row each time: the nine-rows-for-one-bowl failure again, by a
// different route.
const char *const ST_NO_CELLS      = "no_cells";
const char *const ST_CELLS_PARTIAL = "cells_partial";
const char *const ST_OVER_RANGE    = "over_range";
const char *const ST_SETTLING      = "settling";
const char *const ST_UNCALIBRATED  = "uncalibrated";
const char *const ST_UNTARED       = "untared";
const char *const ST_OK            = "ok";

const char *weightState(const scale::Snapshot &s) {
  // No converter is answering. This is also what a missing or unpowered mux
  // looks like from up here, which is why the boot console says so explicitly
  // rather than leaving three dead cells to read as three faults.
  if (s.online == 0) return ST_NO_CELLS;

  // Cells under one platform SUM. A missing cell therefore makes the total
  // silently LOW rather than noisy -- it looks exactly like a lighter bowl --
  // so a partial assembly has no weight at all, not a partial one.
  if (s.online < scale::CELLS) return ST_CELLS_PARTIAL;

  // A saturated converter returns a large, steady, plausible number and the
  // weight simply stops rising. Said rather than inferred.
  if (s.overRange) return ST_OVER_RANGE;

  // The automatic power-up tare has not concluded. Waiting and Observing are
  // both "no zero yet"; GaveUp and Off mean it is not coming, and the tared
  // check below is then the one that matters.
  if (s.autoTare == scale::AutoTare::Waiting ||
      s.autoTare == scale::AutoTare::Observing) {
    return ST_SETTLING;
  }

  // No known mass has ever been applied to this unit, so there is no
  // counts-to-gram factor and there are no grams. net_counts still goes out, so
  // the dashboard can see the cells are alive.
  if (!s.calibrated) return ST_UNCALIBRATED;

  // Calibrated but not zeroed: the figure is right about CHANGE and wrong about
  // the absolute, because it still includes the platform. That is not a weight
  // of the food, which is what the column means.
  if (!s.tared) return ST_UNTARED;

  // OUTSIDE WHAT THE COLUMN WILL ACCEPT, which on this hardware means the
  // calibration factor is wrong by orders of magnitude rather than that the
  // platform is holding 400 kg -- three 20 kg cells cannot.
  //
  // Reported as over_range rather than clamped. Clamping is right for
  // battery_mv, where 6000 is still impossible for a cell and therefore still
  // says "the divider is broken"; clamping a weight to 100 kg produces a number
  // that looks like food. And sending it unclamped is worse than either: the
  // CHECK rejects it, PostgREST answers 400, and the station stops reporting
  // even the state that would have explained the fault.
  const float g = s.totalGrams;
  if (!(g >= (float)config::WEIGHT_PUBLISH_MIN_G &&
        g <= (float)config::WEIGHT_PUBLISH_MAX_G)) {
    // Written as !(in range) rather than (out of range) so a NaN -- which
    // compares false against everything -- lands here too instead of being
    // serialised as `null` by ArduinoJson beside a state of 'ok'.
    return ST_OVER_RANGE;
  }

  return ST_OK;
}

// The tare- and zero-subtracted sum over the online cells -- exactly the figure
// the panel shows when it has no calibration, computed the same way
// publishScale() computes it for the screen. Sent in every state but no_cells,
// so there is always evidence the cells are moving even when there is no gram
// figure to send.
int32_t netCounts(const scale::Snapshot &s) {
  int32_t total = 0;
  for (uint8_t i = 0; i < scale::CELLS; i++) {
    if (s.cell[i].state != CellState::Online) continue;
    total += s.cell[i].counts - s.cell[i].platformZero - s.cell[i].tare;
  }
  return total;
}

// --- the history queue ------------------------------------------------
void enqueue(const scale::Snapshot &s, const char *reason, const char *state,
             int32_t weightG, bool haveWeight, uint16_t batteryMv,
             battery::Level batteryLevel) {
  QueuedSample e;
  e.atMs = millis();
  e.seq = nextSeq_++;
  e.reason = reason;
  e.state = state;
  e.weightG = weightG;
  e.haveWeight = haveWeight;
  e.cellsOnline = s.online;
  e.netCounts = netCounts(s);
  e.haveNetCounts = (s.online > 0);
  e.countsPerGram = s.countsPerGram;
  e.batteryMv = batteryMv;
  e.batteryLevel = batteryLevel;
  e.manualFillPct = inputs::fillPercent();

  if (qCount_ == QUEUE_LEN) {
    // Drop the OLDEST and keep the newest. The newest describes the counter as
    // it is now; the stale one is the expendable entry. Its seq is simply never
    // sent, so the server sees a gap and knows the curve is incomplete rather
    // than reading a smooth line through the missing stretch.
    Serial.printf("scale-uplink: history buffer full, dropping seq %lu\n",
                  (unsigned long)queue_[qHead_].seq);
    qHead_ = (uint8_t)((qHead_ + 1) % QUEUE_LEN);
    qCount_--;
  }
  queue_[(qHead_ + qCount_) % QUEUE_LEN] = e;
  qCount_++;
}

// Appends everything buffered in ONE POST. Returns false to keep the buffer.
bool flushSamples() {
  if (qCount_ == 0) return true;

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  const uint32_t now = millis();

  for (uint8_t i = 0; i < qCount_; i++) {
    const QueuedSample &e = queue_[(qHead_ + i) % QUEUE_LEN];
    JsonObject o = arr.add<JsonObject>();
    o["device_id"] = deviceId_;
    o["boot_id"] = bootId_;
    o["seq"] = e.seq;
    // The clock-free timestamp. The device has no RTC, so it reports how long
    // ago the sample was taken and the server subtracts that from now().
    // Unsigned arithmetic makes this correct across the millis() wrap.
    uint32_t age = now - e.atMs;
    if (age > AGE_MAX_MS) age = AGE_MAX_MS;
    o["age_ms"] = age;
    o["reason"] = e.reason;
    o["weight_state"] = e.state;
    if (e.haveWeight) o["weight_g"] = e.weightG;
    else o["weight_g"] = nullptr;
    o["cells_online"] = e.cellsOnline;
    if (e.haveNetCounts) o["net_counts"] = e.netCounts;
    else o["net_counts"] = nullptr;
    if (e.countsPerGram > 0.0f) o["counts_per_gram"] = e.countsPerGram;
    else o["counts_per_gram"] = nullptr;
    o["battery_mv"] = e.batteryMv > config::BATTERY_PUBLISH_MAX_MV
                          ? config::BATTERY_PUBLISH_MAX_MV
                          : e.batteryMv;
    if (e.batteryLevel == battery::Level::Unknown) o["battery_level"] = nullptr;
    else o["battery_level"] = battery::levelName(e.batteryLevel);
    // TRIAL: the estimate as it stood when this sample was TAKEN, not as it
    // stands now. The whole point is pairing it against the weight from the
    // same instant; reading it at flush time would compare an estimate to a
    // measurement made up to an hour earlier.
    o["manual_fill_pct"] = e.manualFillPct;
    o["firmware"] = BOWLSTACK_FW_VERSION;
  }

  String body;
  serializeJson(doc, body);

  const uplink::Result r =
      uplink::request("POST", "/rest/v1/weight_samples", nullptr,
                      "return=minimal", body);

  if (r.ok()) {
    samplesPosted_ += qCount_;
    qHead_ = 0;
    qCount_ = 0;
    return true;
  }

  // 23505, a duplicate key: these samples are already stored, so the batch has
  // done its job and must be dropped rather than retried forever.
  if (strcmp(r.pgCode, "23505") == 0) {
    Serial.println("scale-uplink: samples already recorded (23505), clearing");
    qHead_ = 0;
    qCount_ = 0;
    return true;
  }

  // 23503, a foreign-key violation: this device is not registered. PostgREST
  // answers 409 for that AND for 23505, so keying off the status alone would
  // discard real history over a provisioning mistake that is about to be fixed.
  // Keep it.
  if (strcmp(r.pgCode, "23503") == 0) {
    unprovisioned_ = true;
    everUnprovisioned_ = true;
    return false;
  }

  // Any other 4xx means the batch is malformed and will never be accepted;
  // keeping it would block the queue permanently. 404 is the one worth naming:
  // it means the table is not there, which is a migration nobody ran rather
  // than anything wrong with the data.
  if (r.code == 404) {
    Serial.println("scale-uplink: no weight_samples table -- run "
                   "supabase/migrate_weight_samples.sql. Dropping this batch; "
                   "current state is still being reported.");
    qHead_ = 0;
    qCount_ = 0;
    return false;
  }
  if (r.code >= 400 && r.code < 500 && r.code != 401 && r.code != 403) {
    Serial.printf("scale-uplink: dropping unacceptable history batch (%d)\n", r.code);
    qHead_ = 0;
    qCount_ = 0;
  }
  return false;
}

bool patchStatus(const scale::Snapshot &s, uint32_t uptimeSec, uint16_t batteryMv,
                 battery::Level batteryLevel, const char *state, int32_t weightG,
                 bool haveWeight) {
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();

  // No device_id in the body: it is the URL filter, and anon holds no UPDATE
  // privilege on that column.
  o["boot_id"] = bootId_;
  o["uptime_s"] = uptimeSec;
  o["firmware"] = BOWLSTACK_FW_VERSION;
  o["mac"] = mac_;

  // --- the load-cell columns ---
  o["weight_state"] = state;
  if (haveWeight) o["weight_g"] = weightG;
  else o["weight_g"] = nullptr;

  o["cells_online"] = s.online;
  // Two statements rather than a ternary: ArduinoJson has no JsonVariant that
  // can be built from an int on one branch and from nothing on the other, and
  // the null here is load-bearing -- with no cell converting there is no count
  // to report, and 0 counts is a reading rather than the absence of one.
  if (s.online > 0) o["net_counts"] = netCounts(s);
  else o["net_counts"] = nullptr;
  // Zero means never calibrated, and null is the honest wire form of that --
  // a factor of 0.000 would read as a calibration that produced nothing.
  if (s.countsPerGram > 0.0f) o["counts_per_gram"] = s.countsPerGram;
  else o["counts_per_gram"] = nullptr;

  // --- power, identical to the bowl counter's ---
  // Clamped to the schema's CHECK bound for the same reason telemetry.cpp
  // clamps it: a floating ADC pin reads far above any real cell, and the raw
  // value would violate `check (battery_mv between 0 and 6000)`, which
  // PostgREST reports as 400 -- so the whole PATCH fails and the station stops
  // reporting its WEIGHT because of a battery-wiring fault.
  o["battery_mv"] = batteryMv > config::BATTERY_PUBLISH_MAX_MV
                        ? config::BATTERY_PUBLISH_MAX_MV
                        : batteryMv;
  if (batteryLevel == battery::Level::Unknown) o["battery_level"] = nullptr;
  else o["battery_level"] = battery::levelName(batteryLevel);

  // NULL, NOT false. The ETA6098's STAT output drives the charge LED and
  // reaches no GPIO on this board, so charge state is genuinely unreadable --
  // and the column is nullable precisely so a firmware that cannot measure it
  // reports nothing instead of inventing "not charging", which is
  // indistinguishable from a real answer. See board_waveshare_s3.h section 5.
  o["charging"] = nullptr;

  // --- TRIAL HARNESS: the manual fill estimate ------------------------------
  // Sent BESIDE the weight, never instead of it. The experiment is the gap
  // between the two, so both have to arrive from the same instant -- and the
  // server reads this column from nothing except the comparison view.
  //
  // The AGE goes with it because staleness is itself a result. An estimate
  // nobody has refreshed for twenty minutes is exactly the failure mode a
  // knob-based system has, and a reviewer needs to be able to exclude those
  // rows -- or count them, which is the more interesting number.
  o["manual_fill_pct"] = inputs::fillPercent();
  o["manual_fill_age_s"] = inputs::fillAgeMs() / 1000;

  // stack_count, stack_status, levels, sensors_ok and sensors_online are NEVER
  // written. They stay NULL from the row's creation, which is what
  // devices.kind = 'scale' tells the dashboard to expect -- and what keeps a
  // scale out of slot_overview's bowl arithmetic.

  String body;
  serializeJson(doc, body);

  const String query = String("device_id=eq.") + deviceId_;

  // count=exact is what makes an unregistered device loud. A PATCH matching no
  // rows is a perfectly successful 204 -- without the count we could not tell
  // "reported" from "wrote nothing at all, forever".
  lastResult_ = uplink::request("PATCH", "/rest/v1/device_status", query.c_str(),
                                "return=minimal,count=exact", body);

  if (!lastResult_.ok()) {
    // 23503 is a foreign-key violation: this device_id is not in `devices`.
    // Latched, because only a human running register_loadcells.sql fixes it.
    if (strcmp(lastResult_.pgCode, "23503") == 0) {
      unprovisioned_ = true;
      everUnprovisioned_ = true;
    }
    return false;
  }

  if (lastResult_.matchedZeroRows) {
    Serial.printf("scale-uplink: no device_status row for '%s' -- run "
                  "supabase/register_loadcells.sql\n",
                  deviceId_);
    unprovisioned_ = true;
    everUnprovisioned_ = true;
    return false;
  }

  // A post that actually wrote a row is the only thing that can clear the
  // latch -- somebody has registered the device since.
  everUnprovisioned_ = false;
  return true;
}

}  // namespace

void begin() {
  deviceId_ = BOWLSTACK_DEVICE_ID;
  bootId_ = uplink::newBootId();

  // The real MAC from the eFuse. Needs no WiFi stack, and it is what identifies
  // WHICH BOARD is currently in this installation slot -- device_id names the
  // slot and survives a board swap, so the two together are what a technician
  // needs to reconcile a unit with its history.
  {
    uint8_t m[6];
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(mac_, sizeof(mac_), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2],
             m[3], m[4], m[5]);
  }

  // The radio this image joins with. bringup_wifi owns it exclusively here,
  // where net.cpp owns it on the discrete board -- uplink.cpp names neither.
  uplink::begin(&bringup_wifi::connected);

  Serial.printf("scale-uplink: %s boot_id=%u -> %s\n", deviceId_, bootId_,
                uplink::apiBase());
}

void loop(const scale::Snapshot &s, uint32_t uptimeSec, uint16_t batteryMv,
          battery::Level batteryLevel) {
  const uint32_t now = millis();

  // --- OBSERVING COMES FIRST, AND IS NOT GATED ON THE LINK -----------------
  // Everything down to the end of the history block runs whether or not there
  // is a network, because the queue exists precisely to hold what happened
  // while there was not one.
  //
  // This section used to sit BELOW `if (!uplink::connected()) return;`, which
  // quietly made the 32-sample buffer unreachable: the only way to fill it was
  // to be ONLINE with posts failing. A WiFi outage -- the one case the buffer
  // is described as covering, and the reason it is 32 entries rather than 4 --
  // recorded nothing at all, so the reconnect replayed nothing and the
  // consumption curve had a clean hole through the middle of it. Worse, the
  // hole is invisible: seq stays contiguous across it, because a sample that
  // was never observed never took a seq. The code read as though it buffered
  // offline; it only buffered mid-failure.
  //
  // Sampling is local and costs nothing. The network gates POSTING, below.
  const char *state = weightState(s);
  const bool haveWeight = (state == ST_OK);
  const int32_t weightG = haveWeight ? (int32_t)lroundf(s.totalGrams) : 0;

  // --- is there news? ------------------------------------------------------
  // Deliberately NOT "has anything changed". The weight changes on every
  // sample; what matters is whether it has changed in a way somebody would act
  // on. Everything else rides the heartbeat.
  bool news = !haveReported_;
  if (haveReported_) {
    // A fault appearing or clearing is always news -- it changes what the
    // dashboard may claim, not merely by how much.
    if (state != reportedState_) news = true;
    if (haveWeight != reportedHadWeight_) news = true;
    // A band change, for the same reason telemetry.cpp treats it as one: it is
    // hysteresed upstream, so it does not flap.
    if (batteryLevel != reportedBand_) news = true;
    // A serving or a refill. Measured against the last POSTED weight so a slow
    // drift accumulates into one post rather than outrunning the comparison.
    if (haveWeight && reportedHadWeight_ &&
        labs((long)weightG - (long)reportedWeightG_) >= WEIGHT_EVENT_G) {
      news = true;
    }
  }
  if (news) statusPending_ = true;

  // --- the history ---------------------------------------------------------
  // Queued on the SAME news, plus a slow tick of its own. Enqueueing is local
  // and costs nothing, so it happens before any of the rate limiting below --
  // a sample must be recorded when it HAPPENS, not when the network next
  // allows a post, or the timestamps describe the uplink rather than the food.
  // Judged against the last RECORDED sample, never against the last posted
  // one -- see lastEnqWeightG_ for the nine-rows-for-one-bowl this fixes.
  const bool stateChanged = everEnqueued_ && (state != lastEnqState_ ||
                                              haveWeight != lastEnqHadWeight_);
  const bool moved = everEnqueued_ && haveWeight && lastEnqHadWeight_ &&
                     labs((long)weightG - (long)lastEnqWeightG_) >= WEIGHT_EVENT_G;
  const bool gapOk = !everEnqueued_ ||
                     (uint32_t)(now - lastEnqMs_) >= MIN_SAMPLE_GAP_MS;
  const bool periodic = everEnqueued_ && (int32_t)(now - nextSampleMs_) >= 0;

  // A state change is never throttled; a movement is. The first is a fact
  // about the hardware and the second is a bowl still settling.
  if (!everEnqueued_ || stateChanged || (moved && gapOk) || periodic) {
    nextSampleMs_ = now + SAMPLE_PERIOD_MS;
    lastEnqMs_ = now;
    lastEnqWeightG_ = weightG;
    lastEnqHadWeight_ = haveWeight;
    lastEnqState_ = state;
    const char *reason = !everEnqueued_ ? "boot"
                       : (stateChanged || moved) ? "change" : "periodic";
    everEnqueued_ = true;
    enqueue(s, reason, state, weightG, haveWeight, batteryMv, batteryLevel);
  }

  // --- from here down, nothing happens without a link ----------------------
  if (!uplink::connected()) return;

  if (backoffActive_) {
    if ((int32_t)(now - backoffUntilMs_) < 0) return;
    backoffActive_ = false;
  }

  if (unprovisioned_) {
    // Latched: only a human inserting a `devices` row fixes this. Back off hard
    // rather than hammering the endpoint for the life of the device, but allow
    // one probe per interval so a unit registered later recovers on its own.
    backoffUntilMs_ = now + UNPROVISIONED_RETRY_MS;
    backoffActive_ = true;
    unprovisioned_ = false;
    Serial.printf("scale-uplink: %s not registered in `devices` -- backing off "
                  "%lu s\n",
                  deviceId_, (unsigned long)(UNPROVISIONED_RETRY_MS / 1000));
    return;
  }

  const bool due = (int32_t)(now - nextStatusMs_) >= 0;
  if (qCount_ == 0 && !statusPending_ && !due) return;

  // The per-device floor. Unsigned subtraction, so it is correct across the
  // millis() wrap at ~49.7 days; everPosted_ keeps the very first report
  // immediate instead of making a freshly booted device wait out a window it
  // has no history for.
  if (everPosted_ && (uint32_t)(now - lastPostMs_) < POST_MIN_INTERVAL_MS) return;

  // HISTORY FIRST, oldest-first, so a reconnect replays what happened while
  // offline before the current-state row is overwritten. Everything queued
  // since the last round goes in ONE batch -- the writes are coalesced, the
  // samples are not merged or dropped, so each keeps its own recorded_at and
  // the consumption curve stays intact.
  //
  // A failure here does NOT stop the state PATCH below. The two answer
  // different questions and only one of them is urgent: if the history table
  // is missing or the append fails, the dashboard must still be able to say
  // how much food is on the counter right now.
  if (qCount_ > 0) flushSamples();

  if (patchStatus(s, uptimeSec, batteryMv, batteryLevel, state, weightG, haveWeight)) {
    lastPostMs_ = now;
    everPosted_ = true;
    posts_++;
    statusPending_ = false;
    nextStatusMs_ = now + STATUS_PERIOD_MS;
    lastOk_ = true;

    haveReported_ = true;
    reportedState_ = state;
    reportedHadWeight_ = haveWeight;
    reportedWeightG_ = weightG;
    reportedBand_ = batteryLevel;
  } else {
    // lastPostMs_ is advanced on failure too, so a failing endpoint cannot be
    // retried faster than the floor allows.
    lastPostMs_ = now;
    everPosted_ = true;
    backoffUntilMs_ = now + RETRY_PERIOD_MS;
    backoffActive_ = true;
    lastOk_ = false;
  }
}

uint32_t bootId() { return bootId_; }
bool lastPostOk() { return lastOk_; }
bool unprovisioned() { return everUnprovisioned_; }
uint32_t posts() { return posts_; }
uint8_t queued() { return qCount_; }
uint32_t samplesPosted() { return samplesPosted_; }
uint32_t lastPostMs() { return lastPostMs_; }

}  // namespace scale_telemetry
