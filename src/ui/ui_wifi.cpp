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

lv_obj_t *root_ = nullptr;
lv_obj_t *viewMain_ = nullptr;
lv_obj_t *viewPass_ = nullptr;
lv_obj_t *lblStatus_ = nullptr;
lv_obj_t *lblPassSsid_ = nullptr;
lv_obj_t *taPass_ = nullptr;
lv_obj_t *list_ = nullptr;

void (*onClose_)(void) = nullptr;

// Same thresholds as the status bar, so a network showing three bars here and
// three bars up there means the same thing.
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

  lv_label_set_text_fmt(lblPassSsid_, "%s", nets_[selected_].ssid);
  lv_textarea_set_text(taPass_, "");
  showPassView(true);
}

void onPassBack(lv_event_t *) { showPassView(false); }

void onClose(lv_event_t *) {
  if (onClose_) onClose_();
}

void addNetworkRow(lv_obj_t *parent, uint8_t idx) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, 38);
  lv_obj_set_style_bg_color(row, lv_color_hex(C_PANEL), LV_PART_MAIN);
  lv_obj_set_style_border_color(row, lv_color_hex(C_BORDER), LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(row, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_all(row, 6, LV_PART_MAIN);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_add_event_cb(row, onNetworkClicked, LV_EVENT_CLICKED,
                      (void *)(uintptr_t)idx);

  lv_obj_t *name = lv_label_create(row);
  lv_obj_set_style_text_font(name, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(name, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  lv_obj_set_width(name, 150);
  lv_label_set_text(name, nets_[idx].ssid);

  lv_obj_t *meta = lv_label_create(row);
  lv_obj_set_style_text_font(meta, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(meta, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text_fmt(meta, "%s%u/4", nets_[idx].secured ? LV_SYMBOL_EYE_CLOSE " " : "",
                        barsFor(nets_[idx].rssi));
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
}
void wifiSetConnected(const char *ssid, int16_t rssi, const char *ip) {
  snprintf(connSsid_, sizeof(connSsid_), "%s", ssid ? ssid : "");
  snprintf(connIp_, sizeof(connIp_), "%s", ip ? ip : "");
  connRssi_ = rssi;
}
void wifiOnClose(void (*cb)(void)) { onClose_ = cb; }

void buildWifiPage(lv_obj_t *parent) {
  root_ = parent;
  lv_obj_set_style_bg_color(parent, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(parent, 0, LV_PART_MAIN);
  lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

  // --- main view -----------------------------------------------------------
  viewMain_ = lv_obj_create(parent);
  lv_obj_set_size(viewMain_, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_opa(viewMain_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(viewMain_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(viewMain_, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_row(viewMain_, 5, LV_PART_MAIN);
  lv_obj_set_flex_flow(viewMain_, LV_FLEX_FLOW_COLUMN);

  lv_obj_t *hdr = lv_obj_create(viewMain_);
  lv_obj_set_size(hdr, LV_PCT(100), 30);
  lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(hdr, 0, LV_PART_MAIN);
  lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lv_obj_t *title = lv_label_create(hdr);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_text(title, "WiFi");

  lv_obj_t *back = lv_button_create(hdr);
  lv_obj_set_size(back, 44, 28);
  lv_obj_add_event_cb(back, onClose, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *bl = lv_label_create(back);
  lv_label_set_text(bl, LV_SYMBOL_CLOSE);
  lv_obj_center(bl);

  lblStatus_ = lv_label_create(viewMain_);
  lv_obj_set_style_text_font(lblStatus_, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_width(lblStatus_, LV_PCT(100));
  lv_label_set_long_mode(lblStatus_, LV_LABEL_LONG_DOT);
  if (connSsid_[0]) {
    lv_obj_set_style_text_color(lblStatus_, lv_color_hex(C_OK), LV_PART_MAIN);
    lv_label_set_text_fmt(lblStatus_, "%s  %d dBm", connSsid_, connRssi_);
  } else {
    lv_obj_set_style_text_color(lblStatus_, lv_color_hex(C_FAULT), LV_PART_MAIN);
    lv_label_set_text(lblStatus_, "not connected");
  }

  // MAC in the information tier, as asked -- it is read up close during
  // diagnostics, never glanced at, and it is what identifies this board to a
  // network admin. Paired with the IP because the two questions ("did it get on
  // the network" / "which box is it") are always asked together.
  lv_obj_t *lblMac = lv_label_create(viewMain_);
  lv_obj_set_style_text_font(lblMac, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblMac, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_obj_set_width(lblMac, LV_PCT(100));
  if (connIp_[0]) lv_label_set_text_fmt(lblMac, "mac %s\nip  %s", mac_, connIp_);
  else lv_label_set_text_fmt(lblMac, "mac %s", mac_);

  // --- the QR, which is the point of the page -----------------------------
  lv_obj_t *qrBox = lv_obj_create(viewMain_);
  lv_obj_set_size(qrBox, LV_PCT(100), 118);
  lv_obj_set_style_bg_color(qrBox, lv_color_hex(C_PANEL), LV_PART_MAIN);
  lv_obj_set_style_border_color(qrBox, lv_color_hex(C_BORDER), LV_PART_MAIN);
  lv_obj_set_style_border_width(qrBox, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(qrBox, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_all(qrBox, 5, LV_PART_MAIN);
  lv_obj_remove_flag(qrBox, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(qrBox, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(qrBox, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(qrBox, 8, LV_PART_MAIN);

  lv_obj_t *qr = lv_qrcode_create(qrBox);
  lv_qrcode_set_size(qr, 104);
  // Light module on dark background is inverted from the usual, and scanners
  // handle it -- but the QUIET ZONE is not optional, so the light square is
  // drawn full-bleed behind the code by LVGL itself.
  lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
  lv_qrcode_set_light_color(qr, lv_color_hex(0xFFFFFF));

  // The standard join-this-network payload. Both iOS and Android camera apps
  // recognise it with no app installed, which is the entire reason this beats
  // any keyboard we could draw.
  char payload[128];
  snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;", apSsid_, apPass_);
  lv_qrcode_update(qr, payload, (uint32_t)strlen(payload));

  lv_obj_t *qrText = lv_label_create(qrBox);
  lv_obj_set_style_text_font(qrText, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(qrText, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_obj_set_width(qrText, 100);
  lv_label_set_long_mode(qrText, LV_LABEL_LONG_WRAP);
  lv_label_set_text(qrText, "Scan to set up from your phone, then follow the page that opens.");

  lv_obj_t *hint = lv_label_create(viewMain_);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(hint, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(hint, "or pick a network to type here:");

  list_ = lv_obj_create(viewMain_);
  lv_obj_set_width(list_, LV_PCT(100));
  lv_obj_set_flex_grow(list_, 1);
  lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(list_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(list_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(list_, 4, LV_PART_MAIN);
  lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
  for (uint8_t i = 0; i < netCount_; i++) addNetworkRow(list_, i);

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

  lv_obj_t *phdr = lv_obj_create(viewPass_);
  lv_obj_set_size(phdr, LV_PCT(100), 26);
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
  lv_obj_set_width(lblPassSsid_, 170);
  lv_label_set_long_mode(lblPassSsid_, LV_LABEL_LONG_DOT);
  lv_label_set_text(lblPassSsid_, "-");

  lv_obj_t *pback = lv_button_create(phdr);
  lv_obj_set_size(pback, 44, 26);
  lv_obj_add_event_cb(pback, onPassBack, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *pbl = lv_label_create(pback);
  lv_label_set_text(pbl, LV_SYMBOL_LEFT);
  lv_obj_center(pbl);

  taPass_ = lv_textarea_create(viewPass_);
  lv_obj_set_width(taPass_, LV_PCT(100));
  lv_textarea_set_one_line(taPass_, true);
  lv_textarea_set_placeholder_text(taPass_, "password");
  // NOT password-masked, deliberately. A passphrase typed one character at a
  // time on a small panel with no tactile feedback is mistyped constantly, and
  // a masked field turns every mistake into "retype the whole thing". The
  // threat model here is a kitchen, not a shared terminal.
  lv_obj_set_style_text_font(taPass_, &lv_font_montserrat_18, LV_PART_MAIN);

  lv_obj_t *kb = lv_keyboard_create(viewPass_);
  lv_obj_set_width(kb, LV_PCT(100));
  lv_obj_set_flex_grow(kb, 1);
  lv_keyboard_set_textarea(kb, taPass_);
  // POPOVERS are what make a ~22 px key usable: the pressed key is echoed in a
  // magnified bubble above the finger, so you see what you actually hit rather
  // than what you aimed at. It is the trick every phone keyboard uses, and for
  // a random passphrase it beats any cleverer layout.
  lv_keyboard_set_popovers(kb, true);
  lv_obj_set_style_text_font(kb, &lv_font_montserrat_16, LV_PART_ITEMS);
}

}  // namespace ui
