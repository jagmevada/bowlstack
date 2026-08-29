// The Supabase wire, with no opinion about what is being reported.
//
// WHY THIS FILE EXISTS. telemetry.h already states the rule: "The point of the
// split is that there is ONE implementation of the Supabase wire interface.
// Giving the simulator its own copy would let the two drift, and the copy that
// matters least is the one that would stay correct." The fleet simulator was
// the first case that pressed on it and was solved inside telemetry.cpp, by
// splitting per-device state (Channel) from per-process state.
//
// The load-cell station is the second case and it cannot be solved there,
// because it cannot COMPILE there. telemetry.h reaches bowl_logic.h and
// device_status.h, device_status.h reaches sensor_array.h, and sensor_array.h
// reaches <VL53L0X.h> -- a library the panel image deliberately does not link.
// So the choice was a second copy of the transport or a seam, and a second copy
// is the thing that rule exists to prevent.
//
// WHAT IS HERE is everything that is true of any report to this project's
// Supabase: the URL normalisation, one reused TLS session, the SQLSTATE scrape
// that separates a duplicate key from an unregistered device, and the
// Content-Range trick that tells "wrote nothing, forever" from "reported".
//
// WHAT IS NOT HERE is the payload, the queue, the change detection and the
// heartbeat schedule. Those differ per product -- a bowl counter has an
// append-only history and a scale currently has none, because status_events is
// NOT NULL on six stack-shaped columns and a load cell could only insert there
// by fabricating a bowl count.
//
// Nothing in this header may name a specific radio. The discrete board joins
// through net.cpp and the panel through bringup_wifi.cpp; uplink.cpp is
// compiled into both, so the link check arrives as a function pointer.

#pragma once

#include <Arduino.h>

namespace uplink {

// Supplied by the image: net::connected() on the discrete board,
// bringup_wifi::connected() on the panel.
typedef bool (*LinkUp)();

// Prepares the TLS session and records how to ask whether the link is up.
// Call once, after the radio has been started. Safe to call twice -- the second
// call replaces the link check and leaves the session alone, which is what a
// second product in one image would want.
void begin(LinkUp linkUp);

// False when no link check has been installed, so a caller that forgot
// begin() backs off rather than hammering a dead socket.
bool connected();

// The REST origin, normalised. Supabase presents several URLs in its dashboard
// and only one of them is the API origin, so a unit configured with the REST
// endpoint (".../rest/v1/") or a trailing slash still works instead of quietly
// building ".../rest/v1//rest/v1/device_status" and failing every request.
//
// Computed once and cached. Print it at boot rather than the raw macro: if
// normalisation changed anything, that difference is the first thing worth
// seeing when requests fail.
const char *apiBase();

struct Result {
  // HTTP status, or a negative HTTPClient error code. -1000 means the link was
  // down and nothing was attempted; -1001 means the client refused the URL.
  int code;

  // SQLSTATE scraped from the error body, or "" if there was none.
  //
  // IT IS THE ONLY THING THAT SEPARATES TWO OPPOSITE ANSWERS. PostgREST reports
  // both a duplicate key (23505) and a foreign-key violation (23503) as HTTP
  // 409 -- and the first means "these rows are already stored, drop the
  // buffer", while the second means "this device is not registered, KEEP the
  // buffer". Keying off the status alone loses real history to a provisioning
  // mistake that is about to be fixed.
  char pgCode[8];

  // The request matched no rows. A PATCH that updates nothing is a perfectly
  // successful 204, so without this an unregistered device looks identical to a
  // healthy one -- forever. Only meaningful when the caller asked for
  // count=exact.
  bool matchedZeroRows;

  bool ok() const { return code >= 200 && code < 300; }
};

// One HTTP transaction. Never retries and never blocks longer than the
// timeout: the caller owns the backoff, because how long to wait after a
// failure is a policy about a product rather than about a socket.
Result request(const char *method, const char *path, const char *query,
               const char *prefer, const String &body);

// A fresh boot_id. PER CHANNEL, not per process -- boot_id is half of the
// (device_id, boot_id, seq) idempotency key, so sharing one across the fleet
// simulator's 31 nodes would make them collide the moment two reached the same
// seq. Offered here only so there is one definition of "how a boot_id is made".
uint32_t newBootId();

}  // namespace uplink
