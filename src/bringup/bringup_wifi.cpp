// The platform half of the WiFi page: the part that knows what a radio is.
//
// ui_wifi.cpp draws and collects; this file scans, joins and reports. The seam
// exists so the page compiles and runs in the desktop preview, which has no
// radio at all -- see ui_state.h for the same argument about DeviceStatus.
//
// This is DELIBERATELY NOT WiFiManager. The discrete build uses it for its
// captive portal, and the portal remains the primary commissioning path (the QR
// on the WiFi page points at it). But WiFiManager owns the join, the retry
// policy and a blocking save path measured at 62 s on the discrete board -- all
// of which would fight a UI that wants to scan on demand and report progress.
// Raw WiFi.h here; WiFiManager comes back when the full firmware does, and the
// two meet at net.cpp rather than inside a screen.

#include <Arduino.h>
#include <WiFi.h>

#include "ui_demo.h"
#include "ui_wifi.h"

namespace bringup_wifi {
namespace {

// Non-blocking by construction. WiFi.scanNetworks(true) returns immediately and
// the result is collected later, because a blocking scan takes 2-10 s -- the
// discrete build measured exactly that -- and a UI frozen for ten seconds is
// indistinguishable from a crashed one.
bool scanRunning_ = false;
uint32_t nextScanMs_ = 0;
uint32_t lastPublishMs_ = 0;

char joinSsid_[33] = "";
char joinPass_[65] = "";
bool joinPending_ = false;
uint32_t joinStartedMs_ = 0;

// Rescan cadence once idle. A scan costs radio time and, while associated, a
// brief drop in throughput; 20 s is often enough for a list a person is
// choosing from and rare enough not to disturb a working link.
const uint32_t RESCAN_MS = 20000;
const uint32_t JOIN_TIMEOUT_MS = 12000;

void publishScan() {
  const int16_t n = WiFi.scanComplete();
  if (n < 0) return;  // still running, or nothing to collect

  ui::Network list[ui::WIFI_MAX_NETWORKS];
  uint8_t count = 0;
  for (int16_t i = 0; i < n && count < ui::WIFI_MAX_NETWORKS; i++) {
    // Skip hidden SSIDs: an empty row is not something a person can choose,
    // and it would occupy one of the ten slots the list has room for.
    const String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;

    snprintf(list[count].ssid, sizeof(list[count].ssid), "%s", ssid.c_str());
    list[count].rssi = (int16_t)WiFi.RSSI(i);
    list[count].secured = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    count++;
  }
  ui::wifiSetNetworks(list, count);
  WiFi.scanDelete();
  scanRunning_ = false;
  nextScanMs_ = millis() + RESCAN_MS;

  Serial.printf("wifi: scan found %d, published %u\n", n, count);
}

void publishStatus() {
  const bool up = (WiFi.status() == WL_CONNECTED);
  if (up) {
    ui::wifiSetConnected(WiFi.SSID().c_str(), (int16_t)WiFi.RSSI(),
                         WiFi.localIP().toString().c_str());
  } else {
    ui::wifiSetConnected(nullptr, 0, nullptr);
  }
  // The STATUS BAR reads this, not the WiFi page. Without it the bars showed
  // whatever the fixture had baked in -- four solid bars on a device with no
  // link at all.
  ui::demoOverrideWifi(up, up ? (int16_t)WiFi.RSSI() : 0);
}

void onJoin(const char *ssid, const char *pass) {
  snprintf(joinSsid_, sizeof(joinSsid_), "%s", ssid ? ssid : "");
  snprintf(joinPass_, sizeof(joinPass_), "%s", pass ? pass : "");
  joinPending_ = true;
  // The passphrase is NOT logged. It is the one thing on this device worth
  // protecting, and a serial console is not private.
  Serial.printf("wifi: join requested for '%s' (%u char key)\n", joinSsid_,
                (unsigned)strlen(joinPass_));
}

}  // namespace

// A one-shot join at boot, for verifying the join PATH without a finger on the
// glass. It calls the same onJoin() the OK button calls, so what it exercises is
// what a person exercises -- WiFi.begin, the bounded wait, and the status
// publish -- rather than a parallel code path that might work when the real one
// does not.
//
// Off unless -DBRINGUP_WIFI_TEST_JOIN=1, and it reads include/secret.h, which
// is gitignored. Never enable it in anything shipped: it would join a network
// named in the firmware image regardless of what the device was commissioned
// for.
#if BRINGUP_WIFI_TEST_JOIN
#include "secret.h"
#endif

void begin() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  ui::wifiOnJoin(onJoin);
  nextScanMs_ = millis() + 500;
  Serial.println("wifi: station mode, scanning shortly");

#if BRINGUP_WIFI_TEST_JOIN
  if (strlen(WIFI_SSID_1) > 0) {
    Serial.println("wifi: TEST JOIN enabled (secret.h) - not for shipping");
    onJoin(WIFI_SSID_1, WIFI_PASS_1);
  }
#endif
}

void loop(uint32_t nowMs) {
  if (joinPending_) {
    joinPending_ = false;
    // A scan in flight would otherwise be abandoned mid-flight by the join and
    // leave scanComplete() reporting a failure that looks like a radio fault.
    if (scanRunning_) {
      WiFi.scanDelete();
      scanRunning_ = false;
    }
    WiFi.begin(joinSsid_, joinPass_);
    joinStartedMs_ = nowMs;
    Serial.printf("wifi: connecting to '%s'...\n", joinSsid_);
  }

  if (joinStartedMs_) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("wifi: connected, ip %s, rssi %d\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      joinStartedMs_ = 0;
    } else if (nowMs - joinStartedMs_ > JOIN_TIMEOUT_MS) {
      // Bounded, and the bound is the point. The discrete build burned
      // JOIN_TIMEOUT_MS per network that was not there; an unbounded wait here
      // would leave the page showing "not connected" with no way to tell a
      // wrong passphrase from a slow one.
      Serial.printf("wifi: join timed out after %lu ms (status %d)\n",
                    (unsigned long)(nowMs - joinStartedMs_), (int)WiFi.status());
      joinStartedMs_ = 0;
      WiFi.disconnect();
    }
  }

  if (!scanRunning_ && !joinStartedMs_ && (int32_t)(nowMs - nextScanMs_) >= 0) {
    // async = true, show_hidden = false. Async is what keeps the UI responsive
    // through the 2-10 s a scan takes.
    WiFi.scanNetworks(true, false);
    scanRunning_ = true;
  }
  if (scanRunning_) publishScan();

  // Status carries RSSI, which drifts constantly; republishing it at frame rate
  // would defeat the change detection in the page above.
  if ((int32_t)(nowMs - lastPublishMs_) >= 1000) {
    lastPublishMs_ = nowMs;
    publishStatus();
  }
}

}  // namespace bringup_wifi
