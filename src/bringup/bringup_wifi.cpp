// The platform half of the WiFi page: the part that knows what a radio is.
//
// ui_wifi.cpp draws and collects; this file scans, joins and reports. The seam
// exists so the page compiles and runs in the desktop preview, which has no
// radio at all -- see ui_state.h for the same argument about DeviceStatus.
//
// THE WiFiManager PLAN IS OFF, and this file used to say the opposite. It read
// "WiFiManager comes back when the full firmware does, and the two meet at
// net.cpp rather than inside a screen". The full firmware is here, and it does
// not: net.cpp exists to give a HEADLESS box a user interface, and this box has
// a better one already -- ui_wifi.cpp renders the scan list, the signal bars, a
// passphrase keyboard with popovers, and the live association state.
//
// Linking net.cpp beside this file would also have been actively broken rather
// than merely redundant, because the two contest the radio in three ways:
//
//   scans   net.cpp scans BLOCKING and then scanDelete()s the result; this file
//           scans async and collects it later. Either one's scanDelete() makes
//           the other's scanComplete() answer -2, which reads as a radio fault.
//   joins   net::retryNextCredential() re-issues WiFi.begin() every 10 s. Each
//           WiFi.begin supersedes the last, so it would silently cancel a join
//           a technician had just made from the glass.
//   the AP  WiFiManager raises an OPEN AP named "LDC-001", while the QR on the
//           WiFi page advertises WPA "Bowlstack-LDC-001". Linking it would not
//           even have made that QR true.
//
// WHAT DID COME ACROSS FROM net.cpp IS THE HALF THAT WAS MISSING HERE, and it
// is the half that matters in the field: three credential slots, commissioned
// credentials in NVS, a scan-then-join-the-strongest-known boot, and a bounded
// retry that survives the millis() wrap. Until this commit a unit joined from
// the glass forgot its network at the next power cycle, and a unit that had
// never been touched never joined anything at all -- so every load-cell station
// was, in practice, an offline device with a WiFi page on it.

#include <string.h>
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>

// GITIGNORED, and the build depends on it. secret.h.example documents the two
// SSID/passphrase pairs this file reads; a clean clone that has not copied it
// fails here rather than silently shipping a device with no networks -- which
// is the failure mode worth having, and the same one net.cpp and telemetry.cpp
// already carry for the discrete images.
#include "secret.h"

#include "bringup_wifi.h"
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

// True while the join in flight came from the scan ranking rather than from a
// finger. It decides only one thing -- whether a failure should advance to the
// next candidate -- but that distinction matters: a person who typed a
// passphrase wants to be told it failed, not to watch the device wander off and
// join something else.
bool joinIsAuto_ = false;

// Rescan cadence while ASSOCIATED, where a scan is no longer doing any work for
// the device -- it only refreshes a list for somebody who might be looking at
// the WiFi page.
//
// It was 20 s, and 20 s is what the Supabase heartbeat will run at
// (STATUS_PERIOD_MS in telemetry.cpp) against an 8 s HTTP timeout. A scan makes
// the radio leave its channel for a couple of seconds; at equal periods it would
// sit at a fixed phase against the post and either always miss it or always land
// on it -- and "always lands on it" is a device that times out, backs off 15 s,
// and looks intermittently offline to a dashboard tuned to a 40 s threshold.
//
// 45 rather than 40 or 60 for exactly that reason: both of those are integer
// multiples of 20, so a collision would recur at a fixed phase instead of
// drifting through it. The real fix is to scan only while the page is open,
// which needs the UI to say so; this is the version that costs no UI change.
const uint32_t RESCAN_MS = 45000;

// ...and the cadence while OFFLINE, where the scan is no longer just feeding a
// list somebody is reading -- it is the retry. 10 s matches net.cpp's
// RETRY_PERIOD_MS, and it is a floor rather than a target: each attempt needs a
// few seconds to resolve, so going much shorter would cancel one still in
// progress.
const uint32_t RECONNECT_SCAN_MS = 10000;

