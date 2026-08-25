#include "ui_calib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ui {
namespace {

const uint32_t C_BG = 0x000000;
const uint32_t C_PANEL = 0x161B22;
const uint32_t C_BORDER = 0x30363D;
const uint32_t C_TEXT = 0xE6EDF3;
const uint32_t C_MUTED = 0x8B949E;
const uint32_t C_KEY = 0x21262D;
const uint32_t C_OK = 0x1F6F43;
const uint32_t C_FAULT = 0xF85149;

lv_obj_t *lblEntry_ = nullptr;
lv_obj_t *lblLive_ = nullptr;
lv_obj_t *lblResult_ = nullptr;

void (*onClose_)(void) = nullptr;
void (*onApply_)(float) = nullptr;

// GRAMS AS AN INTEGER STRING, not a float being accumulated. Typing digits into
// a float and multiplying by ten each time accumulates binary error on a value
// that is by definition exact -- the person is telling the device a number, not
// measuring one. Whole grams, because the reference masses anybody owns are
// whole grams and the deflection guard needs far more than a gram to pass.
char entry_[8] = "";
const uint8_t ENTRY_MAX = 6;  // 999999 g is a tonne; nothing here weighs more

// True while the field still holds a prefill nobody has edited. See onDigit().
bool fresh_ = true;

char prevEntry_[16] = {0};
char prevLive_[64] = {0};
uint32_t nextLiveMs_ = 0;

void refreshEntry() {
  char buf[16];
  if (entry_[0] == '\0') snprintf(buf, sizeof(buf), "---");
  else snprintf(buf, sizeof(buf), "%s", entry_);
  if (strcmp(prevEntry_, buf) == 0) return;
  snprintf(prevEntry_, sizeof(prevEntry_), "%s", buf);
  lv_label_set_text(lblEntry_, buf);
  lv_obj_set_style_text_color(lblEntry_,
                              lv_color_hex(entry_[0] ? C_TEXT : C_MUTED), LV_PART_MAIN);
}

void clearResult() {
  if (!lblResult_) return;
  lv_label_set_text(lblResult_, "");
}

void onDigit(lv_event_t *e) {
  const char d = (char)(uintptr_t)lv_event_get_user_data(e);

  // THE FIRST DIGIT AFTER ARRIVING REPLACES THE PREFILL. It used to append, and
  // that is the single worst bug this page had: the field opens showing the last
  // mass used -- 175 -- so an operator with a 500 g weight taps 5, 0, 0 and the
  // field reads 175500. Six characters is exactly ENTRY_MAX, so nothing
  // truncates and nothing complains, and the derived factor is a THOUSANDTH of
  // the right one. Every guard passes; the tick goes green.
  //
  // It compounds too: 175500 is then persisted as the mass, so the next visit
  // opens with six digits already in the field and every further keypress is
  // silently ignored for being at the limit.
  //
  // Editing still works -- backspace clears the flag, so you can shorten the
  // prefill and keep typing when that is what you meant.
  if (fresh_) {
    fresh_ = false;
    entry_[0] = 0;
  }

  // Cleared BEFORE the length check, so a tap that is then ignored for being at
  // the limit still takes the previous verdict off the screen. Leaving it up
  // makes a dead key look like a key that re-confirmed something.
  clearResult();

  const size_t n = strlen(entry_);
  // A leading zero is dropped rather than accepted: "0500" is not a number
  // anybody means to type, and allowing it costs a digit of the six.
  if (n == 1 && entry_[0] == '0') entry_[0] = 0;
  if (strlen(entry_) >= ENTRY_MAX) {
    refreshEntry();
    return;
  }
  const size_t m = strlen(entry_);
  entry_[m] = d;
  entry_[m + 1] = 0;
  refreshEntry();
}

void onBack(lv_event_t *) {
  // Backspace is EDITING, so it takes ownership of the prefill rather than
  // wiping it. Shortening 175 to 17 and typing 5 is a legitimate way to reach
  // 175, and clearing on the first backspace would make that impossible.
  fresh_ = false;
  const size_t n = strlen(entry_);
  if (n) entry_[n - 1] = '\0';
  clearResult();
  refreshEntry();
}

void onOk(lv_event_t *) {
  if (entry_[0] == '\0') {
    calibSetResult("type the mass first", false);
    return;
  }
  const float g = (float)atol(entry_);
  if (onApply_) onApply_(g);
  // No result is set here on success. The firmware answers through
  // calibSetResult(), because only it knows whether the deflection was big
  // enough -- and a page that congratulated itself before hearing back would be
  // reporting the tap rather than the calibration.
}

void onCloseClicked(lv_event_t *) {
  if (onClose_) onClose_();
}

lv_obj_t *key(lv_obj_t *parent, const char *label, lv_event_cb_t cb, void *user,
              uint32_t colour) {
  lv_obj_t *b = lv_button_create(parent);
  // 40 rather than the 44 the menu rows settled on, and 70 wide against a 44 px
  // row's full width. The smaller dimension is what a finger misses on, and 40
  // is close enough to the target while being what lets all four rows fit -- a
  // key that is off the bottom of the screen has a hit rate of zero.
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

void calibOnClose(void (*cb)(void)) { onClose_ = cb; }
void calibOnApply(void (*cb)(float)) { onApply_ = cb; }

void calibSetMass(float grams) {
  // BOUNDED, because this arrives from NVS and a corrupt or absurd stored float
  // would otherwise be printed straight into a six-character buffer. Anything
  // outside what a person could put on this platform becomes no prefill at all,
  // which is a safe state: the field reads "---" and OK refuses until something
  // is typed.
  const long g = (grams > 0.0f && grams < 1000000.0f) ? (long)(grams + 0.5f) : 0;
  if (g <= 0 || g > 999999L) entry_[0] = '\0';
  else snprintf(entry_, sizeof(entry_), "%ld", g);
  // A fresh prefill, so the next digit replaces it rather than appending.
  fresh_ = true;
  if (lblEntry_) refreshEntry();
  // AND THE VERDICT GOES WITH IT. This is called on every entry to the page, so
  // without it a green "ok -- 104.331 counts/g" from ten minutes ago is still
  // sitting under a field that has since been re-prefilled -- a page asserting
  // something about a calibration the numbers above it no longer describe.
  clearResult();
}

void calibSetResult(const char *msg, bool ok) {
  if (!lblResult_) return;
  lv_obj_set_style_text_color(lblResult_, lv_color_hex(ok ? C_OK : C_FAULT), LV_PART_MAIN);
  lv_label_set_text(lblResult_, msg ? msg : "");
}

void buildCalibPage(lv_obj_t *parent) {
  lv_obj_set_style_bg_color(parent, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN);
  // THE WHOLE PAGE IS A VERTICAL BUDGET, and it does not have slack. Below the
  // 26 px status bar there are 294 px, and this page wants a header, a value
  // box, two message lines and twelve 40 px keys:
  //
  //   4 + 26 + 2 + 36 + 2 + 18 + 2 + 18 + 2 + (4*40 + 3*4) + 4  =  286
  //
  // The first attempt used 6 px padding, 3 px rows and 44 px keys and came to
  // 304, which put the bottom key row -- backspace, 0 and OK, i.e. every key
  // that finishes the job -- off the bottom of the screen. Flex does not
  // complain about that; it just draws what fits.
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
  lv_label_set_text(t, "Calibrate");

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
  // refreshEntry() rather than a literal, because calibSetMass() runs BEFORE
  // buildPages() in setup() -- it stores the mass but cannot touch a label that
  // does not exist yet. Writing "---" here would then paint over the prefill
  // and the page would open blank on a unit that has a perfectly good stored
  // mass. prevEntry_ is cleared first so the guard inside does not suppress it.
  prevEntry_[0] = 0;
  refreshEntry();

  lv_obj_t *u = lv_label_create(row);
  lv_obj_set_style_text_font(u, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(u, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(u, "g");

  // --- the live deflection -------------------------------------------------
  // The reason this page is not just a number pad. See the header.
  lblLive_ = lv_label_create(parent);
  lv_obj_set_style_text_font(lblLive_, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblLive_, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_obj_set_width(lblLive_, LV_PCT(100));
  lv_obj_set_style_text_align(lblLive_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(lblLive_, LV_LABEL_LONG_CLIP);
  lv_label_set_text(lblLive_, "");

  lblResult_ = lv_label_create(parent);
  lv_obj_set_style_text_font(lblResult_, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblResult_, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_obj_set_width(lblResult_, LV_PCT(100));
  lv_obj_set_style_text_align(lblResult_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(lblResult_, LV_LABEL_LONG_CLIP);
  lv_label_set_text(lblResult_, "");

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
                        LV_FLEX_ALIGN_START);

  // Phone order -- 1 2 3 on the top row, not calculator order. Every person
  // holding this has typed a PIN into a phone far more recently than they have
  // used a desk calculator.
  static const char DIGITS[9][2] = {"1", "2", "3", "4", "5", "6", "7", "8", "9"};
  for (uint8_t i = 0; i < 9; i++) {
    key(pad, DIGITS[i], onDigit, (void *)(uintptr_t)DIGITS[i][0], C_KEY);
  }
  key(pad, LV_SYMBOL_BACKSPACE, onBack, nullptr, C_KEY);
  key(pad, "0", onDigit, (void *)(uintptr_t)'0', C_KEY);
  key(pad, LV_SYMBOL_OK, onOk, nullptr, C_OK);
}

void updateCalibPage(const State &s) {
  if (!lblLive_) return;

  const uint32_t now = lv_tick_get();
  if ((int32_t)(now - nextLiveMs_) < 0) return;
  nextLiveMs_ = now + 250;

  const ScaleView &sc = s.scale;
  char buf[64];
  if (sc.online < CELLS) {
    snprintf(buf, sizeof(buf), "%u of %u cells online", sc.online, CELLS);
  } else {
    // THE ZERO AND THE DEFLECTION, side by side, because the deflection is
    // meaningless without knowing what it is measured from. Both are the stored
    // NVS values -- the tare this unit is actually using, not a fresh reading.
    //
    // It is the other half of the same job the deflection does. A tare of zero
    // here means nothing has ever zeroed this platform and the number beside it
    // is an absolute converter reading rather than a load; two tares that look
    // nothing like each other say the corners are carrying very different
    // shares before anything is put on. Either would otherwise have to be
    // chased on a different page, after the calibration had already been taken.
    //
    // COUNTS, not kilograms. The kilograms are computed with the very factor
    // this page is about to replace, so quoting them here would be circular --
    // and on an uncalibrated unit there are none. Counts is the one figure that
    // means the same thing before and after.
    //
    // ONE LINE, and it has to stay one: the label is centred and clipped, so an
    // overflow loses characters from BOTH ends. An earlier version read
    // "now %+ld counts on the platform" and rendered as
    // "w +260516 counts on the platfo".
    snprintf(buf, sizeof(buf), "0 = %ld/%ld   net %+ld", (long)sc.cell[0].offset,
             (long)sc.cell[1].offset, (long)sc.totalCounts);
  }
  if (strcmp(prevLive_, buf) == 0) return;
  snprintf(prevLive_, sizeof(prevLive_), "%s", buf);
  lv_label_set_text(lblLive_, buf);
}

}  // namespace ui
