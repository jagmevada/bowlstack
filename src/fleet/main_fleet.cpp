// Entry point for the fleet-simulator image.
//
// Runs on a BARE ESP32 -- no sensors, no LEDs, no battery divider, nothing wired.
// Flash it, give it WiFi, and it presents itself to Supabase as up to 31
// installations so the front-end has a fully populated database to build
// against. See include/fleet_sim.h for what is faked and what is shared with the
// production firmware.
//
// Deliberately single-threaded, unlike src/main.cpp. The FreeRTOS split exists
// because measurement must never stall behind the network; here there is no
// measurement to protect, and one loop makes the request ordering obvious.

#include <Arduino.h>

#include "fleet_sim.h"
#include "net.h"
#include "telemetry.h"
#include "version.h"

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.printf("\n\nBowlstack FLEET SIMULATOR  fw=%s\n", BOWLSTACK_FW_VERSION);
  Serial.println("Front-end test bed. Sensor data is DUMMY; WiFi and the "
                 "Supabase uplink are the production code paths.");

  net::begin();
  telemetry::begin();
  fleet_sim::begin();
}

void loop() {
  net::loop();
  fleet_sim::loop();

  // The simulator's own pacing. net::loop() and the per-channel limiters do the
  // real throttling; this just stops a tight spin when there is nothing to send.
  delay(50);
}