const uint32_t JOIN_TIMEOUT_MS = 12000;

// How long a failed on-screen join keeps the radio to itself before the ranked
// walk resumes. See the join watchdog in loop() for the argument.
const uint32_t MANUAL_FAIL_GRACE_MS = 30000;

// How a REFUSED scan is retried, as opposed to a completed one. See publishScan.
const uint8_t SCAN_FAST_RETRIES = 4;
const uint32_t SCAN_FAST_RETRY_MS = 1500;
uint8_t scanFailures_ = 0;

// ONE definition, used by both refusal paths -- the synchronous one in loop()
// and the asynchronous one in publishScan(). They are the same condition seen
// from two places, and when they had a backoff each, only one of them got
// fixed.
uint32_t scanFailBackoff() {
  scanFailures_ = (scanFailures_ < 255) ? (uint8_t)(scanFailures_ + 1) : 255;
  if (scanFailures_ <= SCAN_FAST_RETRIES) return SCAN_FAST_RETRY_MS;
  return (WiFi.status() == WL_CONNECTED) ? RESCAN_MS : RECONNECT_SCAN_MS;
}

// --- credentials -----------------------------------------------------------
// Three slots, the same shape net.cpp uses: two compiled in from secret.h, and
// one commissioned on site. The commissioned one lives in OUR OWN NVS namespace
// rather than the WiFi stack's, and both halves of that are deliberate:
//
//   * WiFi.persistent(true) makes every WiFi.begin(ssid, pass) rewrite the
//     stack's stored record, so an auto-join attempt would erase whatever was
//     commissioned -- losing it on the first reboot after commissioning, which
//     is exactly when it has to survive.
//   * WiFi.begin() with no arguments does NOT reload NVS on ESP32; it re-applies
//     whatever station config is already in RAM, so "retry the saved network"
//     written that way would just repeat the previous attempt.
//
// Owning the storage sidesteps both, and WiFi.persistent(false) below then keeps
// the stack from writing flash on every retry.
//
// THE NAMESPACE IS net.cpp's, not a new one. A board moved between the two
// products keeps its network, which is the friendlier field behaviour, and the
// key names are the same two. It does not collide with scale.cpp, which owns
// "bowlscale" -- so a calibration reset and a network reset cannot reach each
// other's keys.
const char *NVS_NS = "bowlstack";

char credSsid_[33] = {0};
char credPass_[65] = {0};

Preferences prefs_;

char joinedSsid_[33] = {0};

// Link-state edge tracking, so the console logs transitions rather than steady
// state. everConnected_ is what distinguishes a first join from a recovery.
bool wasConnected_ = false;
bool everConnected_ = false;
bool offlineTracked_ = false;
uint32_t offlineSinceMs_ = 0;

// Set when the next completed scan should pick a network and join it. Armed at
// boot and re-armed whenever a join attempt fails or the link drops, so the
// device is always working from a FRESH scan rather than joining blind.
bool wantAutoJoin_ = true;

// Which slots this offline spell has already tried, so a cycle walks every
// configured network once instead of hammering the strongest one that happens
// to be refusing us. Cleared on association and when the walk is exhausted.
uint8_t autoTried_ = 0;

const uint8_t CRED_SLOTS = 3;

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

// Returns false when the slot is empty.
bool credentialAt(uint8_t i, const char **ssid, const char **pass) {
  switch (i) {
    case 0:  *ssid = WIFI_SSID_1; *pass = WIFI_PASS_1; break;
    case 1:  *ssid = WIFI_SSID_2; *pass = WIFI_PASS_2; break;
    default: *ssid = credSsid_;   *pass = credPass_;   break;
  }
  return (*ssid)[0] != '\0';
}

