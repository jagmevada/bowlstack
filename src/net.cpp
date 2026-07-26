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

// How long the static credentials get before the portal opens. Short, so an
// installer standing at a unit whose network is wrong does not wait minutes for
// somewhere to type the new one.
//
// It is only safe to be this aggressive because retries CONTINUE while the
// portal is open (see loop()). Otherwise 30 s would turn every router reboot
// into a full PORTAL_TIMEOUT_S outage: the portal would open before the router
// finished restarting, and nothing would attempt the real network until it
// closed again.
const uint32_t PORTAL_AFTER_OFFLINE_MS = 30000;  // 30 seconds

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
  // Read-WRITE, not read-only. Opening a namespace read-only before it has
  // ever been written fails with NOT_FOUND and logs an error, and returns
  // nothing -- so the first boot after flashing looks identical to a corrupt
  // store. Opening read-write creates it; no flash is written unless we put
  // something.
  if (!prefs.begin("bowlstack", false)) {
    Serial.println("wifi: NVS unavailable - commissioned network cannot persist");
    return;
  }
  prefs.getString("ssid", credSsid2, sizeof(credSsid2));
  prefs.getString("pass", credPass2, sizeof(credPass2));
  prefs.end();

  if (credSsid2[0]) {
    Serial.printf("wifi: commissioned network on file: '%s'\n", credSsid2);
  } else {
    Serial.println("wifi: no commissioned network stored yet");
  }
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

  // Complete lines only. Leaving a line open across the blocking wait below
  // let the sensor task -- which now runs on the other core and keeps printing
  // throughout -- interleave into it. The result was a plotter sample prefixed
  // with "wifi: trying...", which no longer starts with '>' and is silently
  // discarded by the plotter.
  Serial.printf("wifi: trying '%s'\n", ssid);
  WiFi.begin(ssid, pass);

  const uint32_t deadline = millis() + JOIN_TIMEOUT_MS;
  while (WiFi.status() != WL_CONNECTED && (int32_t)(millis() - deadline) < 0) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    snprintf(joinedSsid, sizeof(joinedSsid), "%s", ssid);
    Serial.printf("wifi: joined '%s', %s (%d dBm)\n", ssid,
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    // Seed the transition tracker so loop() does not log a duplicate CONNECTED
    // line for a link this function already reported.
    wasConnected = true;
    everConnected = true;
    return true;
  }

  const wl_status_t st = WiFi.status();
  Serial.printf("wifi: '%s' failed (%d: %s)\n", ssid, (int)st, statusText(st));
  WiFi.disconnect();
  return false;
}

