// WiFi detail page, reached by tapping the signal bars in the status bar.
//
// THE PRIMARY PATH IS NOT TYPING. A WPA passphrase is deliberately not a word,
// so every on-screen keyboard is close to its worst case: on a 240 px panel a
// QWERTY row gives ~24 px keys, and a T9 costs one to four taps per character
// plus mode switches for case and symbols. Something like `Xk7$mQ9!` is
// genuinely painful either way.
//
// So the page leads with a QR code of the device's OWN setup access point, in
// the standard WIFI:T:WPA;S:ssid;P:pass;; form that iOS and Android camera apps
// join natively. The person scans it, their phone joins the device, the captive
// portal opens, and they type on a full-size keyboard they already know. The
// firmware already runs that portal -- docs/firmware.md section 4 -- and
// WiFiManager already puts the radio in AP_STA so commissioning and recovery
// happen in parallel. This screen makes the existing path discoverable rather
// than replacing it.
//
// On-device entry stays as a FALLBACK, for when no phone is to hand. It uses
// lv_keyboard with popovers -- the magnified bubble above the pressed key, the
// same trick phones use to make small keys workable -- because for a random
// passphrase, seeing what you actually hit beats any amount of layout
// cleverness.

#pragma once

#include <lvgl.h>
#include <stdint.h>

namespace ui {

struct Network {
  char ssid[33];
  int16_t rssi;
  bool secured;
};

static const uint8_t WIFI_MAX_NETWORKS = 10;

// Supplied by the platform. The UI never reads hardware itself -- see
// ui_state.h for why that separation exists.
void wifiSetMac(const char *mac);
void wifiSetSetupAp(const char *ssid, const char *pass);
void wifiSetNetworks(const Network *list, uint8_t count);
void wifiSetConnected(const char *ssid, int16_t rssi, const char *ip);

void buildWifiPage(lv_obj_t *parent);

// Registered by the owner so the back button can close the detail layer.
void wifiOnClose(void (*cb)(void));

// Called when Join is pressed. ui_wifi collects the passphrase and hands it
// over; it does not know what a radio is, which is what lets this whole page
// compile and run in the desktop preview.
void wifiOnJoin(void (*cb)(const char *ssid, const char *pass));

// Rebuilds whatever the setters have invalidated. Call every loop iteration --
// it early-outs unless something actually changed.
//
// It exists because buildWifiPage() runs ONCE, from buildPages(), and on the
// device that happens before the first scan has returned: the list was built
// from an empty array and never rebuilt, so the page showed no networks and
// "not connected" forever no matter what the radio did. The simulator hid it,
// because there the fixtures are installed before the page is built.
void wifiTick();

// Returns to the network list and clears the passphrase field. Called when the
// page is dismissed, so a walk-away does not leave a cleartext key on screen
// for whoever opens it next.
void wifiResetView();

}  // namespace ui
