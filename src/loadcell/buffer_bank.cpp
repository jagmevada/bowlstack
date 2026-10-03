#include "buffer_bank.h"

#include <Arduino.h>
#include <Preferences.h>
#include <nvs.h>
#include <stdarg.h>
#include <string.h>

// The same bus layer as every other converter on this board: one call set for a
// hardware peripheral and a bit-banged pair alike. This bus is the bit-banged kind.
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "board_waveshare_s3.h"
#include "nau7802.h"

// The reference mass the console's calibrate key ('g') weighs against, in grams.
// platformio.ini sets it; the panel's Calibrate page types its own instead.
#ifndef BOWLSTACK_BUF_CAL_MASS_G
#define BOWLSTACK_BUF_CAL_MASS_G 20000
#endif

namespace bufbank {
namespace {

const int PORT = board::BUF_PORT;
// 100 kHz: bit-banged, a USB-C pigtail of unknown length, and the original
// bit-banged cell on this board only became stable at this rate.
const uint32_t HZ = 100000;

// THE COMPILED DEFAULTS. Ids follow the buffer positions the database already knows;
// channels follow the order the cells are plugged into the bus's mux. Every field
// is overridable from the console (:buf N ...) and kept in NVS -- these are only
// what a unit that has never been told otherwise uses.
const SlotConfig DEFAULTS[SLOTS] = {
    {true, "BWL-001", "B1", 0},
    {true, "BWL-002", "B2", 1},
    {true, "BWL-003", "B3", 2},
};

// Polled faster than the converters' 10 SPS so a conversion is picked up within a
// quarter of its period; the snapshot at the counter's rate.
const uint32_t POLL_MS = 25;
const uint32_t PUBLISH_MS = 100;
// Bring-up attempts per slot. See bringUp() for why a retry is honest here.
const uint8_t BRINGUP_ATTEMPTS = 5;

const char *NVS_NS = "bufbank";
const char *LEGACY_NS = "bufscale";  // the field-trial build's single-cell namespace

struct Slot {
  SlotConfig cfg{};
  bool fitted = false;
  char name[20] = {0};  // "B1 BWL-001": the converter driver prints with it
  Nau7802 cell;
  lscale::LoadScale core;
  lscale::Persisted saved;  // what NVS holds, so only changes are written
  char lastResult[48] = {0};
  uint32_t resultSeq = 0;
};

Slot slots_[SLOTS];
bool busOk_ = false;
bool muxOk_ = false;
uint8_t fitted_ = 0;

SemaphoreHandle_t mutex_ = nullptr;
QueueHandle_t q_ = nullptr;
TaskHandle_t task_ = nullptr;
Snapshot published_;
uint32_t seq_ = 0;

enum class CmdType : uint8_t { Zero, Calibrate, Clear, SetBowls, SetUid, SetLabel, SetCh, SetOn, SetOff, Reliability };
struct Cmd {
  CmdType type;
  uint8_t slot;
  float value;
  char text[12];
};

// --- the bus -------------------------------------------------------------------------

// I2C BUS RECOVERY: nine clocks and a STOP, before the bus is used and before every
// bring-up retry.
//
// WHY: an ESP reset can land in the middle of a transaction, and the converter --
// which has its own power and did not reset -- is left part-way through a byte,
// holding SDA low or out of step with the next START. That is what the field-trial
// build saw after resets: the first bring-up failing at a silent step and the
// revision register reading 0x00. Nine clocks walk any slave to the end of its byte
// (the ninth is a NACK it cannot misread), and a STOP returns every device on the
// bus -- mux included -- to idle. The cost is ~100 us.
//
// Open-drain by hand: a line is driven only LOW (output, written low first so the
// pin never drives high into a device holding it down) and released by going back
// to an input, where R4/R5 pull it up.
void lineLow(int pin) {
  digitalWrite(pin, LOW);
  pinMode(pin, OUTPUT);
}
void lineRelease(int pin) { pinMode(pin, INPUT_PULLUP); }

void busClear() {
  const int sda = board::BUF_SDA, scl = board::BUF_SCL;
  lineRelease(sda);
  lineRelease(scl);
  delayMicroseconds(10);
  for (int i = 0; i < 9; i++) {
    lineLow(scl);
    delayMicroseconds(5);
    lineRelease(scl);
    delayMicroseconds(5);
  }
  // STOP: SDA rises while SCL is high.
  lineLow(scl);
  delayMicroseconds(5);
  lineLow(sda);
  delayMicroseconds(5);
  lineRelease(scl);
  delayMicroseconds(5);
  lineRelease(sda);
  delayMicroseconds(10);
}

bool busInit() {
  busClear();
  return lgfx::i2c::init(PORT, board::BUF_SDA, board::BUF_SCL).has_value();
}

// Writes the mux's control register: exactly one channel open, every other closed.
//
// BEFORE EVERY POLL, not cached. With more than one platform fitted the channel
// changes on every poll anyway, and with one it costs a byte 40 times a second --
// in exchange for recovering, on the very next poll, a mux that browned out and
// dropped its channel. The converter driver latches a cell Offline after five
// failed reads, so recovery that waited any longer would be recovery by luck.
bool selectCh(int8_t ch) {
  const uint8_t v = (ch < 0) ? 0 : (uint8_t)(1u << (ch & 7));
  return lgfx::i2c::transactionWrite(PORT, board::BUF_MUX_ADDR, &v, 1, HZ).has_value();
}

lscale::Link toLink(CellState s) {
  switch (s) {
    case CellState::Online: return lscale::Link::Online;
    case CellState::Warming: return lscale::Link::Warming;
    default: return lscale::Link::Offline;
  }
}

// --- NVS -----------------------------------------------------------------------------
// NVS WRITES BLOCK THIS TASK, and briefly the other core with it (a flash erase stalls
// code running from flash). They happen on a command, a bowl event or a learned
// figure changing -- never from the poll loop otherwise, and only for fields whose
// value actually changed.

const char *k(char *buf, const char *base, uint8_t i) {
  snprintf(buf, 8, "%s%u", base, (unsigned)(i + 1));  // NVS keys are capped at 15
  return buf;
}

// Missing keys are checked first: a get of a key that was never written logs an
// ERROR-level line for strings and floats, and a red line at every boot of a fresh
// unit is noise that teaches people to ignore red lines.
void loadConfig(Preferences &p, uint8_t i, SlotConfig &c) {
  char key[8];
  c = DEFAULTS[i];
  if (p.isKey(k(key, "en", i))) c.enabled = p.getBool(key, c.enabled);
  if (p.isKey(k(key, "uid", i))) p.getString(key, c.uid, sizeof(c.uid));
  if (p.isKey(k(key, "lbl", i))) p.getString(key, c.label, sizeof(c.label));
  if (p.isKey(k(key, "ch", i))) c.muxCh = p.getChar(key, c.muxCh);
}

void loadPersisted(Preferences &p, uint8_t i, lscale::Persisted &v) {
  char key[8];
  v = lscale::Persisted();
  if (p.isKey(k(key, "zs", i))) v.zeroed = p.getBool(key, false);
  if (p.isKey(k(key, "z", i))) v.zero = p.getInt(key, 0);
  if (p.isKey(k(key, "cpg", i))) v.cpg = p.getFloat(key, 0.0f);
  if (p.isKey(k(key, "bw", i))) v.bowls = p.getUChar(key, 0);
  if (p.isKey(k(key, "tf", i))) v.typicalFullG = p.getFloat(key, v.typicalFullG);
}

// THE FIELD TRIAL'S COMMISSIONING IS CARRIED OVER, not thrown away. The single-cell
// build kept B1's zero and factor in its own namespace; a unit flashed with this
// build would otherwise come up uncalibrated and need re-zeroing with the shelf
// emptied. Done once: after it, slot 1's own keys exist and win.
bool importLegacy(Preferences &p) {
  char key[8];
  if (p.isKey(k(key, "zs", 0)) || p.isKey(k(key, "cpg", 0))) return false;
  // THE RAW NVS API, NOT Preferences: the legacy namespace not existing is the NORMAL
  // case on every unit but the trial one, and Preferences reports a failed read-only
  // open as an ERROR line -- which would print on every boot until B1 is calibrated.
  // Types match how the field-trial build stored them through Preferences: putBool
  // is a u8, putInt an i32, putFloat a 4-byte blob.
  nvs_handle_t h;
  if (nvs_open(LEGACY_NS, NVS_READONLY, &h) != ESP_OK) return false;
  bool any = false;
  uint8_t zset = 0;
  int32_t zero = 0;
  if (nvs_get_u8(h, "zset", &zset) == ESP_OK && nvs_get_i32(h, "zero", &zero) == ESP_OK) {
    p.putBool(k(key, "zs", 0), zset != 0);
    p.putInt(k(key, "z", 0), zero);
    any = true;
  }
  float cpg = 0.0f;
  size_t len = sizeof(cpg);
  if (nvs_get_blob(h, "cpg", &cpg, &len) == ESP_OK && len == sizeof(cpg)) {
    p.putFloat(k(key, "cpg", 0), cpg);
    any = true;
  }
  nvs_close(h);
  return any;
}

void savePersisted(uint8_t i) {
  Slot &s = slots_[i];
  const lscale::Persisted v = s.core.persisted();
  Preferences p;
  if (!p.begin(NVS_NS, false)) return;
  char key[8];
  if (v.zeroed != s.saved.zeroed) p.putBool(k(key, "zs", i), v.zeroed);
  if (v.zero != s.saved.zero) p.putInt(k(key, "z", i), v.zero);
  if (v.cpg != s.saved.cpg) p.putFloat(k(key, "cpg", i), v.cpg);
  if (v.bowls != s.saved.bowls) p.putUChar(k(key, "bw", i), v.bowls);
  if (v.typicalFullG != s.saved.typicalFullG) p.putFloat(k(key, "tf", i), v.typicalFullG);
  p.end();
  s.saved = v;
}

void saveConfig(uint8_t i, const SlotConfig &c) {
  Preferences p;
  if (!p.begin(NVS_NS, false)) return;
  char key[8];
  p.putBool(k(key, "en", i), c.enabled);
  p.putString(k(key, "uid", i), c.uid);
  p.putString(k(key, "lbl", i), c.label);
  p.putChar(k(key, "ch", i), c.muxCh);
  p.end();
}

// --- bring-up --------------------------------------------------------------------------

// Is anything answering behind this slot's channel? Asked three times: a single NAK
// on a bit-banged bus is not evidence of absence.
bool probe(Slot &s) {
  for (int t = 0; t < 3; t++) {
    uint8_t rev = 0;
    if (selectCh(s.cfg.muxCh) && s.cell.readRegister(0x1F, &rev)) return true;
    delay(5);
  }
  return false;
}

// BOUNDED RETRY AT BRING-UP ONLY. No reading has been shown to anybody yet, so trying
// again costs nothing and misleads nobody -- unlike reviving a cell that dropped
// mid-run, which can report a weight against a state it no longer matches, and which
// this firmware still does not do. Each attempt clears the bus first and is CHECKED
// by reading the configuration back, because a converter that "came up" on a bad read
// can be converting at the wrong gain.
bool bringUp(Slot &s) {
  char why[40];
  for (uint8_t a = 1; a <= BRINGUP_ATTEMPTS; a++) {
    if (a > 1) {
      busInit();
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!selectCh(s.cfg.muxCh)) {
      snprintf(why, sizeof(why), "mux did not take the channel");
    } else if (!s.cell.begin()) {
      snprintf(why, sizeof(why), "begin() failed");
    } else if (s.cell.verifyConfig(why, sizeof(why))) {
      Serial.printf("  %s: up on attempt %u, config verified, rev 0x%02X\n", s.name, a,
                    s.cell.revision());
      return true;
    }
    Serial.printf("  %s: bring-up attempt %u of %u failed (%s)\n", s.name, a, BRINGUP_ATTEMPTS, why);
  }
  Serial.printf("  %s: OFFLINE -- did not come up in %u attempts; a power cycle retries\n", s.name,
                BRINGUP_ATTEMPTS);
  return false;
}

// --- events and commands ----------------------------------------------------------

void say(uint8_t i, const char *fmt, ...) {
  Slot &s = slots_[i];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s.lastResult, sizeof(s.lastResult), fmt, ap);
  va_end(ap);
  s.resultSeq++;
  Serial.printf("  %s: %s\n", s.name, s.lastResult);
}

void logBowls(uint8_t i, const lscale::BowlChange &c) {
  const lscale::Reading r = slots_[i].core.reading();
  Serial.printf("  %s: bowls %s %+d (step %+.1f kg) -> %u bowl%s%s, food %.1f kg\n", slots_[i].name,
                lscale::bowlEventText(c.event), c.delta, c.stepG / 1000.0f, c.bowlsAfter,
                c.bowlsAfter == 1 ? "" : "s", r.bowlsConfirmed ? "" : " (UNCONFIRMED)",
                r.foodG / 1000.0f);
}

// 1000 reads of the revision register, counted. The figure that says whether this
// bus can be trusted -- a single failure in a thousand is already worth knowing about.
void reliability(uint8_t i) {
  Slot &s = slots_[i];
  uint32_t ok = 0, fail = 0, wrong = 0;
  for (int n = 0; n < 1000; n++) {
    uint8_t v = 0;
    if (!selectCh(s.cfg.muxCh) || !s.cell.readRegister(0x1F, &v)) fail++;
    else if ((v & 0x0F) != 0x0F) wrong++;
    else ok++;
    if ((n & 63) == 63) vTaskDelay(1);  // let the scale task and the UI breathe
  }
  say(i, "1000 reads: %lu ok, %lu failed, %lu wrong value", (unsigned long)ok,
      (unsigned long)fail, (unsigned long)wrong);
}

void runCmd(const Cmd &c) {
  if (c.slot >= SLOTS) return;
  Slot &s = slots_[c.slot];
  switch (c.type) {
    case CmdType::SetUid:
    case CmdType::SetLabel:
    case CmdType::SetCh:
    case CmdType::SetOn:
    case CmdType::SetOff: {
      // CONFIGURATION APPLIES AT THE NEXT BOOT. Moving a live platform to another
      // channel or id mid-run would attribute one platform's history to another.
      SlotConfig n = s.cfg;
      if (c.type == CmdType::SetUid) snprintf(n.uid, sizeof(n.uid), "%s", c.text);
      // %.3s: runLine() already refuses a longer label; this states the bound for
      // the compiler too, so the format can never truncate silently.
      if (c.type == CmdType::SetLabel) snprintf(n.label, sizeof(n.label), "%.3s", c.text);
      if (c.type == CmdType::SetCh) n.muxCh = (int8_t)c.value;
      if (c.type == CmdType::SetOn) n.enabled = true;
      if (c.type == CmdType::SetOff) n.enabled = false;
      saveConfig(c.slot, n);
      say(c.slot, "saved: %s %s ch%d %s -- restart to apply", n.label, n.uid, n.muxCh,
          n.enabled ? "on" : "off");
      return;
    }
    default:
      break;
  }
  if (!s.fitted) {
    say(c.slot, "not fitted -- nothing to do");
    return;
  }
  switch (c.type) {
    case CmdType::Zero: {
      const lscale::Commit r = s.core.zero();
      if (r == lscale::Commit::Ok) {
        const lscale::Reading rd = s.core.reading();
        say(c.slot, "empty zero stored (%ld counts, p-p %ld)", (long)rd.zero, (long)rd.pp);
      } else {
        say(c.slot, "zero refused: %s", lscale::commitText(r));
      }
      break;
    }
    case CmdType::Calibrate: {
      float cpg = 0;
      const lscale::Commit r = s.core.calibrate(c.value, &cpg);
      if (r == lscale::Commit::Ok) say(c.slot, "calibrated: %.3f counts/g from %.2f kg", cpg, c.value / 1000.0f);
      else say(c.slot, "calibrate refused: %s", lscale::commitText(r));
      break;
    }
    case CmdType::Clear:
      s.core.clearCalibration();
      say(c.slot, "calibration cleared -- reading counts");
      break;
    case CmdType::SetBowls:
      s.core.setBowls((uint8_t)c.value);
      say(c.slot, "bowls set to %u (confirmed)", (unsigned)c.value);
      break;
    case CmdType::Reliability:
      reliability(c.slot);
      break;
    default:
      break;
  }
}

void publish() {
  Snapshot n;
  n.busOk = busOk_;
  n.muxOk = muxOk_;
  n.fitted = fitted_;
  for (uint8_t i = 0; i < SLOTS; i++) {
    const Slot &s = slots_[i];
    SlotSnapshot &o = n.slot[i];
    o.cfg = s.cfg;
    o.fitted = s.fitted;
    if (s.fitted) {
      o.r = s.core.reading();
      o.revision = s.cell.revision();
      o.sps = s.cell.sps();
    }
    memcpy(o.lastResult, s.lastResult, sizeof(o.lastResult));
    o.resultSeq = s.resultSeq;
  }
  n.seq = ++seq_;
  if (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    published_ = n;
    xSemaphoreGive(mutex_);
  }
}

// ONE TASK OWNS THE BUFFER BUS: every converter on it, the mux, and the slots' NVS.
void bankTask(void *) {
  for (uint8_t i = 0; i < SLOTS; i++) {
    Slot &s = slots_[i];
    if (!s.fitted) continue;
    if (!bringUp(s)) s.core.setLink(lscale::Link::Offline);
  }
  publish();

  uint32_t nextPublish = 0;
  for (;;) {
    const uint32_t now = millis();
    for (uint8_t i = 0; i < SLOTS; i++) {
      Slot &s = slots_[i];
      if (!s.fitted) continue;
      // A converter that has gone Offline is not polled and its channel is not
      // selected: with SCL held low every transaction burns ~26 ms in lgfx waiting
      // for the line, at a priority above the UI. No mid-run revival -- see bringUp().
      if (s.cell.state() != CellState::Offline) {
        selectCh(s.cfg.muxCh);
        if (s.cell.poll(now)) {
          const lscale::BowlChange ch = s.core.addSample(s.cell.counts(), now);
          if (ch.event != lscale::BowlEvent::None) logBowls(i, ch);
        }
      }
      s.core.setLink(toLink(s.cell.state()));
    }

    Cmd c;
    while (xQueueReceive(q_, &c, 0) == pdTRUE) runCmd(c);

    for (uint8_t i = 0; i < SLOTS; i++) {
      if (slots_[i].core.dirty()) {
        savePersisted(i);
        slots_[i].core.clearDirty();
      }
    }

    if ((int32_t)(now - nextPublish) >= 0) {
      nextPublish = now + PUBLISH_MS;
      publish();
    }
    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
  }
}

bool queueCmd(const Cmd &c) {
  if (!q_) {
    Serial.println("  buffer: no buffer bus -- command ignored");
    return false;
  }
  return xQueueSend(q_, &c, 0) == pdTRUE;
}

// --- the console's command line -----------------------------------------------------
bool lineOpen_ = false;
char line_[64];
uint8_t lineLen_ = 0;

void listSlots() {
  const Snapshot sn = snapshot();
  Serial.printf("\n  buffer bus: %s, mux %s, %u of %u slots fitted\n", sn.busOk ? "up" : "DOWN",
                sn.muxOk ? "answering" : "NOT ANSWERING", sn.fitted, SLOTS);
  for (uint8_t i = 0; i < SLOTS; i++) {
    const SlotSnapshot &s = sn.slot[i];
    Serial.printf("  %u  %-3s %-11s ch%d  %-3s %-10s zero %s  factor %s  bowls %u%s\n", i + 1,
                  s.cfg.label, s.cfg.uid, s.cfg.muxCh, s.cfg.enabled ? "on" : "off",
                  s.fitted ? "fitted" : "not fitted", s.r.zeroed ? "yes" : "no",
                  s.r.calibrated ? "yes" : "no", s.r.bowls, s.r.bowlsConfirmed ? "" : "?");
  }
}

void runLine() {
  Serial.printf("\n> %s\n", line_);
  char *save = nullptr;
  char *w0 = strtok_r(line_, " \t", &save);
  if (!w0 || strcmp(w0, ":buf") != 0) {
    Serial.println("  unknown command -- ':buf' to list, '?' for help");
    return;
  }
  char *w1 = strtok_r(nullptr, " \t", &save);
  if (!w1) {
    listSlots();
    return;
  }
  const int n = atoi(w1);
  if (n < 1 || n > SLOTS) {
    Serial.printf("  slot must be 1..%u\n", SLOTS);
    return;
  }
  char *w2 = strtok_r(nullptr, " \t", &save);
  char *w3 = strtok_r(nullptr, " \t", &save);
  Cmd c{};
  c.slot = (uint8_t)(n - 1);
  if (!w2) {
    Serial.println("  :buf N zero | cal <kg> | clear | bowls <n> | rel | uid <id> | label <L> | ch <0-7> | on | off");
    return;
  }
  if (!strcmp(w2, "zero")) c.type = CmdType::Zero;
  else if (!strcmp(w2, "clear")) c.type = CmdType::Clear;
  else if (!strcmp(w2, "rel")) c.type = CmdType::Reliability;
  else if (!strcmp(w2, "on")) c.type = CmdType::SetOn;
  else if (!strcmp(w2, "off")) c.type = CmdType::SetOff;
  else if (!strcmp(w2, "cal") && w3) {
    c.type = CmdType::Calibrate;
    c.value = (float)atof(w3) * 1000.0f;  // typed in kilograms, the unit the mass is spoken in
  } else if (!strcmp(w2, "bowls") && w3) {
    const int b = atoi(w3);
    if (b < 0 || b > 4) {
      Serial.println("  bowls must be 0..4");
      return;
    }
    c.type = CmdType::SetBowls;
    c.value = (float)b;
  } else if (!strcmp(w2, "ch") && w3) {
    const int ch = atoi(w3);
    if (ch < 0 || ch > 7) {
      Serial.println("  channel must be 0..7");
      return;
    }
    c.type = CmdType::SetCh;
    c.value = (float)ch;
  } else if ((!strcmp(w2, "uid") || !strcmp(w2, "label")) && w3) {
    const bool uid = !strcmp(w2, "uid");
    const size_t max = uid ? sizeof(SlotConfig::uid) - 1 : sizeof(SlotConfig::label) - 1;
    if (strlen(w3) > max) {
      Serial.printf("  %s is at most %u characters\n", w2, (unsigned)max);
      return;
    }
    c.type = uid ? CmdType::SetUid : CmdType::SetLabel;
    snprintf(c.text, sizeof(c.text), "%s", w3);
  } else {
    Serial.println("  :buf N zero | cal <kg> | clear | bowls <n> | rel | uid <id> | label <L> | ch <0-7> | on | off");
    return;
  }
  queueCmd(c);
}

}  // namespace

