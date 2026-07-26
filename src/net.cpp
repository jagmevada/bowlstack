#include "net.h"

#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include "secret.h"
#include "version.h"

namespace net {
namespace {

// Per-credential association timeout during the boot attempt. Blocking is
// acceptable here only because loop() has not started yet.
const uint32_t JOIN_TIMEOUT_MS = 12000;

// How long the captive portal stays open before giving up. Bowl counting works
// offline, so waiting forever would trade a working sensor for a network that
// may never arrive.
const uint32_t PORTAL_TIMEOUT_S = 180;

// Bounds WiFiManager's own association attempts. Without these both
// _connectTimeout and _saveTimeout stay 0, which sends it into
// WiFi.waitForConnectResult() with the Arduino default of 60 s -- inside
// wm.process(), i.e. inside our loop(), with sensors unpolled the whole time.
// That is the same failure this module was rewritten to remove, so it must be
// bounded rather than left to the library default.
const uint32_t WM_CONNECT_TIMEOUT_S = 10;

// How often a reconnection is ATTEMPTED while offline. The link state itself is
// checked every loop iteration, so a network returning is noticed immediately;
// this only paces how often WiFi.begin() is re-issued. Each attempt needs a few
// seconds to resolve, so going much shorter would cancel one still in progress.
const uint32_t RETRY_PERIOD_MS = 10000;

// How long the device stays offline before reopening the portal. Without this a
// station whose router was replaced would retry a network that no longer exists
// forever, recoverable only by reflashing. Long enough that a router reboot does
// not drop a healthy unit into AP mode.
const uint32_t PORTAL_AFTER_OFFLINE_MS = 300000;  // 5 minutes

// --- credentials -----------------------------------------------------------
// Slot 2 is whatever the portal last commissioned. It is stored in our own NVS
// namespace rather than relying on the WiFi stack's:
//
//   * WiFi.persistent(true) makes every WiFi.begin(ssid, pass) rewrite the
//     stack's stored record, so the boot-time tryJoin() calls would erase a
//     portal-commissioned network -- losing it on the first reboot after
//     commissioning, which is exactly when it must survive.
//   * WiFi.begin() with no arguments does NOT reload NVS on ESP32; it re-applies
//     whatever station config is already in RAM. So a "retry the saved network"
//     arm written that way would just repeat the previous slot.
//
// Owning the storage sidesteps both. WiFi.persistent(false) below then keeps the
// stack from writing flash on every retry.
char credSsid2[33] = {0};
char credPass2[65] = {0};

Preferences prefs;

char joinedSsid[33] = {0};

// Module scope because the portal runs non-blocking: process() is pumped from
// loop(), so the manager must outlive begin().
WiFiManager wm;
bool portalRunning = false;

// When the link was lost. A separate flag rather than a zero sentinel, because
// millis() is legitimately 0 for the first millisecond.
bool offlineTracked = false;
uint32_t offlineSinceMs = 0;

// Previous link state, so loop() logs transitions rather than steady state.
// everConnected distinguishes a first connection from a recovery.
bool wasConnected = false;
bool everConnected = false;

// Armed on the first offline tick. Not a bare `static uint32_t nextRetry = 0`:
// that compares (int32_t)(now - 0), which inverts once millis() passes 2^31
// (~24.9 days) and would silently stop all retries.
bool retryArmed = false;
uint32_t nextRetryMs = 0;

// WL_* codes distinguish the two failures that look identical from outside: a
// wrong password versus a network that is not there at all.
const char *statusText(wl_status_t s) {
  switch (s) {
    case WL_NO_SSID_AVAIL:   return "SSID not found (out of range, hidden, or 5 GHz-only)";
    case WL_CONNECT_FAILED:  return "association rejected (usually a wrong password)";
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_DISCONNECTED:    return "disconnected / still trying";
    case WL_IDLE_STATUS:     return "idle";
    default:                 return "no result before timeout";
  }
}

// Three credential slots: two compiled in, one commissioned via the portal.
// Returns false when the slot is empty.
bool credentialAt(uint8_t i, const char **ssid, const char **pass) {
  switch (i) {
    case 0:  *ssid = WIFI_SSID_1; *pass = WIFI_PASS_1; break;
    case 1:  *ssid = WIFI_SSID_2; *pass = WIFI_PASS_2; break;
    default: *ssid = credSsid2;   *pass = credPass2;   break;
  }
  return (*ssid)[0] != '\0';
}

void loadCommissioned() {
  prefs.begin("bowlstack", true);
  prefs.getString("ssid", credSsid2, sizeof(credSsid2));
  prefs.getString("pass", credPass2, sizeof(credPass2));
  prefs.end();
  if (credSsid2[0]) Serial.printf("wifi: commissioned network on file: '%s'\n", credSsid2);
}

void storeCommissioned(const char *ssid, const char *pass) {
  if (ssid == nullptr || ssid[0] == '\0') return;
  if (strcmp(ssid, credSsid2) == 0 && strcmp(pass, credPass2) == 0) return;

  snprintf(credSsid2, sizeof(credSsid2), "%s", ssid);
  snprintf(credPass2, sizeof(credPass2), "%s", pass);

  prefs.begin("bowlstack", false);
  prefs.putString("ssid", credSsid2);
  prefs.putString("pass", credPass2);
  prefs.end();
  Serial.printf("wifi: commissioned '%s' saved for future boots\n", credSsid2);
}

// Blocking join, boot only.
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
    // Seed the transition tracker so loop() does not log a duplicate CONNECTED
    // line for a link this function already reported.
    wasConnected = true;
    everConnected = true;
    return true;
  }