void loadCommissioned() {
  // Read-WRITE, not read-only. Opening a namespace read-only before it has ever
  // been written fails with NOT_FOUND and logs an error, so the first boot after
  // flashing looks identical to a corrupt store. Opening read-write creates it;
  // no flash is written unless we put something.
  if (!prefs_.begin(NVS_NS, false)) {
    Serial.println("wifi: NVS unavailable - commissioned network cannot persist");
    return;
  }
  prefs_.getString("ssid", credSsid_, sizeof(credSsid_));
  prefs_.getString("pass", credPass_, sizeof(credPass_));
  prefs_.end();

  if (credSsid_[0]) Serial.printf("wifi: commissioned network on file: '%s'\n", credSsid_);
  else Serial.println("wifi: no commissioned network stored yet");
}

void storeCommissioned(const char *ssid, const char *pass) {
  if (ssid == nullptr || ssid[0] == '\0') return;
  if (strcmp(ssid, credSsid_) == 0 && strcmp(pass, credPass_) == 0) return;

  snprintf(credSsid_, sizeof(credSsid_), "%s", ssid);
  snprintf(credPass_, sizeof(credPass_), "%s", pass);

  prefs_.begin(NVS_NS, false);
  prefs_.putString("ssid", credSsid_);
  prefs_.putString("pass", credPass_);
  prefs_.end();
  Serial.printf("wifi: commissioned '%s' saved for future boots\n", credSsid_);
}

// One place where a join actually starts, whoever asked for it. The manual path
// and the ranked auto path must exercise the SAME code, or the one used less
// often is the one that quietly stops working.
void beginJoin(const char *ssid, const char *pass, bool isAuto) {
  // A scan in flight would otherwise be abandoned mid-flight by the join and
  // leave scanComplete() reporting a failure that looks like a radio fault.
  if (scanRunning_) {
    WiFi.scanDelete();
    scanRunning_ = false;
  }
  // STAGED THROUGH LOCALS, BECAUSE ONE CALLER HANDS US OUR OWN BUFFERS. The
  // panel's join arrives as joinPending_ and loop() passes joinSsid_/joinPass_
  // straight back in, so copying the arguments directly into those same buffers
  // is snprintf with its source and destination the same object -- undefined
  // behaviour, and undefined on the ONE path somebody is standing there
  // watching. The auto path could never hit it: its pointers come from
  // credentialAt(), which returns secret.h literals or credSsid_/credPass_.
  //
  // That asymmetry is the whole reason it survived. The ranked auto-join is
  // exercised every boot and was always well-formed; the typed join is used
  // once per site and was not.
  char s[sizeof(joinSsid_)];
  char p[sizeof(joinPass_)];
  snprintf(s, sizeof(s), "%s", ssid ? ssid : "");
  snprintf(p, sizeof(p), "%s", pass ? pass : "");
  memcpy(joinSsid_, s, sizeof(joinSsid_));
  memcpy(joinPass_, p, sizeof(joinPass_));
  joinIsAuto_ = isAuto;
  WiFi.begin(joinSsid_, joinPass_);
  joinStartedMs_ = millis();
  Serial.printf("wifi: connecting to '%s' (%s)...\n", joinSsid_,
                isAuto ? "auto" : "from the panel");
}

