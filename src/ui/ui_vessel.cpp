// See include/ui_vessel.h for why this is its own page and not a mode on the
// calibrate keypad.

#include "ui_vessel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_screens.h"

namespace ui {
namespace {

const uint32_t C_BG = 0x000000;
const uint32_t C_TEXT = 0xE6EDF3;
const uint32_t C_MUTED = 0x8B949E;
const uint32_t C_PANEL = 0x161B22;
const uint32_t C_BORDER = 0x30363D;
const uint32_t C_KEY = 0x21262D;
const uint32_t C_OK = 0x1F6F43;

lv_obj_t *lblEntry_ = nullptr;

void (*onClose_)(void) = nullptr;
void (*onApply_)(float) = nullptr;

// KILOGRAMS AS A TYPED STRING, not a float being accumulated. Same reasoning as
// the calibrate page: the person is TELLING the device a number, not measuring
// one, so it should survive to the conversion exactly as typed. The single
// atof() at the end is the only floating point in the path.
char entry_[8] = "";
const uint8_t ENTRY_MAX = 5;  // "99.99" -- no serving vessel is 100 kg

// True while the field still holds a prefill nobody has edited, so the first
// digit REPLACES it rather than appending to it. Adjusting 2.5 to 1.1 by typing
// is otherwise impossible without three backspaces first.
bool fresh_ = true;

char prevEntry_[16] = {0};

void refreshEntry() {
  char buf[16];
  snprintf(buf, sizeof(buf), "%s", entry_[0] ? entry_ : "0");
  if (strcmp(prevEntry_, buf) != 0) {
    snprintf(prevEntry_, sizeof(prevEntry_), "%s", buf);
    lv_label_set_text(lblEntry_, buf);
    lv_obj_set_style_text_color(lblEntry_,
                                lv_color_hex(entry_[0] ? C_TEXT : C_MUTED),
                                LV_PART_MAIN);
  }
}

void onDigit(lv_event_t *e) {
  const char d = (char)(uintptr_t)lv_event_get_user_data(e);
  if (fresh_) {
    entry_[0] = '\0';
    fresh_ = false;
  }
  const size_t n = strlen(entry_);
  if (n >= ENTRY_MAX) return;
  // ONE DECIMAL POINT, and not as the first character. "2.5.1" is not a number
  // and atof would silently read it as 2.5, which is a wrong offset rather than
  // a rejected one.
  if (d == '.' && (n == 0 || strchr(entry_, '.'))) return;
  entry_[n] = d;
  entry_[n + 1] = '\0';
  refreshEntry();
}

void onBack(lv_event_t *) {
  // Backspace is EDITING, so it takes ownership of the prefill rather than
  // wiping it -- shortening 2.5 to 2 and typing .6 is a legitimate way to
  // reach 2.6.
  fresh_ = false;
  const size_t n = strlen(entry_);
  if (n) entry_[n - 1] = '\0';
  refreshEntry();
}

void onOk(lv_event_t *) {
  // An empty field is zero, which is off. Not an error: clearing the box is the
  // obvious way to switch the offset off and it should do exactly that.
  const float kg = entry_[0] ? (float)atof(entry_) : 0.0f;
  if (kg < 0.0f) return;
  if (onApply_) onApply_(kg * 1000.0f);
  if (onClose_) onClose_();
}

void onCloseClicked(lv_event_t *) {
  if (onClose_) onClose_();
}

lv_obj_t *key(lv_obj_t *parent, const char *label, lv_event_cb_t cb, void *user,
              uint32_t colour) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, 70, 40);
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

void vesselOnClose(void (*cb)(void)) { onClose_ = cb; }
void vesselOnApply(void (*cb)(float)) { onApply_ = cb; }

void vesselSetOffset(float grams) {
  if (grams > 0.0f) {
    // One decimal, because that is the resolution the keypad offers and showing
    // 2.50 would invite a precision the entry cannot express.
    snprintf(entry_, sizeof(entry_), "%.1f", grams / 1000.0f);
  } else {
    entry_[0] = '\0';
  }
  fresh_ = true;
  // Guarded, because this runs from showOnly() on every entry to the page --
  // including the first, which happens before the label exists.
  if (lblEntry_) refreshEntry();
}

void buildVesselPage(lv_obj_t *parent) {
  lv_obj_set_style_bg_color(parent, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN);
  // NO EXPLANATORY LINE ON THIS PAGE, and its absence is the point.
  //
  // There was one -- "0 = no offset; the platform reads its real mass" -- set
  // to LV_LABEL_LONG_WRAP. At 14 px on a 232 px page that is TWO lines, 36 px
  // rather than the 18 the budget below was written against, and the eighteen
  // px it overran came off the bottom of the keypad. What falls off a
  // ROW_WRAP grid is its LAST child, which here is the OK key: the page still
  // looked fine and could not be completed.
  //
  // The calibrate page's own comment says exactly this -- "flex does not
  // complain about that; it just draws what fits" -- and this page was written
  // from that one. Measured now rather than budgeted:
  //
  //   4 + 26 + 2 + 36 + 2 + (4*40 + 3*4) + 4  =  246  of 294
  lv_obj_set_style_pad_all(parent, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_row(parent, 2, LV_PART_MAIN);
  lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

  // --- header --------------------------------------------------------------
  lv_obj_t *hdr = lv_obj_create(parent);
  lv_obj_set_size(hdr, LV_PCT(100), 26);
  lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(hdr, 0, LV_PART_MAIN);
  lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lv_obj_t *t = lv_label_create(hdr);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_text(t, "Vessel offset");

  lv_obj_t *b = lv_button_create(hdr);
  lv_obj_set_size(b, 42, 26);
  lv_obj_set_style_radius(b, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_KEY), LV_PART_MAIN);
  lv_obj_add_event_cb(b, onCloseClicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *bl = lv_label_create(b);
  lv_label_set_text(bl, LV_SYMBOL_LEFT);
  lv_obj_center(bl);

  // --- the entered mass ----------------------------------------------------
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
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 4, LV_PART_MAIN);

  lblEntry_ = lv_label_create(row);
  lv_obj_set_style_text_font(lblEntry_, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblEntry_, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(lblEntry_, "0");

  lv_obj_t *u = lv_label_create(row);
  lv_obj_set_style_text_font(u, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(u, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(u, "kg");

  // --- the keypad ----------------------------------------------------------
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
  lv_obj_set_flex_align(pad, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  static const char *const DIGITS[] = {"1", "2", "3", "4", "5",
                                       "6", "7", "8", "9", "0"};
  for (uint8_t i = 0; i < 9; i++)
    key(pad, DIGITS[i], onDigit, (void *)(uintptr_t)DIGITS[i][0], C_KEY);

  // THE DECIMAL POINT IS WHY THIS PAGE EXISTS. It sits where a phone keypad
  // puts it, left of the zero, so the hand goes to it without looking.
  key(pad, ".", onDigit, (void *)(uintptr_t)'.', C_KEY);
  key(pad, "0", onDigit, (void *)(uintptr_t)'0', C_KEY);
  key(pad, LV_SYMBOL_BACKSPACE, onBack, nullptr, C_KEY);
  key(pad, LV_SYMBOL_OK, onOk, nullptr, C_OK);

  refreshEntry();
}

}  // namespace ui
