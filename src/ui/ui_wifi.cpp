#include "ui_wifi.h"

#include <stdio.h>
#include <string.h>

namespace ui {
namespace {

const uint32_t C_BG = 0x000000;
const uint32_t C_PANEL = 0x161B22;
const uint32_t C_BORDER = 0x30363D;
const uint32_t C_TEXT = 0xE6EDF3;
const uint32_t C_MUTED = 0x8B949E;
const uint32_t C_KEY = 0x21262D;
const uint32_t C_KEY_ACT = 0x1F6F43;
const uint32_t C_OK = 0x3FB950;
const uint32_t C_FAULT = 0xF85149;

char mac_[18] = "00:00:00:00:00:00";
char apSsid_[33] = "Bowlstack-Setup";
char apPass_[65] = "bowlstack";
char connSsid_[33] = "";
char connIp_[16] = "";
int16_t connRssi_ = 0;

Network nets_[WIFI_MAX_NETWORKS];
uint8_t netCount_ = 0;
uint8_t selected_ = 0;

lv_obj_t *viewMain_ = nullptr;
lv_obj_t *viewPass_ = nullptr;
lv_obj_t *lblPassSsid_ = nullptr;
lv_obj_t *taPass_ = nullptr;
lv_obj_t *kb_ = nullptr;
lv_obj_t *btnShift_ = nullptr;
lv_obj_t *body_ = nullptr;
lv_obj_t *lblConn_ = nullptr;
lv_obj_t *lblRssi_ = nullptr;
lv_obj_t *lblMac_ = nullptr;
lv_obj_t *qrSep_ = nullptr;
lv_obj_t *qrBox_ = nullptr;

// Set by the setters, consumed by wifiTick(). Rebuilding rows is cheap at a
// 20 s scan cadence and impossible to get wrong; diffing them would be neither.
bool netsDirty_ = true;
bool statusDirty_ = true;

void (*onClose_)(void) = nullptr;
void (*onJoin_)(const char *, const char *) = nullptr;

// --- the split keyboard ----------------------------------------------------
// FIVE COLUMNS, NOT TEN. A full QWERTY row on a 240 px panel gives ~22 px keys,
// which is roughly half a fingertip -- you cannot reliably hit the one you
// aimed at, and for a passphrase every miss is invisible until the join fails.
//
// So the alphabet is split down the middle the way it already reads -- qwert |
// yuiop -- and the arrow keys move between halves. Each key becomes ~46 px
// wide, which is a real target. The cost is one extra tap when a character is
// on the other half, which is cheap against mistyping.
//
// Case lives on a shift key in the HEADER rather than in the grid, so it does
// not consume one of the twenty slots and cannot be hit by accident mid-word.
bool upper_ = false;
bool symbols_ = false;
bool rightHalf_ = false;

const char *KB_AL[] = {"q", "w", "e", "r", "t", "\n",
                       "a", "s", "d", "f", "g", "\n",
                       "z", "x", "c", "v", "b", "\n",
                       "123", "space", LV_SYMBOL_BACKSPACE, LV_SYMBOL_RIGHT, ""};
const char *KB_AU[] = {"Q", "W", "E", "R", "T", "\n",
                       "A", "S", "D", "F", "G", "\n",
                       "Z", "X", "C", "V", "B", "\n",
                       "123", "space", LV_SYMBOL_BACKSPACE, LV_SYMBOL_RIGHT, ""};
const char *KB_BL[] = {"y", "u", "i", "o", "p", "\n",
                       "h", "j", "k", "l", "m", "\n",
                       "n", ",", ".", "-", "_", "\n",
                       "123", "space", LV_SYMBOL_BACKSPACE, LV_SYMBOL_LEFT, ""};
const char *KB_BU[] = {"Y", "U", "I", "O", "P", "\n",
                       "H", "J", "K", "L", "M", "\n",
                       "N", ",", ".", "-", "_", "\n",
                       "123", "space", LV_SYMBOL_BACKSPACE, LV_SYMBOL_LEFT, ""};
// WPA passphrases are full of these, and a keyboard that cannot produce them is
// one that cannot join half the networks it can see.
const char *KB_SA[] = {"1", "2", "3", "4", "5", "\n",
                       "6", "7", "8", "9", "0", "\n",
                       "@", "#", "$", "%", "&", "\n",
                       "abc", "space", LV_SYMBOL_BACKSPACE, LV_SYMBOL_RIGHT, ""};
const char *KB_SB[] = {"!", "?", "+", "=", "/", "\n",
                       ":", ";", "(", ")", "'", "\n",
                       "*", "[", "]", "~", "^", "\n",
                       "abc", "space", LV_SYMBOL_BACKSPACE, LV_SYMBOL_LEFT, ""};

void refreshKb() {
  const char **map;
  if (symbols_) map = rightHalf_ ? KB_SB : KB_SA;
  else if (upper_) map = rightHalf_ ? KB_BU : KB_AU;
  else map = rightHalf_ ? KB_BL : KB_AL;
  lv_buttonmatrix_set_map(kb_, map);

  if (btnShift_) {
    lv_obj_t *l = lv_obj_get_child(btnShift_, 0);
    if (l) lv_label_set_text(l, upper_ ? "AB" : "ab");
  }
}

void onKey(lv_event_t *e) {
  lv_obj_t *bm = (lv_obj_t *)lv_event_get_target(e);
  const char *txt = lv_buttonmatrix_get_button_text(bm, lv_buttonmatrix_get_selected_button(bm));
  if (!txt || !taPass_) return;

  if (strcmp(txt, LV_SYMBOL_RIGHT) == 0 || strcmp(txt, LV_SYMBOL_LEFT) == 0) {
    rightHalf_ = !rightHalf_;
    refreshKb();
    return;
  }
  if (strcmp(txt, "123") == 0 || strcmp(txt, "abc") == 0) {
    symbols_ = !symbols_;
    rightHalf_ = false;
    refreshKb();
    return;
  }
  if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
    lv_textarea_delete_char(taPass_);
    return;
  }
  if (strcmp(txt, "space") == 0) {
    lv_textarea_add_text(taPass_, " ");
    return;
  }
  lv_textarea_add_text(taPass_, txt);
}

void onShift(lv_event_t *) {
  upper_ = !upper_;
  symbols_ = false;
  refreshKb();
}

// Same thresholds as the status bar, so three bars here and three bars up there
// mean the same thing.
uint8_t barsFor(int16_t rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  return 1;
}

void showPassView(bool show) {
  if (show) {
    lv_obj_add_flag(viewMain_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(viewPass_, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(viewMain_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(viewPass_, LV_OBJ_FLAG_HIDDEN);
  }
}

void onNetworkClicked(lv_event_t *e) {
  const uint32_t idx = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
  if (idx >= netCount_) return;
  selected_ = (uint8_t)idx;
  lv_label_set_text(lblPassSsid_, nets_[selected_].ssid);
  lv_textarea_set_text(taPass_, "");
  upper_ = false;
  symbols_ = false;
  rightHalf_ = false;
  refreshKb();
  showPassView(true);
}

void onJoin(lv_event_t *) {
  if (!taPass_) return;
  const char *pass = lv_textarea_get_text(taPass_);
  // Handed to the platform rather than acted on here. ui_wifi draws and
  // collects; it does not know what a radio is -- the same separation that lets
  // this whole page compile and run in the desktop preview.
  if (onJoin_) onJoin_(nets_[selected_].ssid, pass);
  showPassView(false);
}

void onPassBack(lv_event_t *) { showPassView(false); }
void onClose(lv_event_t *) {
  if (onClose_) onClose_();
}

lv_obj_t *iconButton(lv_obj_t *parent, const char *label, int16_t w, int16_t h,
                     lv_event_cb_t cb) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_radius(b, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_KEY), LV_PART_MAIN);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(l, label);
  lv_obj_center(l);
  return b;
}

lv_obj_t *addNetworkRow(lv_obj_t *parent, uint8_t idx) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, 30);
  lv_obj_set_style_bg_color(row, lv_color_hex(C_PANEL), LV_PART_MAIN);
  lv_obj_set_style_border_color(row, lv_color_hex(C_BORDER), LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(row, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_all(row, 5, LV_PART_MAIN);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_add_event_cb(row, onNetworkClicked, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);

  lv_obj_t *name = lv_label_create(row);
  // 16, below the 18 floor, and legitimately so. An SSID is INFORMATION read
  // deliberately at arm's length while choosing from a list -- not a state
  // glanced at during service -- which is the tier the scale puts at 14/16.
  // It also fits two more networks on screen, which is the point.
  lv_obj_set_style_text_font(name, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(name, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  lv_obj_set_width(name, 155);
  lv_label_set_text(name, nets_[idx].ssid);

  lv_obj_t *meta = lv_label_create(row);
  lv_obj_set_style_text_font(meta, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(meta, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text_fmt(meta, "%s%u/4", nets_[idx].secured ? "*" : "", barsFor(nets_[idx].rssi));
  return row;
}

}  // namespace

void wifiSetMac(const char *mac) {
  if (mac) snprintf(mac_, sizeof(mac_), "%s", mac);
}
void wifiSetSetupAp(const char *ssid, const char *pass) {
  if (ssid) snprintf(apSsid_, sizeof(apSsid_), "%s", ssid);
  if (pass) snprintf(apPass_, sizeof(apPass_), "%s", pass);
}
void wifiSetNetworks(const Network *list, uint8_t count) {
  netCount_ = count > WIFI_MAX_NETWORKS ? WIFI_MAX_NETWORKS : count;
  for (uint8_t i = 0; i < netCount_; i++) nets_[i] = list[i];
  netsDirty_ = true;
}
void wifiSetConnected(const char *ssid, int16_t rssi, const char *ip) {
  const bool changed = (strcmp(connSsid_, ssid ? ssid : "") != 0) ||
                       (strcmp(connIp_, ip ? ip : "") != 0) || (connRssi_ != rssi);
  snprintf(connSsid_, sizeof(connSsid_), "%s", ssid ? ssid : "");
  snprintf(connIp_, sizeof(connIp_), "%s", ip ? ip : "");
  connRssi_ = rssi;
  // Compared before flagging: this is called at 1 Hz with an RSSI that drifts
  // constantly, and rebuilding on every call would undo the page's own change
  // detection.
  if (changed) statusDirty_ = true;
}

void wifiResetView() {
  if (!viewPass_ || !viewMain_) return;
  if (taPass_) lv_textarea_set_text(taPass_, "");
  upper_ = false;
  symbols_ = false;
  rightHalf_ = false;
  showPassView(false);
}

void wifiTick() {
  if (statusDirty_ && lblConn_ && lblRssi_ && lblMac_) {
    statusDirty_ = false;
    if (connSsid_[0]) {
      lv_obj_set_style_text_color(lblConn_, lv_color_hex(C_OK), LV_PART_MAIN);
      lv_label_set_text(lblConn_, connSsid_);
      lv_label_set_text_fmt(lblRssi_, "%d dBm", connRssi_);
      lv_label_set_text(lblMac_, connIp_[0] ? connIp_ : mac_);
    } else {
      lv_obj_set_style_text_color(lblConn_, lv_color_hex(C_FAULT), LV_PART_MAIN);
      lv_label_set_text(lblConn_, "not connected");
      lv_label_set_text(lblRssi_, "");
      lv_label_set_text(lblMac_, mac_);
    }
  }

  if (netsDirty_ && body_) {
    netsDirty_ = false;
    // Delete only the ROWS. The separator and the QR box are children of the
    // same container and must survive, so they are moved to the end rather than
    // rebuilt -- a QR regenerates its whole symbol on creation.
    while (lv_obj_get_child_count(body_) > 0) {
      lv_obj_t *c = lv_obj_get_child(body_, 0);
      if (c == qrSep_ || c == qrBox_) break;
      lv_obj_delete(c);
    }
    for (uint8_t i = 0; i < netCount_; i++) {
      lv_obj_t *row = addNetworkRow(body_, i);
      lv_obj_move_to_index(row, (int32_t)i);
    }
  }
}
void wifiOnClose(void (*cb)(void)) { onClose_ = cb; }
void wifiOnJoin(void (*cb)(const char *, const char *)) { onJoin_ = cb; }

void buildWifiPage(lv_obj_t *parent) {
  lv_obj_set_style_bg_color(parent, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(parent, 0, LV_PART_MAIN);
  lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

  // --- main view -----------------------------------------------------------
  viewMain_ = lv_obj_create(parent);
  lv_obj_set_size(viewMain_, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_opa(viewMain_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(viewMain_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(viewMain_, 5, LV_PART_MAIN);
  lv_obj_set_style_pad_row(viewMain_, 5, LV_PART_MAIN);
  lv_obj_set_flex_flow(viewMain_, LV_FLEX_FLOW_COLUMN);

  // Header carries the title, the DIAGNOSTICS, and the close button on one
  // line. Status and MAC used to be two full-width rows of their own, which
  // cost ~40 px of height and pushed the network list off the bottom -- and
  // they are short strings that were wasting most of that width. Stacked in the
  // middle at 14 they occupy space that was empty anyway.
  lv_obj_t *hdr = lv_obj_create(viewMain_);
  lv_obj_set_size(hdr, LV_PCT(100), 34);
  lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(hdr, 0, LV_PART_MAIN);
  lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  // Left column: the page name with the signal strength directly under it, so
  // the number is read as "the WiFi is -44 dBm" without needing a word to say
  // so. Two rows total across the whole header, which is what leaves the
  // network list its height.
  lv_obj_t *titleCol = lv_obj_create(hdr);
  lv_obj_set_size(titleCol, 60, 34);
  lv_obj_set_style_bg_opa(titleCol, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(titleCol, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(titleCol, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(titleCol, 0, LV_PART_MAIN);
  lv_obj_remove_flag(titleCol, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(titleCol, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(titleCol, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);

  lv_obj_t *title = lv_label_create(titleCol);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_text(title, "WiFi");

  lblRssi_ = lv_label_create(titleCol);
  lv_obj_t *lblRssi = lblRssi_;
  lv_obj_set_style_text_font(lblRssi, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblRssi, lv_color_hex(C_MUTED), LV_PART_MAIN);
  // Blank rather than "0 dBm" when there is no link. An RSSI is a measurement
  // of an association that does not exist, and printing a number for it would
  // be the same mistake as reporting a missing cell as 0%.
  if (connSsid_[0]) lv_label_set_text_fmt(lblRssi, "%d dBm", connRssi_);
  else lv_label_set_text(lblRssi, "");

  lv_obj_t *info = lv_obj_create(hdr);
  lv_obj_set_size(info, 124, 34);
  lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(info, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(info, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(info, 0, LV_PART_MAIN);
  lv_obj_remove_flag(info, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

  lblConn_ = lv_label_create(info);
  lv_obj_t *lblConn = lblConn_;
  lv_obj_set_style_text_font(lblConn, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_long_mode(lblConn, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lblConn, 124);
  lv_obj_set_style_text_align(lblConn, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
  if (connSsid_[0]) {
    lv_obj_set_style_text_color(lblConn, lv_color_hex(C_OK), LV_PART_MAIN);
    lv_label_set_text(lblConn, connSsid_);
  } else {
    lv_obj_set_style_text_color(lblConn, lv_color_hex(C_FAULT), LV_PART_MAIN);
    lv_label_set_text(lblConn, "not connected");
  }

  lblMac_ = lv_label_create(info);
  lv_obj_t *lblMac = lblMac_;
  lv_obj_set_style_text_font(lblMac, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblMac, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_obj_set_width(lblMac, 124);
  lv_obj_set_style_text_align(lblMac, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
  lv_label_set_text(lblMac, mac_);

  iconButton(hdr, LV_SYMBOL_CLOSE, 38, 30, onClose);

  // --- scrollable body: networks first, QR at the end ----------------------
  // Order is a statement about which path is expected. Picking a visible
  // network is the everyday case and now needs no scrolling; the phone-based
  // setup is deliberate enough that scrolling to it is no burden, and it earns
  // a QR big enough to scan from a comfortable distance in return.
  body_ = lv_obj_create(viewMain_);
  lv_obj_t *body = body_;
  lv_obj_set_width(body, LV_PCT(100));
  lv_obj_set_flex_grow(body, 1);
  lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(body, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(body, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(body, 4, LV_PART_MAIN);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  // The one place on this screen that SHOULD scroll, so it says so explicitly
  // rather than relying on LVGL's default -- everything else has the flag
  // removed, and an unmarked exception reads as an oversight.
  lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(body, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

  for (uint8_t i = 0; i < netCount_; i++) addNetworkRow(body, i);

  qrSep_ = lv_label_create(body);
  lv_obj_t *sep = qrSep_;
  lv_obj_set_style_text_font(sep, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(sep, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_obj_set_style_margin_top(sep, 6, LV_PART_MAIN);
  lv_label_set_text(sep, "or set up from your phone:");

  qrBox_ = lv_obj_create(body);
  lv_obj_t *qrBox = qrBox_;
  lv_obj_set_style_bg_color(qrBox, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_border_width(qrBox, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(qrBox, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_all(qrBox, 0, LV_PART_MAIN);
  lv_obj_remove_flag(qrBox, LV_OBJ_FLAG_SCROLLABLE);

  char payload[128];
  snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;", apSsid_, apPass_);
  const uint32_t plen = (uint32_t)strlen(payload);

  // THE BEZEL, from reading lv_qrcode.c rather than guessing at it. Two causes,
  // and my first two attempts fixed neither.
  //
  // 1. lv_qrcode draws each module as a whole number of pixels -- scale =
  //    obj_w / qr_size, integer division -- and centres the result, so up to
  //    modules-1 pixels become blank border. Snapping the size to an exact
  //    multiple removes that.
  //
  // 2. LVGL encodes at qrcodegen_Ecc_MEDIUM, NOT low. That is the one that
  //    caught me: MEDIUM byte-mode capacities are 14/26/42/62/84/106, so a
  //    44-byte payload is version 4 (33 modules), where the LOW table I had
  //    used says version 3 (29). Snapping to a multiple of the wrong number
  //    leaves exactly the margin it was supposed to remove.
  //
  // Version -> modules is 4v + 17, from qrcodegen_version2size().
  uint16_t modules = 21;                  // v1, <= 14 bytes
  if (plen > 106) modules = 41;           // v6
  else if (plen > 84) modules = 37;       // v5
  else if (plen > 62) modules = 37;       // v5 -- 63..84 bytes
  else if (plen > 42) modules = 33;       // v4 -- 43..62 bytes
  else if (plen > 26) modules = 29;       // v3
  else if (plen > 14) modules = 25;       // v2

  // Widest exact multiple that fits, then the BOX is sized to hug it. The
  // previous version left the box at 100% width with a 174 px code centred in
  // it -- 27 px of white on each side that had nothing to do with the symbol,
  // which is most of what was actually visible as a bezel.
  const uint16_t budget = 200;
  const uint16_t scale = budget / modules;
  const uint16_t qrPx = (uint16_t)(modules * scale);

  // Two modules of quiet zone. The spec asks for four; at this scale that would
  // be 48 px and the box would not fit the panel. Phone cameras read two
  // reliably, and the alternative is a symbol small enough that scan distance
  // suffers more than the margin helps.
  const uint16_t quiet = (uint16_t)(scale * 2);
  lv_obj_set_size(qrBox, qrPx + quiet * 2, qrPx + quiet * 2);

  lv_obj_t *qr = lv_qrcode_create(qrBox);
  lv_qrcode_set_size(qr, qrPx);
  lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
  lv_qrcode_set_light_color(qr, lv_color_hex(0xFFFFFF));
  lv_obj_center(qr);
  lv_qrcode_update(qr, payload, plen);

  // The white that REMAINS is the quiet zone, supplied by this box's own white
  // background rather than by the symbol. That part is not optional: it is in
  // the spec and scanners fail without it. Keeping the AP SSID short keeps the
  // module count low, which keeps the modules fat and the scan distance long.

  // --- password view -------------------------------------------------------
  viewPass_ = lv_obj_create(parent);
  lv_obj_set_size(viewPass_, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(viewPass_, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_border_width(viewPass_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(viewPass_, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_row(viewPass_, 4, LV_PART_MAIN);
  lv_obj_set_flex_flow(viewPass_, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(viewPass_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(viewPass_, LV_OBJ_FLAG_HIDDEN);

  // SSID, shift, back -- all on one line, as asked. Shift belongs here rather
  // than in the grid: it does not consume one of the twenty key slots, and it
  // cannot be hit by accident mid-passphrase.
  lv_obj_t *phdr = lv_obj_create(viewPass_);
  lv_obj_set_size(phdr, LV_PCT(100), 28);
  lv_obj_set_style_bg_opa(phdr, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(phdr, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(phdr, 0, LV_PART_MAIN);
  lv_obj_remove_flag(phdr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(phdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(phdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lblPassSsid_ = lv_label_create(phdr);
  lv_obj_set_style_text_font(lblPassSsid_, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblPassSsid_, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_obj_set_width(lblPassSsid_, 140);
  lv_label_set_long_mode(lblPassSsid_, LV_LABEL_LONG_DOT);
  lv_label_set_text(lblPassSsid_, "-");

  btnShift_ = iconButton(phdr, "ab", 40, 28, onShift);
  iconButton(phdr, LV_SYMBOL_LEFT, 38, 28, onPassBack);

  // Password field and JOIN on one line. The OK key was lost in the split-
  // keyboard rewrite -- you could type a passphrase and had no way to submit it
  // -- and putting it back in the grid would have cost one of the twenty key
  // slots. Beside the field it reads the way a search box with a Go button
  // does, and the row is shorter than the field alone used to be.
  lv_obj_t *entry = lv_obj_create(viewPass_);
  lv_obj_set_size(entry, LV_PCT(100), 30);
  lv_obj_set_style_bg_opa(entry, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(entry, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(entry, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(entry, 4, LV_PART_MAIN);
  lv_obj_remove_flag(entry, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(entry, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(entry, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  taPass_ = lv_textarea_create(entry);
  lv_obj_set_flex_grow(taPass_, 1);
  lv_obj_set_height(taPass_, 30);
  lv_obj_set_style_pad_all(taPass_, 3, LV_PART_MAIN);
  lv_textarea_set_one_line(taPass_, true);
  lv_textarea_set_placeholder_text(taPass_, "password");
  // NOT masked, deliberately. A passphrase typed one character at a time on a
  // small panel with no tactile feedback is mistyped constantly, and masking
  // turns every mistake into "retype the whole thing". The threat model here is
  // a kitchen, not a shared terminal.
  lv_obj_set_style_text_font(taPass_, &lv_font_montserrat_16, LV_PART_MAIN);

  lv_obj_t *btnJoin = lv_button_create(entry);
  lv_obj_set_size(btnJoin, 46, 30);
  lv_obj_set_style_radius(btnJoin, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(btnJoin, lv_color_hex(C_KEY_ACT), LV_PART_MAIN);
  lv_obj_add_event_cb(btnJoin, onJoin, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *jl = lv_label_create(btnJoin);
  // "OK", not "Join". At 56 px next to the field, "Join" either wraps or gets
  // ellipsised, and a truncated verb on a button is worse than a shorter one:
  // the point of the key is that it is unmistakably the submit action, and OK
  // carries that at any width.
  lv_obj_set_style_text_font(jl, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_label_set_text(jl, "OK");
  lv_obj_center(jl);

  kb_ = lv_buttonmatrix_create(viewPass_);
  lv_obj_set_width(kb_, LV_PCT(100));
  lv_obj_set_flex_grow(kb_, 1);
  lv_obj_set_style_bg_opa(kb_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(kb_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(kb_, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(kb_, lv_color_hex(C_KEY), LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(kb_, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_text_color(kb_, lv_color_hex(C_TEXT), LV_PART_ITEMS);
  lv_obj_set_style_text_font(kb_, &lv_font_montserrat_20, LV_PART_ITEMS);
  lv_obj_set_style_radius(kb_, 4, LV_PART_ITEMS);
  lv_obj_set_style_pad_all(kb_, 3, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(kb_, lv_color_hex(C_KEY_ACT),
                            (lv_style_selector_t)(LV_PART_ITEMS | LV_STATE_PRESSED));
  lv_obj_add_event_cb(kb_, onKey, LV_EVENT_VALUE_CHANGED, nullptr);
  refreshKb();
}

}  // namespace ui