// Picks the strongest KNOWN network that is actually on air, out of a scan
// result that has NOT yet been deleted.
//
// A single radio cannot associate with several APs at once -- each WiFi.begin()
// supersedes the last -- but a SCAN examines every SSID on every channel in one
// operation, which is as close to "all at once" as the hardware gets. Joining
// blind instead costs JOIN_TIMEOUT_MS for every network that is not there: on
// the discrete product that meant 12 s burned on a 5 GHz SSID the radio cannot
// even see, before the reachable network was attempted at all.
//
// Returns the slot to join, or -1. It deliberately does NOT start the join
// itself: beginJoin() abandons any scan in flight, and calling it from in here
// would delete the result set its caller is still standing on. Choose here,
// join after the scan has been released -- see publishScan().
int8_t pickFromScan(int16_t n) {
  int32_t rssi[CRED_SLOTS];
  bool present[CRED_SLOTS] = {};

  for (uint8_t s = 0; s < CRED_SLOTS; s++) rssi[s] = -127;

  for (int16_t i = 0; i < n; i++) {
    const String seen = WiFi.SSID(i);
    if (seen.length() == 0) continue;
    for (uint8_t s = 0; s < CRED_SLOTS; s++) {
      const char *ssid, *pass;
      if (!credentialAt(s, &ssid, &pass)) continue;
      if (seen != ssid) continue;
      if (!present[s] || WiFi.RSSI(i) > rssi[s]) {
        present[s] = true;
        rssi[s] = WiFi.RSSI(i);
      }
    }
  }

  // Strongest first, skipping whatever this offline spell has already tried.
  int8_t best = -1;
  for (uint8_t s = 0; s < CRED_SLOTS; s++) {
    if (!present[s]) continue;
    if (autoTried_ & (1u << s)) continue;
    if (best < 0 || rssi[s] > rssi[best]) best = (int8_t)s;
  }

  if (best < 0) {
    // Account for every configured slot, so a missing one is EXPLAINED rather
    // than silently skipped. This doubles as the diagnostic a bare join failure
    // cannot give: a configured SSID absent from a scan says "wrong name, out of
    // range, or a band this radio cannot receive", where a failed association
    // could equally be a wrong passphrase.
    if (autoTried_ == 0) {
      Serial.println("wifi: no configured network is on air (ESP32 is 2.4 GHz only)");
      for (uint8_t s = 0; s < CRED_SLOTS; s++) {
        const char *ssid, *pass;
        if (!credentialAt(s, &ssid, &pass)) continue;
        Serial.printf("wifi:   '%s' NOT visible\n", ssid);
      }
      // AND WHAT *IS* ON AIR, because "not visible" on its own cannot tell a
      // network that is absent from one whose name does not match byte for
      // byte. An SSID is case sensitive and may carry a trailing space, and
      // neither shows up on a phone's hotspot screen -- so the LENGTH is
      // printed beside the name. 'Unodari' is 7; if the line below says 8, the
      // name has something on the end of it.
      Serial.printf("wifi: %d on air:\n", (int)n);
      for (int16_t i = 0; i < n; i++) {
        const String seen = WiFi.SSID(i);
        Serial.printf("wifi:   '%s' (%u chars, %d dBm, ch %d%s)\n", seen.c_str(),
                      (unsigned)seen.length(), (int)WiFi.RSSI(i), (int)WiFi.channel(i),
                      WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? ", open" : "");
      }
      Serial.println("wifi: Menu > Settings > WiFi to pick one from the list");
    } else {
      // Every visible candidate has now been tried and refused. Start the walk
      // again on the next scan rather than latching -- an AP that rejected us
      // while it was booting will accept us a minute later.
      autoTried_ = 0;
    }
    return -1;
  }

  autoTried_ |= (uint8_t)(1u << best);
  const char *ssid, *pass;
  credentialAt((uint8_t)best, &ssid, &pass);
  Serial.printf("wifi: '%s' is the strongest known network on air (%d dBm)\n", ssid,
                (int)rssi[best]);
  return best;
}

