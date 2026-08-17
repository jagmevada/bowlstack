// Platform half of the WiFi page. See src/bringup/bringup_wifi.cpp.
#pragma once

#include <stdint.h>

namespace bringup_wifi {
void begin();
void loop(uint32_t nowMs);
}  // namespace bringup_wifi
