// See include/ui_bufcal.h for why this is its own page.

#include "ui_bufcal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ui {
namespace {

const uint32_t C_BG = 0x000000;
const uint32_t C_TEXT = 0xE6EDF3;
const uint32_t C_MUTED = 0x8B949E;
const uint32_t C_PANEL = 0x161B22;
const uint32_t C_BORDER = 0x30363D;
const uint32_t C_KEY = 0x21262D;
const uint32_t C_OK = 0x1F6F43;

lv_obj_t *lblTitle_ = nullptr;
lv_obj_t *lblEntry_ = nullptr;

void (*onClose_)(void) = nullptr;
void (*onApply_)(uint8_t, float) = nullptr;
uint8_t slot_ = 0;

// KILOGRAMS AS A TYPED STRING, converted once at OK -- the person is telling the
// device a number, so it survives exactly as typed until then.
char entry_[8] = "";
const uint8_t ENTRY_MAX = 6;  // "200.00"
char prevEntry_[16] = {0};

void refreshEntry() {
  char buf[16];
  snprintf(buf, sizeof(buf), "%s", entry_[0] ? entry_ : "0");
  if (strcmp(prevEntry_, buf) != 0) {
    snprintf(prevEntry_, sizeof(prevEntry_), "%s", buf);
    lv_label_set_text(lblEntry_, buf);
    lv_obj_set_style_text_color(lblEntry_, lv_color_hex(entry_[0] ? C_TEXT : C_MUTED),
                                LV_PART_MAIN);
  }
}

void onDigit(lv_event_t *e) {
  const char d = (char)(uintptr_t)lv_event_get_user_data(e);
  const size_t n = strlen(entry_);
  if (n >= ENTRY_MAX) return;
  // ONE decimal point, and not first: "12.5.1" is not a number and atof would read
  // it as 12.5 -- a wrong mass rather than a refused one.
  if (d == '.' && (n == 0 || strchr(entry_, '.'))) return;
  entry_[n] = d;
  entry_[n + 1] = '\0';
  refreshEntry();
}

void onBack(lv_event_t *) {
  const size_t n = strlen(entry_);
  if (n) entry_[n - 1] = '\0';
  refreshEntry();
}

void onOk(lv_event_t *) {
  // AN EMPTY FIELD DOES NOTHING. Unlike the vessel page, where empty means "off",
  // there is no meaning to calibrating against nothing -- and the firmware would
  // refuse it anyway, so saying nothing here is the honest outcome.
  if (!entry_[0]) return;
  const float kg = (float)atof(entry_);
  if (!(kg > 0.0f)) return;
  if (onApply_) onApply_(slot_, kg * 1000.0f);
  if (onClose_) onClose_();
}

void onCloseClicked(lv_event_t *) {
  if (onClose_) onClose_();
}

lv_obj_t *key(lv_obj_t *parent, const char *label, lv_event_cb_t cb, void *user,
              uint32_t colour) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, 70, 36);  // see the budget in buildBufCalPage()
  lv_obj_set_style_radius(b, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(b, lv_color_hex(colour), LV_PART_MAIN);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_text(l, label);
  lv_obj_center(l);
  return b;
}

}  // namespace

void bufCalOnClose(void (*cb)(void)) { onClose_ = cb; }
void bufCalOnApply(void (*cb)(uint8_t, float)) { onApply_ = cb; }

void bufCalOpenFor(uint8_t slot, const char *label) {
  slot_ = slot;
  entry_[0] = '\0';
  // Guarded: this runs on every entry, including before the page is first built.
  if (lblTitle_) {
    char t[24];
    snprintf(t, sizeof(t), "Calibrate %s", label ? label : "");
    lv_label_set_text(lblTitle_, t);
  }
  if (lblEntry_) refreshEntry();
}

void buildBufCalPage(lv_obj_t *parent) {
  lv_obj_set_style_bg_color(parent, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN);
  // THE BUDGET, counted from the keys rather than assumed. Thirteen keys at three a
  // row is FIVE rows, not four -- and this page adds one 18 px line the vessel page
  // does not have, saying what to do, because the order matters (empty zero FIRST,
  // then the mass ON). At the vessel page's 40 px keys that came to 310 of 294, and
  // what falls off a ROW_WRAP keypad is its LAST child: the OK key. At 36 px:
  //
  //   4 + 26 + 2 + 18 + 2 + 36 + 2 + (5*36 + 4*4) + 4  =  290  of 294
  //
  // The hint is CLIP for the same reason: wrapped, it would be two lines and the OK
  // key would go again.
  lv_obj_set_style_pad_all(parent, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_row(parent, 2, LV_PART_MAIN);
  lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *hdr = lv_obj_create(parent);
  lv_obj_set_size(hdr, LV_PCT(100), 26);
  lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(hdr, 0, LV_PART_MAIN);
  lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lblTitle_ = lv_label_create(hdr);
  lv_obj_set_style_text_font(lblTitle_, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblTitle_, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_text(lblTitle_, "Calibrate");

  lv_obj_t *b = lv_button_create(hdr);
  lv_obj_set_size(b, 42, 26);
  lv_obj_set_style_radius(b, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_KEY), LV_PART_MAIN);
  lv_obj_add_event_cb(b, onCloseClicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *bl = lv_label_create(b);
  lv_label_set_text(bl, LV_SYMBOL_LEFT);
  lv_obj_center(bl);

  lv_obj_t *hint = lv_label_create(parent);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(hint, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_obj_set_width(hint, LV_PCT(100));
  lv_label_set_long_mode(hint, LV_LABEL_LONG_CLIP);
  // 31 characters: the longest that fits 232 px at 14 px (the screenshot of the
  // first wording, at 41, ended "...then put the").
  lv_label_set_text(hint, "Zero it empty, then put mass on");

  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_set_size(row, LV_PCT(100), 36);
  lv_obj_set_style_bg_color(row, lv_color_hex(C_PANEL), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(row, lv_color_hex(C_BORDER), LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_left(row, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_right(row, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(row, 0, LV_PART_MAIN);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 4, LV_PART_MAIN);

  lblEntry_ = lv_label_create(row);
  lv_obj_set_style_text_font(lblEntry_, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblEntry_, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(lblEntry_, "0");

  lv_obj_t *u = lv_label_create(row);
  lv_obj_set_style_text_font(u, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(u, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(u, "kg");

  lv_obj_t *pad = lv_obj_create(parent);
  lv_obj_set_width(pad, LV_PCT(100));
  lv_obj_set_flex_grow(pad, 1);
  lv_obj_set_style_bg_opa(pad, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(pad, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(pad, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(pad, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_column(pad, 4, LV_PART_MAIN);
  lv_obj_remove_flag(pad, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(pad, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(pad, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  static const char *const DIGITS[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9"};
  for (uint8_t i = 0; i < 9; i++) key(pad, DIGITS[i], onDigit, (void *)(uintptr_t)DIGITS[i][0], C_KEY);
  // The decimal point left of the zero, where a phone keypad puts it.
  key(pad, ".", onDigit, (void *)(uintptr_t)'.', C_KEY);
  key(pad, "0", onDigit, (void *)(uintptr_t)'0', C_KEY);
  key(pad, LV_SYMBOL_BACKSPACE, onBack, nullptr, C_KEY);
  key(pad, LV_SYMBOL_OK, onOk, nullptr, C_OK);

  refreshEntry();
}

}  // namespace ui