void publishScan() {
  const int16_t n = WiFi.scanComplete();

  // -1 is RUNNING and -2 is FAILED, and treating them alike latched scanning
  // off permanently: the rescan gate requires !scanRunning_, which only
  // publishScan() clears. One refusal -- reachable on a cold boot, since the
  // first scan fires 500 ms after WiFi.mode() while the STA may still be coming
  // up -- and the page froze on its last list with nothing said.
  if (n == WIFI_SCAN_RUNNING) return;
  if (n < 0) {
    // THE FIRST SCAN OF A COLD BOOT USUALLY FAILS, and that is the case this
    // backoff has to be sized for rather than against. begin() fires it 500 ms
    // after WiFi.mode(), and esp_wifi_start() is asynchronous -- so the driver
    // is often still coming up and answers WIFI_SCAN_FAILED. Measured on this
    // board: `scan failed (-2)` at boot+1.9 s, every time.
    //
    // Retrying THAT on the idle cadence is what makes it a bug rather than a
    // hiccup: the idle cadence is 45 s and this path used it unconditionally, so
    // one predictable startup refusal left the station offline for three quarters
    // of a minute before it had tried anything at all. It was survivable at the
    // 20 s this used to be and stopped being when the rescan slowed down --
    // a constant changed for one caller, silently repriced for another.
    //
    // So: a few fast retries while the radio settles, then the ordinary pacing
    // for whatever the link state actually is. Bounded, because a genuinely
    // broken radio must not spin the loop.
    const uint32_t backoff = scanFailBackoff();
    Serial.printf("wifi: scan failed (%d), retry %u in %lu ms\n", (int)n, scanFailures_,
                  (unsigned long)backoff);
    WiFi.scanDelete();
    scanRunning_ = false;
    nextScanMs_ = millis() + backoff;
    return;
  }
  scanFailures_ = 0;

  // THE STRONGEST TEN, ONE ROW PER NAME -- not the first ten the driver happened
  // to return. The loop here used to stop at `count < WIFI_MAX_NETWORKS` while
  // walking the scan in driver order, which is channel order and not signal
  // order. With 16 APs on air and room for 10, six were dropped essentially at
  // random, and the network somebody actually needed could simply be absent
  // from the picker with nothing on screen to say so. On a quiet bench every
  // network fits and the fault is invisible; a canteen with twenty APs is where
  // it bites, which is exactly where the panel gets used.
  //
  // Deduplicated by name as well: a mesh or an extender puts the same SSID on
  // several radios, and three rows reading the same word are three of the ten
  // slots spent saying one thing. pickFromScan() already keeps only the
  // strongest per credential; this is the same rule for the list a person taps.
  //
  // Insertion sort with eviction, because n is at most a few dozen and the list
  // is ten long -- the sort is cheaper than the String allocations above it.
  ui::Network list[ui::WIFI_MAX_NETWORKS];
  uint8_t count = 0;
  for (int16_t i = 0; i < n; i++) {
    // Skip hidden SSIDs: an empty row is not something a person can choose,
    // and it would occupy one of the ten slots the list has room for.
    const String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    const int16_t r = (int16_t)WiFi.RSSI(i);
    const bool sec = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);

    bool dup = false;
    for (uint8_t k = 0; k < count; k++) {
      if (strcmp(list[k].ssid, ssid.c_str()) != 0) continue;
      dup = true;
      if (r > list[k].rssi) { list[k].rssi = r; list[k].secured = sec; }
      break;
    }
    if (dup) continue;

    uint8_t pos = count;
    while (pos > 0 && list[pos - 1].rssi < r) pos--;
    if (pos >= ui::WIFI_MAX_NETWORKS) continue;  // weaker than everything held

    const uint8_t last = (count < ui::WIFI_MAX_NETWORKS)
                             ? count
                             : (uint8_t)(ui::WIFI_MAX_NETWORKS - 1);
    for (uint8_t k = last; k > pos; k--) list[k] = list[k - 1];
    snprintf(list[pos].ssid, sizeof(list[pos].ssid), "%s", ssid.c_str());
    list[pos].rssi = r;
    list[pos].secured = sec;
    if (count < ui::WIFI_MAX_NETWORKS) count++;
  }
  ui::wifiSetNetworks(list, count);

  // CHOOSE while the result set still exists, JOIN after it is released.
  // WiFi.SSID(i) and WiFi.RSSI(i) read out of the buffer scanDelete() frees, so
  // the ranking has to happen up here -- but beginJoin() itself deletes any scan
  // in flight, which from in here would be this one. Splitting the two keeps
  // exactly one scanDelete() on this path.
  const int8_t pick = (wantAutoJoin_ && !joinStartedMs_ && WiFi.status() != WL_CONNECTED)
                          ? pickFromScan(n)
                          : (int8_t)-1;

  WiFi.scanDelete();
  scanRunning_ = false;
  nextScanMs_ = millis() + (WiFi.status() == WL_CONNECTED ? RESCAN_MS : RECONNECT_SCAN_MS);

  Serial.printf("wifi: scan found %d, published %u\n", n, count);

  if (pick >= 0) {
    const char *ssid, *pass;
    credentialAt((uint8_t)pick, &ssid, &pass);
    wantAutoJoin_ = false;
    beginJoin(ssid, pass, true);
  }
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