  const wl_status_t st = WiFi.status();
  Serial.printf("failed (%d: %s)\n", (int)st, statusText(st));
  WiFi.disconnect();
  return false;
}

// Lists what the radio can actually see. The commonest cause of a silent join
// failure on ESP32 is a 2.4 GHz-only radio pointed at a 5 GHz network -- which
// is invisible rather than merely unreachable, so "wrong password" and "wrong
// band" look identical without this.
void scanNetworks() {
  Serial.println("wifi: scanning (ESP32 is 2.4 GHz only; 5 GHz APs cannot appear)");
  const int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("wifi:   no networks visible at all - check the antenna");
    return;
  }

  for (int i = 0; i < n && i < 20; i++) {
    Serial.printf("wifi:   %-24s ch%-3d %4d dBm\n", WiFi.SSID(i).c_str(),
                  WiFi.channel(i), WiFi.RSSI(i));
  }

  for (uint8_t s = 0; s < 3; s++) {
    const char *ssid, *pass;
    if (!credentialAt(s, &ssid, &pass)) continue;
    bool seen = false;
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == ssid) { seen = true; break; }
    }
    Serial.printf("wifi: '%s' %s\n", ssid,
                  seen ? "IS visible -> the password is the likely problem"
                       : "NOT visible -> wrong name, out of range, or 5 GHz");
  }
  WiFi.scanDelete();
}

// Opens the commissioning portal, non-blocking.
void openPortal() {
  if (portalRunning) return;
  Serial.printf("wifi: opening config portal '%s' for %us\n",
                BOWLSTACK_DEVICE_ID, PORTAL_TIMEOUT_S);

  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);

  // Both are essential, not tuning. setConfigPortalBlocking(false) only skips
  // the startConfigPortal() wait loop -- it does nothing for the SAVE path,
  // which runs inside wm.process() and therefore inside our loop(). Left at
  // their defaults of 0, WiFiManager falls through to
  // WiFi.waitForConnectResult() with Arduino's 60 s default, stalling the
  // sensors for a minute at the exact moment a technician is commissioning the
  // unit. The library's own `if(connect)` guard around that wait is commented
  // out, so setSaveConnect(false) does not avoid it either -- only a bound does.
  wm.setConnectTimeout(WM_CONNECT_TIMEOUT_S);
  wm.setSaveConnectTimeout(WM_CONNECT_TIMEOUT_S);

  // startConfigPortal, not autoConnect: autoConnect first retries the saved
  // credentials and blocks doing so. Every credential we know has already been
  // tried by the caller, so that attempt would be pure delay.
  wm.startConfigPortal(BOWLSTACK_DEVICE_ID);
  portalRunning = true;
}

// Non-blocking retry. Rotates through every slot because the compiled-in
// networks and the commissioned one can each come back independently -- always
// retrying the last-used credential would miss a site whose original network
// returned.
void retryNextCredential() {
  static uint8_t idx = 0;
  for (uint8_t attempt = 0; attempt < 3; attempt++) {
    const uint8_t slot = idx++ % 3;
    const char *ssid, *pass;
    if (!credentialAt(slot, &ssid, &pass)) continue;
    Serial.printf("wifi: retrying '%s'\n", ssid);
    WiFi.begin(ssid, pass);  // returns immediately; associates in background
    return;
  }
}

}  // namespace