// Scans once, then joins the strongest KNOWN network that is actually on air.
//
// A single radio cannot associate with several APs at once -- each WiFi.begin()
// supersedes the last -- but a SCAN does examine every SSID on every channel in
// one operation, which is as close to "all at once" as the hardware gets.
//
// Joining blind instead costs JOIN_TIMEOUT_MS for every network that is not
// there: in the field that meant 12 s burned on a 5 GHz SSID the radio cannot
// even see, before the reachable network was attempted at all. Scanning first
// spends ~2-3 s once and then makes exactly one join attempt per network that
// genuinely exists, strongest first.
//
// It doubles as the diagnostic: a configured SSID missing from the scan
// explains itself, where a bare join failure cannot distinguish a wrong
// password from a network on a band this radio cannot receive.
bool joinBestVisible() {
  Serial.println("wifi: scanning (ESP32 is 2.4 GHz only; 5 GHz APs cannot appear)");
  const int n = WiFi.scanNetworks();

  if (n <= 0) {
    Serial.println("wifi:   nothing visible at all - check the antenna");
    WiFi.scanDelete();
    return false;
  }

  int32_t rssi[3] = {-127, -127, -127};
  bool present[3] = {false, false, false};

  for (int i = 0; i < n && i < 30; i++) {
    Serial.printf("wifi:   %-24s ch%-3d %4d dBm\n", WiFi.SSID(i).c_str(),
                  WiFi.channel(i), WiFi.RSSI(i));
    for (uint8_t s = 0; s < 3; s++) {
      const char *ssid, *pass;
      if (!credentialAt(s, &ssid, &pass)) continue;
      if (WiFi.SSID(i) != ssid) continue;
      if (!present[s] || WiFi.RSSI(i) > rssi[s]) {
        present[s] = true;
        rssi[s] = WiFi.RSSI(i);
      }
    }
  }
  WiFi.scanDelete();

  // Account for every configured slot, so a missing one is explained rather
  // than silently skipped.
  for (uint8_t s = 0; s < 3; s++) {
    const char *ssid, *pass;
    if (!credentialAt(s, &ssid, &pass)) continue;
    if (present[s]) {
      Serial.printf("wifi: '%s' visible (%d dBm)\n", ssid, rssi[s]);
    } else {
      Serial.printf("wifi: '%s' NOT visible - wrong name, out of range, or 5 GHz\n",
                    ssid);
    }
  }

  // Strongest first: with several known networks on air, the one with the best
  // signal is the one most likely to hold up.
  for (uint8_t attempt = 0; attempt < 3; attempt++) {
    int8_t best = -1;
    for (uint8_t s = 0; s < 3; s++) {
      if (!present[s]) continue;
      if (best < 0 || rssi[s] > rssi[best]) best = s;
    }
    if (best < 0) break;
    present[best] = false;  // consumed

    const char *ssid, *pass;
    credentialAt((uint8_t)best, &ssid, &pass);
    if (tryJoin(ssid, pass)) return true;
  }

  Serial.println("wifi: no known network could be joined");
  return false;
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

  if (joinBestVisible()) return;

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

      // Remember ANY network that is not compiled in -- by definition someone
      // commissioned it, and it must survive the next boot.
      //
      // Deliberately NOT gated on portalRunning. WiFiManager's save path calls
      // shutdownConfigPortal() from inside wm.process(), so by the time this
      // transition is seen the portal has already gone inactive and
      // portalRunning has been cleared by the branch below -- the commissioned
      // network would never be stored, which is exactly how a unit configured
      // through the AP came back after a reboot still hunting only the static
      // SSIDs.
      const bool isStatic = (WIFI_SSID_1[0] && strcmp(joinedSsid, WIFI_SSID_1) == 0) ||
                            (WIFI_SSID_2[0] && strcmp(joinedSsid, WIFI_SSID_2) == 0);
      if (!isStatic) {
        storeCommissioned(WiFi.SSID().c_str(), WiFi.psk().c_str());
      }

      if (portalRunning) {
        // The AP has done its job: take it down rather than leaving it
        // advertising the device and burning power.
        wm.stopConfigPortal();
        portalRunning = false;
        Serial.println("wifi: link is up - closing config portal");
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

  if (!nowConnected) {
    if (!offlineTracked) {
      offlineTracked = true;
      offlineSinceMs = now;
    }

    // Open the portal once the known networks have had their chance, so a unit
    // whose network was replaced can be re-pointed on site rather than needing
    // a reflash.
    if (!portalRunning &&
        (int32_t)(now - (offlineSinceMs + PORTAL_AFTER_OFFLINE_MS)) >= 0) {
      Serial.printf("wifi: offline %us with no known network\n",
                    (now - offlineSinceMs) / 1000);
      openPortal();
    }

    // Retries run EVEN WHILE THE PORTAL IS OPEN. WiFiManager puts the radio in
    // AP_STA, so commissioning and recovery proceed in parallel: the AP stays
    // available for an installer while the device keeps reaching for its own
    // networks. Skipping retries here is what would make a 30 s portal trigger
    // turn a brief router reboot into a full portal-timeout outage -- and the
    // link-state check above notices the moment one succeeds, tearing the AP
    // down on its own.
    if (!retryArmed) {
      retryArmed = true;
      nextRetryMs = now;  // first retry immediately on going offline
    }
    if ((int32_t)(now - nextRetryMs) >= 0) {
      nextRetryMs = now + RETRY_PERIOD_MS;
      retryNextCredential();
    }
  }

  // Pump the captive portal. Cheap while idle; the save path is bounded by the
  // timeouts set in openPortal().
  if (portalRunning) {
    wm.process();

    if (!wm.getConfigPortalActive()) {
      // Timed out with nobody configuring it. Not fatal: bowl counting
      // continues, telemetry buffers, and the retries above never stopped.
      // Restart the offline clock so the portal reopens after another window
      // rather than immediately re-arming on the next iteration.
      portalRunning = false;
      offlineSinceMs = now;
      offlineTracked = true;
      Serial.println("wifi: portal closed unconfigured - still retrying");
    }
  }
}

bool connected() { return WiFi.status() == WL_CONNECTED; }

const char *ssid() { return joinedSsid; }

int8_t rssi() { return WiFi.status() == WL_CONNECTED ? (int8_t)WiFi.RSSI() : 0; }

}  // namespace net