void begin() {
  // Configuration first, so the boot lines can name each slot.
  Preferences p;
  const bool nvsOk = p.begin(NVS_NS, false);
  const bool imported = nvsOk && importLegacy(p);
  for (uint8_t i = 0; i < SLOTS; i++) {
    Slot &s = slots_[i];
    if (nvsOk) {
      loadConfig(p, i, s.cfg);
      loadPersisted(p, i, s.saved);
    } else {
      s.cfg = DEFAULTS[i];
    }
    snprintf(s.name, sizeof(s.name), "%s %s", s.cfg.label, s.cfg.uid);
  }
  if (nvsOk) p.end();

  // Two slots on one channel would read one platform twice under two ids.
  for (uint8_t i = 0; i < SLOTS; i++)
    for (uint8_t j = 0; j < i; j++)
      if (slots_[i].cfg.enabled && slots_[j].cfg.enabled && slots_[i].cfg.muxCh == slots_[j].cfg.muxCh) {
        Serial.printf("  %s: channel %d already used by %s -- disabled\n", slots_[i].name,
                      slots_[i].cfg.muxCh, slots_[j].name);
        slots_[i].cfg.enabled = false;
      }

  busOk_ = busInit();
  muxOk_ = busOk_ && selectCh(-1);
  if (!muxOk_) {
    // THE NORMAL STATE OF A UNIT WITH NO BUFFER MODULE, so one calm line, no task,
    // no bus traffic. From the bus it is also exactly what a loose lead looks like,
    // so the line says what to check.
    Serial.printf("\n--- buffer bank: none (nothing at 0x%02X on IO%d/IO%d%s) ---\n", board::BUF_MUX_ADDR,
                  board::BUF_SDA, board::BUF_SCL,
                  busOk_ ? "; if a module IS fitted, check its lead, 3V3/GND and address straps" : "; bus init FAILED");
    return;
  }

  Serial.printf("\n--- buffer bank: IO%d/IO%d bit-banged, mux 0x%02X ---\n", board::BUF_SDA, board::BUF_SCL,
                board::BUF_MUX_ADDR);
  if (imported) Serial.println("  B1's zero and factor imported from the field-trial build");

  lscale::FilterConfig fc;
  lscale::BowlConfig bc;
  for (uint8_t i = 0; i < SLOTS; i++) {
    Slot &s = slots_[i];
    if (!s.cfg.enabled || s.cfg.muxCh < 0) {
      Serial.printf("  %s: off\n", s.name);
      continue;
    }
    // A plain bus as far as the driver is concerned (muxChannel -1): this bus's mux
    // is not the counter's i2cmux singleton, and selectCh() above handles it.
    s.cell.configure(PORT, HZ, s.name, -1);
    s.fitted = probe(s);
    if (!s.fitted) {
      Serial.printf("  %s ch%d: nothing answering -- not fitted\n", s.name, s.cfg.muxCh);
      continue;
    }
    fitted_++;
    s.core.configure(lscale::Role::Buffer, fc, bc);
    s.core.restore(s.saved);
    // Read back what restore() accepted: a stored factor outside the believable band
    // is dropped there, and saving that decision keeps NVS honest.
    s.saved = s.core.persisted();
    Serial.printf("  %s ch%d: fitted; zero %s, factor %s, %u bowl%s (unconfirmed until seen)\n", s.name,
                  s.cfg.muxCh, s.saved.zeroed ? "stored" : "not set",
                  s.saved.cpg > 0 ? "stored" : "not set", s.saved.bowls, s.saved.bowls == 1 ? "" : "s");
  }
  selectCh(-1);

  mutex_ = xSemaphoreCreateMutex();
  q_ = xQueueCreate(8, sizeof(Cmd));
  publish();  // a first snapshot before the task exists, so nothing reads defaults
  if (fitted_ == 0) {
    Serial.println("  no platform fitted -- task not started");
    return;
  }
  // Core 1, priority 2: the measurement core, BELOW the counter's scale task (3), so
  // the counter always wins a contention. 5 kB: the snapshot it builds is ~0.5 kB.
  xTaskCreatePinnedToCore(bankTask, "bufbank", 5120, nullptr, 2, &task_, 1);
  Serial.println("  task started on core 1");
}

