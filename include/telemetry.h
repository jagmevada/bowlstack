// Supabase uplink. Production behaviour, unlike debug_plot.
//
// Two destinations, for the reason set out in supabase/schema.sql: the current
// state is upserted into one row per device (bounded storage), while history is
// appended only when something actually changed. Posting every report as
// history would be ~259k rows/day across the fleet.

#pragma once

#include <Arduino.h>

#include "battery_soc.h"
#include "bowl_logic.h"
#include "config.h"
#include "device_status.h"

namespace telemetry {

// Reason a report was generated. Pinned by a CHECK constraint server-side.
enum class Reason : uint8_t { Boot, Change, Periodic };

// ---------------------------------------------------------------------------
// Channel -- everything the uplink tracks PER DEVICE.
//
// A real unit owns exactly one of these, and the free functions below operate on
// a default instance, so nothing in the production firmware needs to know this
// type exists. It is split out because the fleet simulator drives 31 of them
// from a single bare ESP32: the state below is per-installation (its own
// boot_id, its own seq, its own buffered history, its own backoff), while the
// TLS session, the URL builder, the payload writer and the SQLSTATE handling are
// per-process and stay shared.
//
// The point of the split is that there is ONE implementation of the Supabase
// wire interface. Giving the simulator its own copy would let the two drift, and
// the copy that matters least is the one that would stay correct -- the
// simulator exists precisely to prove what the real fleet will do.
// ---------------------------------------------------------------------------
struct QueuedEvent {
  uint32_t atMs;  // millis() when queued; converted to age at send time
  uint32_t seq;
  Reason reason;
  uint8_t stackCount;
  StackStatus stackStatus;
  LevelState levels[config::SENSOR_COUNT];
  bool sensorOk[config::SENSOR_COUNT];
  uint8_t sensorsOnline;
  battery::Level batteryLevel;
  bool charging;
};

struct Channel {
  // Identity. Points at storage that must outlive the channel -- a string
  // literal for the real device, the simulator's own table for a virtual one.
  const char *deviceId = nullptr;

  uint32_t bootId = 0;

  QueuedEvent queue[config::TELEMETRY_QUEUE_LEN];
  uint8_t head = 0;  // oldest
  uint8_t count = 0;

  bool statusPending = true;  // always report once at boot
  bool lastOk = false;
  bool unprovisioned = false;

  // Armed only while a backoff is actually in force. Not a bare timestamp
  // compared as (int32_t)(now - 0): that inverts once millis() passes 2^31
  // (~24.9 days), which would silently halt telemetry on a device that had
  // never failed a post.
  bool backoffActive = false;
  uint32_t backoffUntilMs = 0;

  uint32_t nextStatusMs = 0;

  // Enforces POST_MIN_INTERVAL_MS. everPosted rather than a sentinel timestamp,
  // so a device booting after the millis() wrap does not sit out a window.
  bool everPosted = false;
  uint32_t lastPostMs = 0;
};

// Seeds boot_id and prepares the TLS client. Call once from setup(), after
// net::begin().
void begin();

// --- per-channel API, used directly only by the fleet simulator ------------

// Assigns an identity and a fresh boot_id. `deviceId` is stored by pointer.
void openChannel(Channel &ch, const char *deviceId);

void enqueue(Channel &ch, const DeviceStatus &s, Reason reason, uint32_t seq);
void loop(Channel &ch, const DeviceStatus &current);
void requestImmediateUpsert(Channel &ch);

// Drains the queued history of SEVERAL channels into one POST.
//
// Legitimate because status_events carries device_id per row and the endpoint
// already takes an array -- this is the same batch INSERT the real firmware
// makes, with rows from more than one installation in it. It is what lets one
// ESP32 stand in for 31 without turning 31 buffers into 31 requests.
//
// Returns false if the batch was not accepted; each channel keeps its own
// entries in that case, so nothing is lost by a shared failure.
bool flushMany(Channel **channels, uint8_t n);

// Queues a status snapshot for the history table. Safe to call with no network:
// entries are held in RAM and flushed on reconnect. The ring buffer is
// intentionally small -- if it overflows, the oldest entries are dropped and
// the gap is visible server-side as a jump in `seq`.
//
// `seq` is supplied by the caller rather than allocated here. It must be
// allocated at the moment a change is OBSERVED, not when it reaches this
// buffer: anything dropped between those two points would otherwise consume no
// seq, leaving the server a contiguous sequence and no evidence that data was
// lost -- which is the one thing seq exists to prove.
// --- default-channel API ---------------------------------------------------
// What the production firmware uses. Each of these forwards to the one channel
// belonging to BOWLSTACK_DEVICE_ID, so callers never see the type above.

void enqueue(const DeviceStatus &s, Reason reason, uint32_t seq);

// Call every loop. Flushes queued history oldest-first, then upserts current
// state when due. Never blocks the sensor loop for long: one HTTP transaction
// per call at most.
void loop(const DeviceStatus &current);

// Marks the current state as needing an immediate upsert, rather than waiting
// out the heartbeat period.
void requestImmediateUpsert();

// --- diagnostics -----------------------------------------------------------
uint8_t queued();
uint32_t bootId();
bool lastPostOk();

// True once the server has rejected this device's ID with a foreign-key
// violation, meaning it was never registered in the `devices` table. Latched:
// retrying cannot fix a provisioning error, so the uplink backs off hard.
bool unprovisioned();

}  // namespace telemetry
