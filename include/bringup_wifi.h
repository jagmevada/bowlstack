// Platform half of the WiFi page. See src/bringup/bringup_wifi.cpp.
//
// It owns the radio for this image, exclusively: the scan, the join, the
// credential store and the reconnect. Nothing else may call WiFi.begin() or
// WiFi.scanNetworks() -- see the note at the top of the .cpp for what happened
// when net.cpp was considered as a second owner.
#pragma once

#include <stdint.h>

namespace bringup_wifi {

// Loads the commissioned credentials, brings the station up and arms a ranked
// auto-join for the first completed scan. Does NOT block on an association:
// this runs inside setup(), where every millisecond is splash time.
void begin();

// Call every loop iteration. Pumps the scan, the bounded join, the link-state
// transitions and the 1 Hz publish into the UI. Never blocks.
void loop(uint32_t nowMs);

// --- what the rest of the firmware asks ------------------------------------
// The same three questions net.h exposes for the discrete product, so a
// telemetry layer written against one reads the same against the other.

bool connected();

// Empty until associated. For logging and diagnostics.
const char *ssid();

// 0 when not associated -- distinct from a genuinely weak -90, and the same
// convention ui_state.h's wifiRssi uses.
int8_t rssi();

}  // namespace bringup_wifi
