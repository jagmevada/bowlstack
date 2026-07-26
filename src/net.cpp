#include "net.h"

#include <WiFi.h>
#include <WiFiManager.h>

#include "secret.h"
#include "version.h"

namespace net {
namespace {

// Per-credential association timeout during the boot attempt.
const uint32_t JOIN_TIMEOUT_MS = 12000;

// How long the captive portal stays open before the device gives up and boots
// anyway. Bowl counting works offline, so hanging here forever would trade a
// working sensor for a network that may never arrive.
const uint32_t PORTAL_TIMEOUT_S = 180;

// Backoff between background reconnection attempts once running.
const uint32_t RETRY_PERIOD_MS = 30000;

char joinedSsid[33] = {0};

// WL_* codes distinguish the two failures that look identical from outside:
// a wrong password versus a network that is not there at all.
const char *statusText(wl_status_t s) {
  switch (s) {
    case WL_NO_SSID_AVAIL:  return "SSID not found (out of range, hidden, or 5 GHz-only)";
    case WL_CONNECT_FAILED: return "association rejected (usually a wrong password)";
    case WL_CONNECTION_LOST:return "connection lost";
    case WL_DISCONNECTED:   return "disconnected / still trying";
    case WL_IDLE_STATUS:    return "idle";
    default:                return "no result before timeout";
  }
}

bool tryJoin(const char *ssid, const char *pass) {
  if (ssid == nullptr || ssid[0] == '\0') return false;

  Serial.printf("wifi: trying '%s' ... ", ssid);
  WiFi.begin(ssid, pass);

  const uint32_t deadline = millis() + JOIN_TIMEOUT_MS;
  while (WiFi.status() != WL_CONNECTED && (int32_t)(millis() - deadline) < 0) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    snprintf(joinedSsid, sizeof(joinedSsid), "%s", ssid);
    Serial.printf("ok, %s (%d dBm)\n", WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
    return true;
  }

  const wl_status_t st = WiFi.status();
  Serial.printf("failed (%d: %s)\n", (int)st, statusText(st));
  WiFi.disconnect();
  return false;
}

// Lists what the radio can actually see. The single most common cause of a
// silent join failure on ESP32 is a 2.4 GHz-only radio being pointed at a
// 5 GHz network -- which is invisible here rather than merely unreachable, so
// "not in this list" and "not in range" look the same and both need ruling
// out before suspecting the password.
void scanNetworks(const char *want1, const char *want2) {
  Serial.println("wifi: scanning (ESP32 is 2.4 GHz only; 5 GHz APs cannot appear)");
  const int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("wifi:   no networks visible at all - check the antenna");
    return;
  }

  bool saw1 = false, saw2 = false;
  for (int i = 0; i < n && i < 20; i++) {
    const String s = WiFi.SSID(i);
    Serial.printf("wifi:   %-24s ch%-3d %4d dBm %s\n", s.c_str(),
                  WiFi.channel(i), WiFi.RSSI(i),
                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "");
    if (want1 && s == want1) saw1 = true;
    if (want2 && s == want2) saw2 = true;
  }

  if (want1 && want1[0]) {
    Serial.printf("wifi: '%s' %s\n", want1,
                  saw1 ? "IS visible -> the password is the likely problem"
                       : "NOT visible -> wrong name, out of range, or 5 GHz");
  }
  if (want2 && want2[0]) {
    Serial.printf("wifi: '%s' %s\n", want2,
                  saw2 ? "IS visible -> the password is the likely problem"
                       : "NOT visible -> wrong name, out of range, or 5 GHz");
  }
  WiFi.scanDelete();
}

}  // namespace

void begin() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  if (tryJoin(WIFI_SSID_1, WIFI_PASS_1)) return;
  if (tryJoin(WIFI_SSID_2, WIFI_PASS_2)) return;

  // Say WHY before falling back, so a bad password is distinguishable from a
  // network that was never visible.
  scanNetworks(WIFI_SSID_1, WIFI_SSID_2);

  // Neither known network is reachable. Open the portal so the unit can be
  // commissioned on site without a rebuild -- the reason one binary can serve
  // the whole fleet.
  Serial.printf("wifi: opening config portal '%s' for %us\n",
                BOWLSTACK_DEVICE_ID, PORTAL_TIMEOUT_S);

  WiFiManager wm;
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  wm.setConnectTimeout(20);

  if (wm.autoConnect(BOWLSTACK_DEVICE_ID)) {
    snprintf(joinedSsid, sizeof(joinedSsid), "%s", WiFi.SSID().c_str());
    Serial.printf("wifi: portal joined '%s', %s\n", joinedSsid,
                  WiFi.localIP().toString().c_str());
  } else {
    // Not fatal. Sensors keep running and telemetry buffers; loop() will keep
    // retrying whatever credentials WiFiManager persisted.
    Serial.println("wifi: no connection - running offline, will retry");
  }
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) return;

  static uint32_t nextRetry = 0;
  const uint32_t now = millis();
  if ((int32_t)(now - nextRetry) < 0) return;
  nextRetry = now + RETRY_PERIOD_MS;

  // WiFi.begin() with no arguments reuses the stored credentials and returns
  // immediately; the association completes in the background, so the sensor
  // loop keeps running at full rate throughout.
  Serial.println("wifi: reconnecting");
  WiFi.reconnect();
}

bool connected() { return WiFi.status() == WL_CONNECTED; }

const char *ssid() { return joinedSsid; }

int8_t rssi() { return WiFi.status() == WL_CONNECTED ? (int8_t)WiFi.RSSI() : 0; }

}  // namespace net
