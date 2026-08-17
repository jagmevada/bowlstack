// Sensor half of the bring-up harness. See src/bringup/bringup_sensors.cpp.
#pragma once

#include <stdint.h>

class SensorArray;

namespace bringup_sensors {
void begin();
void loop(uint32_t nowMs);
const SensorArray &array();
}  // namespace bringup_sensors