void begin() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  // NOT persistent(true). With flash storage enabled every WiFi.begin(ssid,
  // pass) rewrites the stack's stored record, so the boot joins below would
  // erase whatever the portal commissioned. Credentials are kept in our own
  // Preferences namespace instead, which also spares the flash a write on every
  // retry.
  WiFi.persistent(false);

  loadCommissioned();

  for (uint8_t s = 0; s < 3; s++) {
    const char *ssid, *pass;
    if (credentialAt(s, &ssid, &pass) && tryJoin(ssid, pass)) return;
  }

  // Say WHY before falling back, so a bad password is distinguishable from a
  // network that was never visible.
  scanNetworks();

  // Nothing known is reachable. Open the portal so the unit can be commissioned
  // on site without a rebuild -- the reason one binary can serve the fleet.
  openPortal();
}

void loop() {
  const uint32_t now = millis();

  // Link state is sampled EVERY iteration -- WiFi.status() is a variable read,
  // so this costs nothing and a change is noticed within a millisecond. The
  // retry interval below governs how often we ACT, not how quickly we notice.
  const bool nowConnected = (WiFi.status() == WL_CONNECTED);

  // Transitions only, never steady state: one line per link change keeps the
  // terminal readable while still showing every connect, drop and recovery.
  if (nowConnected != wasConnected) {
    if (nowConnected) {
      snprintf(joinedSsid, sizeof(joinedSsid), "%s", WiFi.SSID().c_str());

      // How long the outage lasted is the most useful number from a field
      // trial: it says whether a site's WiFi is stable enough for the fleet.
      char outage[32] = "";
      if (offlineTracked) {
        snprintf(outage, sizeof(outage), "  (offline %us)",
                 (now - offlineSinceMs) / 1000);
      }
      Serial.printf("wifi: %s '%s'  ip=%s  %d dBm%s\n",
                    everConnected ? "RECONNECTED to" : "CONNECTED to",
                    joinedSsid, WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                    outage);
      everConnected = true;
      offlineTracked = false;
      retryArmed = false;

      if (portalRunning) {
        // Someone just commissioned this unit. Remember the network so it
        // survives the next reboot, then take the AP down -- leaving it up
        // wastes power and keeps advertising the device to anyone scanning.
        storeCommissioned(WiFi.SSID().c_str(), WiFi.psk().c_str());
        wm.stopConfigPortal();
        portalRunning = false;
      }
    } else {
      Serial.printf("wifi: DISCONNECTED from '%s'\n",
                    joinedSsid[0] ? joinedSsid : "(none)");
      offlineTracked = true;
      offlineSinceMs = now;
      retryArmed = false;
    }
    wasConnected = nowConnected;
  }

  // Pump the captive portal. Cheap while idle; the save path is bounded by the
  // timeouts set in openPortal().
  if (portalRunning) {
    wm.process();

    if (!wm.getConfigPortalActive()) {
      // Timed out with nobody configuring it. Not fatal: bowl counting
      // continues and telemetry buffers. Restart the offline clock so the
      // retries get another full window before the portal reopens -- otherwise
      // a permanently dead network would pin the device in AP mode, unable to
      // notice its own network returning.
      portalRunning = false;
      offlineSinceMs = now;
      offlineTracked = true;
      retryArmed = false;
      Serial.println("wifi: portal closed unconfigured - offline, will retry");
    }
    return;
  }

  if (nowConnected) return;

  if (!offlineTracked) {
    offlineTracked = true;
    offlineSinceMs = now;
  }

  // Nothing known has answered for a long time. Reopen the portal so the unit
  // can be pointed at a new network in the field.
  if ((int32_t)(now - (offlineSinceMs + PORTAL_AFTER_OFFLINE_MS)) >= 0) {
    Serial.printf("wifi: offline %us with no known network - reopening portal\n",
                  (now - offlineSinceMs) / 1000);
    offlineSinceMs = now;  // restart the clock; the portal has its own timeout
    openPortal();
    return;
  }

  if (!retryArmed) {
    retryArmed = true;
    nextRetryMs = now;  // first retry immediately on going offline
  }
  if ((int32_t)(now - nextRetryMs) < 0) return;
  nextRetryMs = now + RETRY_PERIOD_MS;

  retryNextCredential();
}

bool connected() { return WiFi.status() == WL_CONNECTED; }

const char *ssid() { return joinedSsid; }

int8_t rssi() { return WiFi.status() == WL_CONNECTED ? (int8_t)WiFi.RSSI() : 0; }

}  // namespace net
