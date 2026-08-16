#include "ui_status.h"

namespace ui {
namespace {

const uint32_t C_TEXT = 0xE6EDF3;
const uint32_t C_MUTED = 0x8B949E;
const uint32_t C_DIM = 0x30363D;
const uint32_t C_OK = 0x3FB950;
const uint32_t C_WARN = 0xD29922;
const uint32_t C_FAULT = 0xF85149;
const uint32_t C_RULE = 0x21262D;

// Draws a hairline around each touch target so its real extent is visible.
//
// It outlines the BUTTON OBJECT, which is now also exactly the hit area -- the
// previous version used lv_obj_set_ext_click_area(), and an extended hit test
// cannot be outlined at all, because the extension exists only in hit-testing
// and has no geometry to draw. Sizing the boxes properly instead means what you
// see is what responds.
//
// Set to 0 once the sizes are settled.
#ifndef UI_TOUCH_DEBUG
#define UI_TOUCH_DEBUG 1
#endif

void debugOutline(lv_obj_t *o) {
#if UI_TOUCH_DEBUG
  lv_obj_set_style_border_color(o, lv_color_hex(0x8B949E), LV_PART_MAIN);
  lv_obj_set_style_border_width(o, 1, LV_PART_MAIN);
  lv_obj_set_style_border_opa(o, LV_OPA_60, LV_PART_MAIN);
  lv_obj_set_style_radius(o, 3, LV_PART_MAIN);
#else
  (void)o;
#endif
}

// --- WiFi strength ---------------------------------------------------------
// Four bars, drawn as rectangles. Thresholds are the conventional ones for
// 2.4 GHz: -55 is excellent, -85 is barely associated.
const uint8_t WIFI_BARS = 4;
const int16_t WIFI_BAR_W = 3;
const int16_t WIFI_BAR_GAP = 2;
const int16_t WIFI_BAR_H[WIFI_BARS] = {4, 7, 10, 13};

lv_obj_t *bars_[WIFI_BARS];
lv_obj_t *lblDevice_;
lv_obj_t *lblTime_;
lv_obj_t *battBody_;
lv_obj_t *battFill_;
lv_obj_t *battBolt_;
lv_obj_t *wifiBtn_;
lv_obj_t *battBtn_;
void (*onWifiTap_)(void) = nullptr;
void (*onBattTap_)(void) = nullptr;

void wifiClicked(lv_event_t *) {
  if (onWifiTap_) onWifiTap_();
}
void battClicked(lv_event_t *) {
  if (onBattTap_) onBattTap_();
}

// Cached, so a steady state does no work and invalidates nothing. Without this
// the bar would repaint every frame behind a scope running at full tilt, for a
// clock that changes once a minute.
int8_t lastBars_ = -2;
bool lastWifi_ = false;
uint8_t lastBatt_ = 255;
bool lastCharging_ = false;
bool lastChargingKnown_ = false;
bool lastTimeKnown_ = false;
uint8_t lastHH_ = 255, lastMM_ = 255;
const char *lastId_ = nullptr;

int8_t barsForRssi(const State &s) {
  if (!s.wifiConnected) return -1;   // disconnected: all bars dim
  if (s.wifiRssi == 0) return 0;     // connected, strength not yet known
  if (s.wifiRssi >= -55) return 4;
  if (s.wifiRssi >= -65) return 3;
  if (s.wifiRssi >= -75) return 2;
  if (s.wifiRssi >= -85) return 1;
  return 1;  // associated at all means at least one bar
}

// Battery fill, in FOUR DISCRETE STEPS rather than a percentage.
//
// The device computes a percentage internally and deliberately does not publish
// it -- a resting-voltage estimate moves several points with load, temperature,
// cell age and per-unit ADC calibration. Drawing a smoothly varying bar would
// re-invent exactly the precision the band exists to withhold. Four visible
// steps are honest: the bar has as many positions as the measurement has.
uint8_t battFillPct(Battery b) {
  switch (b) {
    case Battery::Good: return 100;
    case Battery::Medium: return 66;
    case Battery::Low: return 33;
    case Battery::Critical: return 12;
    default: return 0;
  }
}

uint32_t battColor(Battery b) {
  switch (b) {
    case Battery::Good: return C_OK;
    case Battery::Medium: return C_OK;
    case Battery::Low: return C_WARN;
    case Battery::Critical: return C_FAULT;
    default: return C_DIM;
  }
}

lv_obj_t *rect(lv_obj_t *parent, int16_t w, int16_t h, uint32_t color) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(o, 1, LV_PART_MAIN);
  lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

}  // namespace

void buildStatus(lv_obj_t *parent) {
  lv_obj_set_size(parent, LV_PCT(100), STATUS_H);
  lv_obj_set_style_bg_opa(parent, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_pad_all(parent, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_left(parent, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_right(parent, 6, LV_PART_MAIN);
  lv_obj_set_style_radius(parent, 0, LV_PART_MAIN);
  lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

  // A hairline under the bar, so the pages below read as a separate surface
  // and the eye stops looking for the bar to move with them.
  lv_obj_set_style_border_color(parent, lv_color_hex(C_RULE), LV_PART_MAIN);
  lv_obj_set_style_border_width(parent, 1, LV_PART_MAIN);
  lv_obj_set_style_border_side(parent, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);

  lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lblDevice_ = lv_label_create(parent);
  lv_obj_set_style_text_font(lblDevice_, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblDevice_, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(lblDevice_, "BWL-000");

  lblTime_ = lv_label_create(parent);
  lv_obj_set_style_text_font(lblTime_, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblTime_, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_text(lblTime_, "--:--");

  // Right-hand cluster: signal, then battery, the order a phone uses.
  //
  // 72 wide, up from 62. In a space-between row this cluster is pinned to the
  // right edge, so widening it moves its LEFT edge -- and the WiFi bars with it
  // -- 10 px toward the clock, which is the requested shift. The 10 px reappear
  // as gap between the two icons, which is what lets their touch boxes grow
  // without overlapping.
  lv_obj_t *right = lv_obj_create(parent);
  lv_obj_set_size(right, 72, STATUS_H);
  lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(right, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(right, 0, LV_PART_MAIN);
  lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);

  // The bars get their own container so the whole cluster is one TOUCH TARGET.
  // Four 3 px rectangles are not tappable; an 26x24 box around them is. The box
  // is transparent, so it costs nothing visually and everything in usability.
  wifiBtn_ = lv_obj_create(right);
  lv_obj_set_size(wifiBtn_, 30, STATUS_H);
  lv_obj_set_pos(wifiBtn_, 0, 0);
  lv_obj_set_style_bg_opa(wifiBtn_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(wifiBtn_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(wifiBtn_, 0, LV_PART_MAIN);
  lv_obj_remove_flag(wifiBtn_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(wifiBtn_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(wifiBtn_, wifiClicked, LV_EVENT_CLICKED, nullptr);
  debugOutline(wifiBtn_);

  // Bars share a common BASELINE, so they grow upward like a signal meter
  // rather than being centred, which would read as a bar chart of nothing.
  // They are inset within the button rather than flush to it: the box is the
  // touch target and the icon is what you aim at, and the two are allowed to be
  // different sizes.
  const int16_t baseY = 20;
  const int16_t barsW = WIFI_BARS * WIFI_BAR_W + (WIFI_BARS - 1) * WIFI_BAR_GAP;
  const int16_t barsX = (30 - barsW) / 2;
  for (uint8_t i = 0; i < WIFI_BARS; i++) {
    bars_[i] = rect(wifiBtn_, WIFI_BAR_W, WIFI_BAR_H[i], C_DIM);
    lv_obj_set_pos(bars_[i], barsX + i * (WIFI_BAR_W + WIFI_BAR_GAP), baseY - WIFI_BAR_H[i]);
  }

  // Same reasoning as the WiFi cluster: a 26x13 icon plus its 2 px nub is not a
  // touch target, so a transparent box wraps the whole thing.
  battBtn_ = lv_obj_create(right);
  lv_obj_set_size(battBtn_, 36, STATUS_H);
  lv_obj_set_pos(battBtn_, 36, 0);
  lv_obj_set_style_bg_opa(battBtn_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(battBtn_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(battBtn_, 0, LV_PART_MAIN);
  lv_obj_remove_flag(battBtn_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(battBtn_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(battBtn_, battClicked, LV_EVENT_CLICKED, nullptr);
  debugOutline(battBtn_);

  // Battery: body, nub, fill. Phone-shaped because that is the one icon every
  // person already reads correctly without a legend.
  battBody_ = lv_obj_create(battBtn_);
  lv_obj_set_size(battBody_, 26, 13);
  lv_obj_set_pos(battBody_, 3, 7);
  lv_obj_set_style_bg_opa(battBody_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_color(battBody_, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_obj_set_style_border_width(battBody_, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(battBody_, 2, LV_PART_MAIN);
  lv_obj_set_style_pad_all(battBody_, 0, LV_PART_MAIN);
  lv_obj_remove_flag(battBody_, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *nub = rect(battBtn_, 2, 5, C_MUTED);
  lv_obj_set_pos(nub, 29, 11);

  battFill_ = rect(battBody_, 22, 9, C_OK);
  lv_obj_set_pos(battFill_, 1, 1);

  // The charging bolt sits ON the battery, the way a phone draws it, rather
  // than beside it -- so "charging" reads as a state OF the battery and not as
  // one more icon competing for the same 62 px.
  battBolt_ = lv_label_create(battBtn_);
  lv_obj_set_style_text_font(battBolt_, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(battBolt_, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_label_set_text(battBolt_, LV_SYMBOL_CHARGE);
  lv_obj_set_pos(battBolt_, 11, 6);
  lv_obj_add_flag(battBolt_, LV_OBJ_FLAG_HIDDEN);
}

void statusOnWifiTap(void (*cb)(void)) { onWifiTap_ = cb; }
void statusOnBatteryTap(void (*cb)(void)) { onBattTap_ = cb; }

void updateStatus(const State &s) {
  if (s.deviceId != lastId_) {
    lastId_ = s.deviceId;
    lv_label_set_text(lblDevice_, s.deviceId ? s.deviceId : "BWL-000");
  }

  if (s.timeKnown != lastTimeKnown_ || s.hh != lastHH_ || s.mm != lastMM_) {
    lastTimeKnown_ = s.timeKnown;
    lastHH_ = s.hh;
    lastMM_ = s.mm;
    if (s.timeKnown) {
      lv_label_set_text_fmt(lblTime_, "%02u:%02u", s.hh, s.mm);
      lv_obj_set_style_text_color(lblTime_, lv_color_hex(C_TEXT), LV_PART_MAIN);
    } else {
      // Dimmed AND dashed. This device has no RTC, so until something supplies
      // the time this is genuinely unknown -- and "00:00" would read as
      // midnight rather than as ignorance.
      lv_label_set_text(lblTime_, "--:--");
      lv_obj_set_style_text_color(lblTime_, lv_color_hex(C_DIM), LV_PART_MAIN);
    }
  }

  const int8_t n = barsForRssi(s);
  if (n != lastBars_ || s.wifiConnected != lastWifi_) {
    lastBars_ = n;
    lastWifi_ = s.wifiConnected;
    for (uint8_t i = 0; i < WIFI_BARS; i++) {
      uint32_t c = C_DIM;
      if (n > 0 && i < (uint8_t)n) c = C_TEXT;
      if (n < 0) c = C_FAULT;  // disconnected reads as a fault, not as "weak"
      lv_obj_set_style_bg_color(bars_[i], lv_color_hex(c), LV_PART_MAIN);
    }
  }

  const uint8_t bIdx = (uint8_t)s.battery;
  if (bIdx != lastBatt_ || s.charging != lastCharging_ ||
      s.chargingKnown != lastChargingKnown_) {
    lastBatt_ = bIdx;
    lastCharging_ = s.charging;
    lastChargingKnown_ = s.chargingKnown;

    const uint8_t pct = battFillPct(s.battery);
    lv_obj_set_width(battFill_, (int16_t)((22 * pct) / 100));
    lv_obj_set_style_bg_opa(battFill_, pct ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(battFill_, lv_color_hex(battColor(s.battery)), LV_PART_MAIN);

    // Outline goes red on an empty/unknown cell so the icon is not silently a
    // hollow rectangle that reads as "fine, just low".
    lv_obj_set_style_border_color(
        battBody_, lv_color_hex(s.battery == Battery::Unknown ? C_FAULT : C_MUTED),
        LV_PART_MAIN);

    // Shown ONLY when charge state is actually known. On this board it never
    // is -- the ETA6098's STAT pin reaches no GPIO -- so the bolt stays hidden
    // rather than asserting "not charging", which the device cannot see.
    if (s.chargingKnown && s.charging) lv_obj_remove_flag(battBolt_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(battBolt_, LV_OBJ_FLAG_HIDDEN);
  }
}

}  // namespace ui