// Link transitions, and everything that has to happen exactly once at one.
void serviceLink(uint32_t nowMs) {
  // Sampled EVERY iteration -- WiFi.status() is a variable read, so this costs
  // nothing and a change is noticed within a millisecond. The scan cadence above
  // governs how often we ACT, not how quickly we notice.
  const bool nowConnected = (WiFi.status() == WL_CONNECTED);
  if (nowConnected == wasConnected_) return;

  if (nowConnected) {
    snprintf(joinedSsid_, sizeof(joinedSsid_), "%s", WiFi.SSID().c_str());

    // How long the outage lasted is the single most useful number from a field
    // trial: it says whether a site's WiFi is stable enough for the fleet.
    char outage[32] = "";
    if (offlineTracked_) {
      snprintf(outage, sizeof(outage), "  (offline %lus)",
               (unsigned long)((nowMs - offlineSinceMs_) / 1000));
    }
    Serial.printf("wifi: %s '%s'  ip=%s  %d dBm%s\n",
                  everConnected_ ? "RECONNECTED to" : "CONNECTED to", joinedSsid_,
                  WiFi.localIP().toString().c_str(), WiFi.RSSI(), outage);

    everConnected_ = true;
    offlineTracked_ = false;
    joinStartedMs_ = 0;
    wantAutoJoin_ = false;
    autoTried_ = 0;

    // Remember ANY network that is not compiled in -- by definition somebody
    // commissioned it, and it must survive the next boot. Done HERE, on the
    // transition, rather than in the join callback: a join that fails must not
    // be remembered, and only the link coming up proves one did not.
    const bool isStatic = (WIFI_SSID_1[0] && strcmp(joinedSsid_, WIFI_SSID_1) == 0) ||
                          (WIFI_SSID_2[0] && strcmp(joinedSsid_, WIFI_SSID_2) == 0);
    if (!isStatic) storeCommissioned(joinedSsid_, WiFi.psk().c_str());
  } else {
    Serial.printf("wifi: DISCONNECTED from '%s'\n",
                  joinedSsid_[0] ? joinedSsid_ : "(none)");
    offlineTracked_ = true;
    offlineSinceMs_ = nowMs;
    // Re-arm the ranked join, and scan sooner than the idle cadence would.
    wantAutoJoin_ = true;
    autoTried_ = 0;
    nextScanMs_ = nowMs;
  }
  wasConnected_ = nowConnected;
}

void onJoin(const char *ssid, const char *pass) {
  snprintf(joinSsid_, sizeof(joinSsid_), "%s", ssid ? ssid : "");
  snprintf(joinPass_, sizeof(joinPass_), "%s", pass ? pass : "");
  joinPending_ = true;
  // A person at the glass outranks the ranking. Without this the auto path
  // could fire on the very next scan and supersede the join they just asked
  // for -- WiFi.begin() has no queue, the last caller wins.
  wantAutoJoin_ = false;
  // The passphrase is NOT logged. It is the one thing on this device worth
  // protecting, and a serial console is not private.
  Serial.printf("wifi: join requested for '%s' (%u char key)\n", joinSsid_,
                (unsigned)strlen(joinPass_));
}

}  // namespace

