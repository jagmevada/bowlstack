// Front-panel status LEDs.
//
//   LED 1-4   thresholded presence of levels f1..f4 -- lit when a bowl is there
//   LED 5     device health:
//               solid       everything nominal
//               1 Hz blink  a sensor is misbehaving
//               2 Hz blink  WiFi is disconnected
//
// All LEDs are common anode, so every pin is ACTIVE LOW.
//
// This is production behaviour, unlike debug_plot: it is the only status a
// person standing at the station can read, and it must stay correct in the
// build that ships.

#pragma once

#include <Arduino.h>

#include "tasks.h"

namespace indicators {

// Configures the pins and drives every LED OFF. Call as early as possible:
// until a pin is an output it floats, and the common anode lights the LED.
void begin();

// Call regularly; it does its own blink timing. `sensorFault` and
// `wifiDown` decide the health pattern.
void update(const tasks::PlotFrame &f, bool sensorFault, bool wifiDown);

}  // namespace indicators
