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

  Serial.println("failed");
  WiFi.disconnect();
  return false;
}

}  // namespace

void begin() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  if (tryJoin(WIFI_SSID_1, WIFI_PASS_1)) return;
  if (tryJoin(WIFI_SSID_2, WIFI_PASS_2)) return;

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