Snapshot snapshot() {
  Snapshot s;
  if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
    s = published_;
    xSemaphoreGive(mutex_);
  }
  return s;
}

void zero(uint8_t slot) { queueCmd(Cmd{CmdType::Zero, slot, 0, {0}}); }
void calibrate(uint8_t slot, float knownG) { queueCmd(Cmd{CmdType::Calibrate, slot, knownG, {0}}); }
void clearCalibration(uint8_t slot) { queueCmd(Cmd{CmdType::Clear, slot, 0, {0}}); }
void setBowls(uint8_t slot, uint8_t n) { queueCmd(Cmd{CmdType::SetBowls, slot, (float)n, {0}}); }
float defaultCalMassG() { return (float)BOWLSTACK_BUF_CAL_MASS_G; }

uint32_t stackFreeBytes() { return task_ ? (uint32_t)uxTaskGetStackHighWaterMark(task_) : 0; }

void printStatus() {
  const Snapshot sn = snapshot();
  for (uint8_t i = 0; i < SLOTS; i++) {
    const SlotSnapshot &s = sn.slot[i];
    if (!s.fitted) continue;  // said once at boot; every 5 s on every unit is noise
    // ONE WRITE PER LINE: a native-USB write can block ~100 ms with no listener.
    char b[200];
    const lscale::Reading &r = s.r;
    if (r.link != lscale::Link::Online || r.samples == 0) {
      snprintf(b, sizeof(b), "  %-3s %-8s %-7s %s", s.cfg.label, s.cfg.uid,
               r.link == lscale::Link::Offline ? "OFFLINE" : "warming", lscale::wstateToken(r.state));
    } else {
      int n = snprintf(b, sizeof(b), "  %-3s %-8s raw %8ld filt %8ld p-p %5ld %u/s (%u)  %s", s.cfg.label,
                       s.cfg.uid, (long)r.raw, (long)r.counts, (long)r.pp, s.sps, r.samples,
                       lscale::wstateToken(r.state));
      if (r.kgKnown && n > 0 && n < (int)sizeof(b))
        snprintf(b + n, sizeof(b) - n, "  food %.2f kg gross %.2f kg, %u bowl%s%s", r.foodG / 1000.0f,
                 r.grossG / 1000.0f, r.bowls, r.bowls == 1 ? "" : "s", r.bowlsConfirmed ? "" : " ?");
    }
    Serial.println(b);
  }
  if (task_) Serial.printf("  bufbank stack %lu B free\n", (unsigned long)stackFreeBytes());
}