void begin() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  // DISASSOCIATE, DO NOT POWER THE INTERFACE DOWN. This was disconnect(true) --
  // whose first argument is `wifioff` -- which was right for a bring-up harness
  // that wanted a clean slate and only ever scanned. It is wrong now that begin()
  // goes straight on to rank and join: stopping the station makes the first scan
  // wait for it to come back up, for no benefit. eraseap stays false either way;
  // our credentials are in our own namespace, not the stack's.
  WiFi.disconnect(false, false);
  // Lets the stack re-associate with the SAME AP on its own after a brief drop,
  // which is most drops. The ranked retry below is for the case where that
  // fails or the AP is genuinely gone.
  WiFi.setAutoReconnect(true);

  ui::wifiOnJoin(onJoin);

  loadCommissioned();

  // Report what this image was built with, so a unit that never joins can be
  // diagnosed from the boot log alone rather than by guessing whether secret.h
  // was filled in. The passphrases are not printed.
  for (uint8_t s = 0; s < CRED_SLOTS; s++) {
    const char *ssid, *pass;
    if (!credentialAt(s, &ssid, &pass)) continue;
    Serial.printf("wifi: slot %u '%s'%s\n", s, ssid, s == 2 ? " (commissioned)" : "");
  }

  // The join is driven by the FIRST SCAN RESULT rather than attempted here.
  // begin() runs inside setup(), where a blocking association would be added
  // straight to the splash -- the discrete product spends up to 12 s per network
  // doing exactly that. Waiting for the scan costs nothing the UI notices,
  // because the scale task and page construction are running through it.
  wantAutoJoin_ = true;
  nextScanMs_ = millis() + 500;
  Serial.println("wifi: station mode, scanning shortly");
}

