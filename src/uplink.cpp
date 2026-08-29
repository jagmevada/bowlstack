#include "uplink.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "secret.h"

namespace uplink {
namespace {

// Long enough for a TLS handshake over a marginal link, short enough that a
// stalled request cannot hold a caller's task for a whole heartbeat period.
const uint32_t HTTP_TIMEOUT_MS = 8000;

// Per PROCESS, deliberately shared by every caller: one TLS session for the
// whole device, because re-handshaking per request is what dominates egress --
// far more than the payloads themselves.
WiFiClientSecure *tls_ = nullptr;

LinkUp linkUp_ = nullptr;

String base_;
bool baseComputed_ = false;

}  // namespace

void begin(LinkUp linkUp) {
  linkUp_ = linkUp;

  // Only once. A second product initialising in the same image must not throw
  // away a session the first one is holding.
  if (tls_ == nullptr) {
    tls_ = new WiFiClientSecure();
    // No certificate pinning: the anon key grants write-only access with no
    // read path, so an intercepted session yields nothing readable and can at
    // worst inject telemetry. Pin a root CA here if that ever changes.
    tls_->setInsecure();
  }
}

bool connected() { return linkUp_ != nullptr && linkUp_(); }

const char *apiBase() {
  if (!baseComputed_) {
    baseComputed_ = true;
    base_ = String(SUPABASE_URL);
    while (base_.endsWith("/")) base_.remove(base_.length() - 1);
    if (base_.endsWith("/rest/v1")) base_.remove(base_.length() - 8);
    while (base_.endsWith("/")) base_.remove(base_.length() - 1);
  }
  return base_.c_str();
}

uint32_t newBootId() { return esp_random(); }

Result request(const char *method, const char *path, const char *query,
               const char *prefer, const String &body) {
  Result r;
  r.code = -1000;
  r.pgCode[0] = '\0';
  r.matchedZeroRows = false;

  if (!connected()) return r;
  if (tls_ == nullptr) {
    r.code = -1002;  // begin() was never called
    return r;
  }

  HTTPClient http;
  String url = String(apiBase()) + path;
  if (query != nullptr && query[0] != '\0') {
    url += "?";
    url += query;
  }

  if (!http.begin(*tls_, url)) {
    r.code = -1001;
    return r;
  }

  // Reuse keeps the TLS session alive across posts. Without it each request
  // re-downloads the certificate chain, which dominates egress far more than
  // the payloads themselves.
  http.setReuse(true);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", prefer);

  static const char *kHeaders[] = {"Content-Range"};
  http.collectHeaders(kHeaders, 1);

  r.code = http.sendRequest(method, (uint8_t *)body.c_str(), body.length());

  // "0-0/1" on success, "*/0" when nothing matched.
  const String range = http.header("Content-Range");
  r.matchedZeroRows = range.endsWith("/0");

  // Only read the body on failure -- on success it is empty anyway thanks to
  // return=minimal, and the error text is what we want for diagnosis.
  if (!r.ok()) {
    const String err = http.getString();
    Serial.printf("uplink: %s %s -> %d %s\n", method, path, r.code,
                  err.substring(0, 180).c_str());

    // Extracted here but ACTED ON by the caller. This function knows nothing
    // about devices, while "this device is not registered" is a fact about one
    // installation -- and a batch carrying rows for several installations
    // cannot attribute a foreign-key violation from the SQLSTATE alone.
    const int at = err.indexOf("\"code\":\"");
    if (at >= 0 && (int)err.length() >= at + 13) {
      err.substring(at + 8, at + 13).toCharArray(r.pgCode, sizeof(r.pgCode));
    }
  }

  http.end();
  return r;
}

}  // namespace uplink
