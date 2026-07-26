// Supabase uplink. Production behaviour, unlike debug_plot.
//
// Two destinations, for the reason set out in supabase/schema.sql: the current
// state is upserted into one row per device (bounded storage), while history is
// appended only when something actually changed. Posting every report as
// history would be ~259k rows/day across the fleet.

#pragma once

#include <Arduino.h>

#include "config.h"
#include "device_status.h"

namespace telemetry {

// Reason a report was generated. Pinned by a CHECK constraint server-side.
enum class Reason : uint8_t { Boot, Change, Periodic };

// Seeds boot_id and prepares the TLS client. Call once from setup(), after
// net::begin().
void begin();

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
