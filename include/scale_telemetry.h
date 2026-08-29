// Supabase uplink for the load-cell station -- the LDC half of what
// telemetry.h does for BWL.
//
// SAME WIRE, SAME TABLE, DIFFERENT COLUMNS. A load-cell station is registered
// in `devices` exactly like a bowl counter, gets its device_status row from the
// same trigger, is watched by the same service windows and the same
// offline_after(), and writes through the same anon column grant. What differs
// is which columns of that one row it fills: a scale writes weight_g,
// weight_state, cells_online, counts_per_gram and net_counts, and leaves every
// stack_* column NULL. `devices.kind` is what tells the dashboard which half to
// read.
//
// TWO DESTINATIONS, for the reason telemetry.h gives: current state is
// UPDATED in one row per device (bounded storage), history is APPENDED only
// when something happened.
//
//   device_status    PATCH, every 20 s. What is on the counter NOW.
//   weight_samples   POST,  on change and on a slow tick. How it got there.
//
// HISTORY IS NOT status_events, and cannot be. That table is NOT NULL on
// stack_count, stack_status, levels, sensors_ok and sensors_online -- all
// bowl-shaped -- so a scale could only append to it by fabricating a bowl
// count. docs/PHASE1_BOWL_WEIGHT.md named the separate table at Phase 4 and
// supabase/migrate_weight_samples.sql is it.
//
// WHY THE HISTORY EXISTS AT ALL: device_status can say "8.2 kg of dal left"
// and can never say "it is going twice as fast as the rice, and it runs out
// forty minutes before service ends". That second sentence is the one that
// starts a second production run while there is still time to cook, and it
// needs a series rather than a number.
//
// The transport is uplink.h -- shared with telemetry.cpp, one implementation of
// the Supabase wire for the whole project. See the note at the top of that
// header for why the load cell could not simply use telemetry.cpp.

#pragma once

#include <stdint.h>

#include "battery_soc.h"
#include "scale.h"

namespace scale_telemetry {

// Opens the channel and prepares the TLS session. Call once from setup(), after
// bringup_wifi::begin() -- the radio need not be associated yet, only started.
void begin();

// One round: at most one HTTP transaction, and only when something is due.
//
// MUST NOT BE CALLED FROM THE RENDER LOOP. A request can take up to the 8 s
// HTTP timeout, which on this board is the Arduino loop() that also drives
// LVGL, the touch read and the 20 Hz publish -- so a stalled post would freeze
// the panel for eight seconds. It runs on its own core-0 task, which is also
// where docs/firmware.md puts every piece of network work.
//
// The snapshot is passed in rather than taken here so this module never touches
// the scale mutex: the caller already holds an immutable copy, which is the
// same rule every other consumer of scale::snapshot() follows.
void loop(const scale::Snapshot &s, uint32_t uptimeSec, uint16_t batteryMv,
          battery::Level batteryLevel);

// --- diagnostics ------------------------------------------------------------
uint32_t bootId();
bool lastPostOk();

// Samples waiting to be appended to weight_samples. Non-zero while offline --
// the history buffers in RAM and drains on reconnect, so a station that loses
// WiFi mid-service does not lose the consumption curve for that meal.
uint8_t queued();

// Samples successfully appended since boot. Beside posts(), this is what
// separates "reporting fine, recording nothing" -- which is exactly what a
// missing weight_samples table looks like from the panel.
uint32_t samplesPosted();

// True once the server has rejected this device's id with a foreign-key
// violation or a zero-row PATCH, meaning it was never registered in `devices`.
// Retrying cannot fix a provisioning error, so the uplink backs off hard.
bool unprovisioned();

// How many posts have succeeded, and when the last one was. For the console
// line -- a station that is weighing correctly and reporting nothing looks
// identical, from the panel, to one that is doing both.
uint32_t posts();
uint32_t lastPostMs();

}  // namespace scale_telemetry
