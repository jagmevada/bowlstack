#include "config.h"

namespace config {

// Ordered bottom of the stack upward: f1 watches the lowest bowl, f4 the
// highest. XSHUT must be an output-capable GPIO -- 34-39 are input-only on the
// ESP32 and cannot pull the line low. f3 sits on 23 to keep its harness short.
#if BOWLSTACK_BOARD_WAVESHARE_S3
// Waveshare ESP32-S3-Touch-LCD-2. All four on Wire1, because Wire is the
// panel's touch controller and the IMU -- see config.h for why one bus is
// forced here.
//
// XSHUT pins are chosen for harness length against the two headers: f1/f2 sit
// beside the sensor bus on P1, f3/f4 on P2 where the 3V3 and GND the sensors
// need also are. Every one is output-capable; the ESP32's input-only 34-39
// problem does not exist on the S3.
const SensorConfig SENSORS[SENSOR_COUNT] = {
    {"f1", &Wire1, 2, 0x30},
    {"f2", &Wire1, 4, 0x31},
    {"f3", &Wire1, 13, 0x32},
    {"f4", &Wire1, 12, 0x33},
};
#else
const SensorConfig SENSORS[SENSOR_COUNT] = {
    {"f1", &Wire, 32, 0x30},
    {"f2", &Wire, 33, 0x31},
    {"f3", &Wire1, 23, 0x32},
    {"f4", &Wire1, 26, 0x33},
};
#endif

const float OFFSET_MM[SENSOR_COUNT] = {0.0f, 0.0f, 0.0f, 0.0f};

}  // namespace config