void loop(uint32_t nowMs) {
  if (joinPending_) {
    joinPending_ = false;
    beginJoin(joinSsid_, joinPass_, false);
  }

  if (joinStartedMs_) {
    if (WiFi.status() == WL_CONNECTED) {
      // The CONNECTED line itself is serviceLink()'s, so it is printed once
      // wherever the association came from -- including a stack auto-reconnect
      // that this branch never sees.
      joinStartedMs_ = 0;
    } else if ((int32_t)(nowMs - joinStartedMs_) > (int32_t)JOIN_TIMEOUT_MS) {
      // SIGNED, AND THAT IS THE WHOLE TYPED-JOIN BUG. joinStartedMs_ can be
      // LATER than nowMs: loop() is handed millis() sampled by its caller, and
      // the joinPending_ block right above runs beginJoin() -- which stamps
      // joinStartedMs_ with a fresh millis() a few milliseconds further on.
      // Unsigned, nowMs - joinStartedMs_ was then 4294967290 rather than -6,
      // which comfortably exceeds any timeout, so the panel's join was declared
      // failed on the same iteration it started and WiFi.disconnect() killed
      // the association about six milliseconds in. Every time.
      //
      // The ranked auto path never hit it because publishScan() calls
      // beginJoin() BELOW this check, so the check never sees a future stamp --
      // which is exactly why the field report was "it only connects to the
      // hardcoded network". Observed, not deduced: "failed after 4294967290 ms
      // (6: disconnected / still trying)".
      //
      // The cast also makes this wrap-safe at 49.7 days, the same way every
      // other deadline in this file is written.
      // Bounded, and the bound is the point. An unbounded wait would leave the
      // page showing "not connected" with no way to tell a wrong passphrase from
      // a slow one.
      const wl_status_t st = WiFi.status();
      Serial.printf("wifi: '%s' failed after %lu ms (%d: %s)\n", joinSsid_,
                    (unsigned long)(nowMs - joinStartedMs_), (int)st, statusText(st));
      joinStartedMs_ = 0;
      WiFi.disconnect();
      // BOTH PATHS RESUME THE RANKED WALK, but a typed join gets a grace period
      // first. The two failures want opposite things and the compromise is
      // timing rather than behaviour: somebody who has just mistyped a
      // passphrase needs the page to keep saying "not connected" long enough to
      // read it and retry, and must not watch the device wander onto another
      // network as if the key had worked. But a person who mistypes and then
      // walks away must not leave a kitchen station offline until the next power
      // cycle, which is what never re-arming would do.
      //
      // So: 30 s of the honest failure on screen, then fall back to whatever is
      // known and reachable.
      wantAutoJoin_ = true;
      nextScanMs_ = nowMs + (joinIsAuto_ ? 0 : MANUAL_FAIL_GRACE_MS);
    }
  }

  serviceLink(nowMs);

  if (!scanRunning_ && !joinStartedMs_ && (int32_t)(nowMs - nextScanMs_) >= 0) {
    // async = true, show_hidden = false. Async is what keeps the UI responsive
    // through the 2-10 s a scan takes.
    const int16_t started = WiFi.scanNetworks(true, false);
    if (started == WIFI_SCAN_FAILED) {
      // scanNetworks returns WIFI_SCAN_FAILED synchronously when the driver
      // refuses. Marking the scan as running on that would wedge the gate.
      //
      // THE SAME BACKOFF AS THE ASYNCHRONOUS REFUSAL, and it has to be: this is
      // the same cold-boot condition arriving through the other door. The
      // driver may decline the request outright, or accept it and answer
      // WIFI_SCAN_FAILED seconds later from scanComplete() -- which of those
      // happens is a race against esp_wifi_start() finishing. Pacing one at
      // 1.5 s and the other at the idle 45 s would make time-to-first-join
      // depend on which side of that race the boot landed on.
      // AND ONCE THE COLD-BOOT RETRIES ARE SPENT, STOP THE STACK CHASING AN AP
      // THAT IS GONE. esp_wifi refuses scan_start while a connect is in flight,
      // and WiFi.setAutoReconnect(true) in begin() has the stack retrying the
      // last access point on its own, indefinitely. So losing an AP could wedge
      // scanning for good -- observed on the bench at "retry 29" after the
      // joined network was switched off, with the panel showing an empty list
      // and a QR code and no way back short of a power cycle. In the field that
      // is a station which drops its network once and then never finds another,
      // including the two compiled in.
      //
      // eraseap = true, deliberately: it clears the STACK's copy of the AP so
      // auto-reconnect has nothing left to chase. Ours live in our own NVS
      // namespace -- see the note above credentialAt() -- and WiFi.persistent()
      // is false, so this touches RAM, not flash. The ranked walk re-joins from
      // our own store on the first scan that succeeds.
      //
      // Gated on being DISASSOCIATED and past the fast retries: a refusal in the
      // first second of a boot is only the radio still coming up, and tearing
      // the station down then would fight the thing we are waiting for.
      if (scanFailures_ > SCAN_FAST_RETRIES && WiFi.status() != WL_CONNECTED) {
        Serial.println("wifi: scan still refused -- dropping the stack's stale "
                       "association so the radio can look around");
        WiFi.disconnect(false, true);
      }
      nextScanMs_ = nowMs + scanFailBackoff();
      Serial.printf("wifi: scan request refused, retry %u in %lu ms\n", scanFailures_,
                    (unsigned long)(nextScanMs_ - nowMs));
    } else {
      scanRunning_ = true;
    }
  }
  if (scanRunning_) publishScan();

  // Status carries RSSI, which drifts constantly; republishing it at frame rate
  // would defeat the change detection in the page above.
  if ((int32_t)(nowMs - lastPublishMs_) >= 1000) {
    lastPublishMs_ = nowMs;
    publishStatus();
  }
}

bool connected() { return WiFi.status() == WL_CONNECTED; }

const char *ssid() { return joinedSsid_; }

int8_t rssi() { return WiFi.status() == WL_CONNECTED ? (int8_t)WiFi.RSSI() : 0; }

}  // namespace bringup_wifi
