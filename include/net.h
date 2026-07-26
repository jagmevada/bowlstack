// WiFi bring-up and connection upkeep.
//
// Deliberately non-blocking after the initial attempt: the bowl count must stay
// correct and the sensors must keep polling with no network at all. Nothing in
// the measurement path may ever wait on this module.

#pragma once

#include <Arduino.h>

namespace net {

// Tries the two credential pairs from secret.h in order. If neither
// associates, opens the WiFiManager captive portal and returns immediately --
// the portal is then pumped from loop(). Blocks only for the join attempts.
void begin();

// Call every loop. Pumps the captive portal when open and reconnects in the
// background when the link drops. Never blocks for long: the bowl count must
// stay live with no network at all.
void loop();

bool connected();

// Empty until associated. For logging and diagnostics.
const char *ssid();
int8_t rssi();

}  // namespace net