void printHelp() {
  if (!muxOk_) return;  // no module: advertising keys that do nothing would be a claim
  Serial.printf(
      "\n  BUFFER PLATFORMS (B1..B%u on IO21/IO16)\n"
      "  z / g / k  B1 only: store the EMPTY zero / calibrate against %.0f g / clear\n"
      "  :buf               list slots: id, label, channel, fitted, zero, factor, bowls\n"
      "  :buf N zero        store the EMPTY-platform zero (persisted; empties the bowl count)\n"
      "  :buf N cal 20.0    calibrate against a known mass, in kg, on the platform now\n"
      "  :buf N clear       forget the factor (zero kept)\n"
      "  :buf N bowls 3     set the bowl count (0..4) and confirm it\n"
      "  :buf N rel         1000-read reliability test of the bus to that platform\n"
      "  :buf N uid BWL-005 | label B5 | ch 0..7 | on | off    (saved; restart to apply)\n",
      SLOTS, defaultCalMassG());
}

bool consoleKey(int c) {
  if (!muxOk_) return false;
  switch (c) {
    case 'z':
    case 'Z':
      Serial.println("\n> B1: store EMPTY-platform zero");
      zero(0);
      return true;
    case 'g':
    case 'G':
      Serial.printf("\n> B1: calibrate against %.0f g\n", defaultCalMassG());
      calibrate(0, defaultCalMassG());
      return true;
    case 'k':
    case 'K':
      Serial.println("\n> B1: clear calibration");
      clearCalibration(0);
      return true;
    default:
      return false;
  }
}

bool consoleFeed(int c) {
  if (!lineOpen_) {
    if (c != ':') return false;
    lineOpen_ = true;
    lineLen_ = 0;
    line_[lineLen_++] = ':';
    line_[lineLen_] = 0;
    return true;
  }
  if (c == '\r' || c == '\n') {
    lineOpen_ = false;
    runLine();
    return true;
  }
  if (c == 8 || c == 127) {  // backspace
    if (lineLen_ > 1) line_[--lineLen_] = 0;
    return true;
  }
  if (c >= 32 && c < 127 && lineLen_ < sizeof(line_) - 1) {
    line_[lineLen_++] = (char)c;
    line_[lineLen_] = 0;
  }
  return true;
}

}  // namespace bufbank
