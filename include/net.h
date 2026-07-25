// WiFi bring-up and connection upkeep.
//
// Deliberately non-blocking after the initial attempt: the bowl count must stay
// correct and the sensors must keep polling with no network at all. Nothing in
// the measurement path may ever wait on this module.

#pragma once

#include <Arduino.h>

namespace net {

// Tries the two credential pairs from secret.h in order, then falls back to the
// WiFiManager captive portal. Blocking, and only called once from setup() --
// there are no sensor readings to lose yet at that point.
void begin();

// Call every loop. Reconnects in the background when the link drops; never
// blocks. Returns immediately when already connected.
void loop();

bool connected();

// Empty until associated. For logging and diagnostics.
const char *ssid();
int8_t rssi();

}  // namespace net
