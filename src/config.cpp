#include "config.h"

namespace config {

// Ordered bottom of the stack upward: f1 watches the lowest bowl, f4 the
// highest. XSHUT must be an output-capable GPIO -- 34-39 are input-only on the
// ESP32 and cannot pull the line low. f3 sits on 23 to keep its harness short.
const SensorConfig SENSORS[SENSOR_COUNT] = {
    {"f1", &Wire, 32, 0x30},
    {"f2", &Wire, 33, 0x31},
    {"f3", &Wire1, 23, 0x32},
    {"f4", &Wire1, 26, 0x33},
};

const float OFFSET_MM[SENSOR_COUNT] = {0.0f, 0.0f, 0.0f, 0.0f};

}  // namespace config
