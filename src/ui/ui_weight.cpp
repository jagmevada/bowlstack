// Weight view -- the primary screen.
//
// WHAT IT SHOWS: one TOTAL -- every weighing platform on this station added up, the
// serving counter (C1) and each buffer-stock platform (B1..B3) -- and under it one
// row per platform. A station is one board but several instruments, and the person
// walking up wants the food on hand first and where it is second.
//
// THE PER-CELL BREAKDOWN IS NOT HERE. The counter's three cells (A/B/C) and their
// shares were a setup view that lived on this page while platforms were being
// levelled; that job is done, and they are on Settings > Diagnose, read
// deliberately by somebody who came looking for them.
//
// THE TARE BUTTON IS NOT HERE EITHER, and for the opposite reason: it was too easy
// to hit, at thumb height on the one screen used with a bowl in the other hand. A
// stray tap silently redefines zero. It is Settings > Scale > Tare, three taps deep;
// buffer platforms are zeroed from Settings > Buffers for the same reason.
//
// LAID OUT WITH FLEX, like every other page here. Arithmetic against a fixed anchor
// cannot express "these must not overlap", it can only happen to satisfy it for one
// font and one string length.

#include "ui_weight.h"

#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "load_scale.h"
#include "ui_font.h"
#include "ui_platforms.h"

namespace ui {
namespace {

const uint32_t C_BG = 0x000000;
const uint32_t C_TEXT = 0xE6EDF3;
const uint32_t C_MUTED = 0x8B949E;
const uint32_t C_WARN = 0x9E6A03;
const uint32_t C_WARN_TEXT = 0xD29922;  // the warn hue legible as TEXT on black
const uint32_t C_FAULT = 0xB62324;
const uint32_t C_CELL_FAULT = 0xE05C5C;
const uint32_t C_KEY = 0x21262D;
// The same blue ui_screens.cpp uses for an occupied level, and chosen here for
// the reason stated there: it "says occupied without editorialising, which
// leaves green and red free to mean healthy and faulty on the same screen".
const uint32_t C_PRESENT = 0x1F6FEB;

lv_obj_t *lblCaption;
// The counter's vessel offset in force, at the left end of the caption row. A
// figure subtracted from the number below has to be visible beside it.
lv_obj_t *lblOffset_ = nullptr;
char prevOffset_[16] = {0};
lv_obj_t *lblTotal;

// --- the platform rows -------------------------------------------------------
// One per platform, built once and only shown/hidden: building on demand would be a
// layout change on a page redrawn ten times a second.
lv_obj_t *platBox_ = nullptr;
lv_obj_t *rowObj_[PLATFORMS] = {nullptr, nullptr, nullptr, nullptr};
lv_obj_t *rowLbl_[PLATFORMS] = {nullptr, nullptr, nullptr, nullptr};
lv_obj_t *rowVal_[PLATFORMS] = {nullptr, nullptr, nullptr, nullptr};
lv_obj_t *rowBowls_[PLATFORMS] = {nullptr, nullptr, nullptr, nullptr};
char prevRowLbl_[PLATFORMS][4] = {{0}, {0}, {0}, {0}};
char prevRowVal_[PLATFORMS][16] = {{0}, {0}, {0}, {0}};
char prevRowBowls_[PLATFORMS][8] = {{0}, {0}, {0}, {0}};
uint32_t prevRowColor_[PLATFORMS] = {0, 0, 0, 0};
bool haveRowColor_[PLATFORMS] = {false, false, false, false};
int8_t prevRowShown_[PLATFORMS] = {-1, -1, -1, -1};

lv_obj_t *lblFlag;
lv_obj_t *btnSettings;

// --- the knob row ----------------------------------------------------------
// TRIAL HARNESS. One line directly above the action row: a press dot, then the
// attendant's manual fill estimate.
lv_obj_t *encDot = nullptr;
// TRIAL: the manual fill estimate -- the number the attendant sets and the one the
// experiment is about.
lv_obj_t *lblFill = nullptr;
// AS LONG AS THE BUFFER IT CACHES (40). At 16 the stale form "37%  (14 min old)"
// was truncated, the truncated string never equalled the freshly formatted one, and
// the guard below failed on EVERY frame once the estimate went stale. 32 fixed that
// string but still sat below updateWeight()'s 40-byte buffer, which the compiler
// flagged: a cache must hold the longest thing it is compared against.
char prevFill_[40] = {0};

// The dot is held for half a second after each press, on LVGL's own tick (src/ui/
// has no millis()). ARMED IS NOT OPTIONAL: a signed tick compare is only correct
// while the deadline is within +/-24.9 days of now, and an unset or long-expired
// deadline has unbounded age -- with the sentinel 0 still in place the dot would
// latch solid once uptime passed 2^31 ms. Disarming on expiry bounds it.
uint32_t encFlashUntil_ = 0;
bool encFlashArmed_ = false;
uint32_t prevPressCount_ = 0;
bool havePress_ = false;
bool encDotLit_ = false;

void (*onSettings_)(void) = nullptr;
void (*onSwap_)(void) = nullptr;
void swapClicked(lv_event_t *) {
  if (onSwap_) onSwap_();
}

void settingsClicked(lv_event_t *) {
  if (onSettings_) onSettings_();
}

void styleFlat(lv_obj_t *o) {
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(o, 0, LV_PART_MAIN);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

// --- change detection -------------------------------------------------------
// Held as the FORMATTED strings rather than the numbers behind them, which is the
// stricter test and the one that matches what the panel actually repaints. Two
// totals 0.4 g apart are a different float and the same pixels.
char prevTotal_[16] = {0};
char prevCaption_[32] = {0};
char prevFlag_[24] = {0};
uint32_t prevFlagColor_ = 0;
uint32_t prevTotalColor_ = 0;
bool haveTotalColor_ = false;

void setIfChanged(lv_obj_t *label, char *prev, uint32_t prevLen, const char *text) {
  if (strncmp(prev, text, prevLen - 1) == 0) return;
  snprintf(prev, prevLen, "%s", text);
  lv_label_set_text(label, text);
}

void setTotalColor(uint32_t rgb) {
  if (haveTotalColor_ && prevTotalColor_ == rgb) return;
  haveTotalColor_ = true;
  prevTotalColor_ = rgb;
  lv_obj_set_style_text_color(lblTotal, lv_color_hex(rgb), LV_PART_MAIN);
}

// Kilograms to `decimals` places (1, 2 or 3), from a gram value.
//
// lroundf and then an integer split, rather than "%.*f" on the float: printf rounds
// in binary and 1.0005 is not representable. And it is ROUNDING ONCE, at the
// displayed place -- rounding to the gram and again at the print is how 1.4996 kg
// becomes 1.50 -- never truncation, which would build a scale that reads light.
void formatKg(char *buf, uint32_t len, float grams, uint8_t decimals) {
  if (decimals < 1) decimals = 1;
  if (decimals > 3) decimals = 3;
  const long perKg = (decimals == 1) ? 10L : (decimals == 2) ? 100L : 1000L;
  const long gramsPerStep = 1000L / perKg;  // 100, 10, 1
  const long steps = (long)lroundf(grams / (float)gramsPerStep);
  const long a = steps < 0 ? -steps : steps;
  snprintf(buf, len, "%s%ld.%0*ld", steps < 0 ? "-" : "", a / perKg, (int)decimals,
           a % perKg);
}

// ONE DECIMAL EVERYWHERE ON THIS PAGE. The counter's third decimal was a gram on a
// 20 kg cell; with 200 kg buffer platforms on the same page a mixed precision would
// make a column of figures that cannot be compared at a glance, and 0.1 kg is
// already finer than a serving. (Diagnose keeps every place.)
const uint8_t DECIMALS = 1;

}  // namespace

void weightPageHidden() {
  // Drop the press reference. The next updateWeight() re-reads it instead of
  // comparing against a count from before the page went away -- which is the
  // difference between "the knob was just pressed" and "the knob was pressed at
  // some point while you were in the menu".
  havePress_ = false;
}

void weightOnSettings(void (*cb)(void)) { onSettings_ = cb; }
void weightOnSwap(void (*cb)(void)) { onSwap_ = cb; }

void buildWeight(lv_obj_t *parent) {
  lv_obj_t *scr = parent ? parent : lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_text_color(scr, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(scr, 8, LV_PART_MAIN);
  // 2 PX BETWEEN ROWS, DOWN FROM 4, and the budget is why. The content box is
  // 278 px and this tile does not scroll, so an overflow is not a scrollbar, it is
  // the bottom of the gear gone. With four platforms AND a warning chip up:
  //
  //   caption 18 + total 39 + rows (4x24 + 3x2) 102 + chip 22 + knob 22
  //   + buttons 56 = 259, plus six gaps of 2 = 271 of 278
  //
  // Six gaps of 4 would be 283. Fewer platforms leave more slack, which the spacer
  // above the chip absorbs.
  lv_obj_set_style_pad_row(scr, 2, LV_PART_MAIN);
  lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lblCaption = lv_label_create(scr);
  lv_obj_set_style_text_font(lblCaption, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblCaption, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_obj_set_width(lblCaption, LV_PCT(100));
  lv_obj_set_style_text_align(lblCaption, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(lblCaption, LV_LABEL_LONG_CLIP);
  lv_label_set_text(lblCaption, "total, kg");

  // THE COUNTER'S VESSEL OFFSET, at the left end of the caption's row: it is
  // subtracted from C1 and so from the total, and a figure being subtracted has to
  // be visible beside it. IGNORE_LAYOUT and positioned, so the centred caption does
  // not move; (0,0) is the top-left of the page's content box. Blank when off.
  lblOffset_ = lv_label_create(scr);
  lv_obj_add_flag(lblOffset_, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_set_style_text_font(lblOffset_, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblOffset_, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_obj_set_pos(lblOffset_, 0, 1);
  lv_obj_set_width(lblOffset_, 74);
  lv_obj_set_style_text_align(lblOffset_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  lv_label_set_long_mode(lblOffset_, LV_LABEL_LONG_CLIP);
  lv_label_set_text(lblOffset_, "");

  // THE TOTAL. FIXED WIDTH, which is the performance story of this page: a
  // content-sized label re-measures on every set_text and, when the width changes,
  // dirties the flex layout and moves every sibling -- measured once at 250 ms a
  // frame. 224 is the whole content width.
  //
  // font_mass_56, ALWAYS. The 84 px face fits "100.0" but not "888.8" (~245 px),
  // and a station with buffer stock reads three digits before the point as a matter
  // of course. One face also keeps the line height fixed, so the rows below never
  // jump when the reading crosses a size boundary. "1234.5" at 56 px is ~201 px.
  lblTotal = lv_label_create(scr);
  lv_obj_set_style_text_font(lblTotal, &font_mass_56, LV_PART_MAIN);
  lv_obj_set_width(lblTotal, 224);
  lv_obj_set_style_text_align(lblTotal, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  // CLIP rather than WRAP: a second line changes the label's height and dirties the
  // layout exactly the way the width change above would.
  lv_label_set_long_mode(lblTotal, LV_LABEL_LONG_CLIP);
  lv_label_set_text(lblTotal, "--");

  // --- one row per platform --------------------------------------------------
  // A fixed-width name, a fixed-width right-aligned weight, and the bowl count:
  // 36 + 4 + 124 + 4 + 56 = 224. Never content-sized labels, for the reason above.
  // 20 px for the figure: the type scale's glanceable floor is 18.
  platBox_ = lv_obj_create(scr);
  styleFlat(platBox_);
  lv_obj_set_width(platBox_, LV_PCT(100));
  lv_obj_set_height(platBox_, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(platBox_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(platBox_, 2, LV_PART_MAIN);

  for (uint8_t i = 0; i < PLATFORMS; i++) {
    lv_obj_t *r = lv_obj_create(platBox_);
    styleFlat(r);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, 24);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r, 4, LV_PART_MAIN);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
    rowObj_[i] = r;

    rowLbl_[i] = lv_label_create(r);
    lv_obj_set_style_text_font(rowLbl_[i], &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(rowLbl_[i], lv_color_hex(C_MUTED), LV_PART_MAIN);
    lv_obj_set_width(rowLbl_[i], 36);
    lv_label_set_long_mode(rowLbl_[i], LV_LABEL_LONG_CLIP);
    lv_label_set_text(rowLbl_[i], "");

    // 124 holds the widest thing it can print: "1234.5 kg" is ~100 px at 20 px.
    rowVal_[i] = lv_label_create(r);
    lv_obj_set_style_text_font(rowVal_[i], &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_width(rowVal_[i], 124);
    lv_obj_set_style_text_align(rowVal_[i], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_long_mode(rowVal_[i], LV_LABEL_LONG_CLIP);
    lv_label_set_text(rowVal_[i], "--");

    // "4 bw?" at 16 px is ~42 px; muted, because it qualifies the weight beside it
    // rather than competing with it.
    rowBowls_[i] = lv_label_create(r);
    lv_obj_set_style_text_font(rowBowls_[i], &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(rowBowls_[i], lv_color_hex(C_MUTED), LV_PART_MAIN);
    lv_obj_set_width(rowBowls_[i], 56);
    lv_obj_set_style_text_align(rowBowls_[i], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_long_mode(rowBowls_[i], LV_LABEL_LONG_CLIP);
    lv_label_set_text(rowBowls_[i], "");
  }

  // A spacer that eats the slack, so the warning chip, the knob row and the buttons
  // sit together at the bottom -- the chip "just above the knob row" -- and the
  // platforms stay directly under the total they add up to.
  lv_obj_t *gap = lv_obj_create(scr);
  styleFlat(gap);
  lv_obj_set_width(gap, LV_PCT(100));
  lv_obj_set_flex_grow(gap, 1);

  // Shown only when something is wrong or unproven. A chip lit in the ordinary state
  // teaches people to stop reading the area, the opposite of what a chip is for.
  lblFlag = lv_label_create(scr);
  lv_obj_set_style_text_font(lblFlag, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(lblFlag, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(lblFlag, lv_color_hex(C_WARN), LV_PART_MAIN);
  lv_obj_set_style_radius(lblFlag, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_all(lblFlag, 3, LV_PART_MAIN);
  lv_label_set_text(lblFlag, "");
  lv_obj_add_flag(lblFlag, LV_OBJ_FLAG_HIDDEN);

  // --- the knob row -------------------------------------------------------
  // Above the buttons, because it is a readout and they are controls, and a readout
  // below the thing you press gets covered by the hand that presses it.
  lv_obj_t *encRow = lv_obj_create(scr);
  styleFlat(encRow);
  lv_obj_set_width(encRow, LV_PCT(100));
  lv_obj_set_height(encRow, 22);  // TRIAL: exactly the 20 px figure's line box
  lv_obj_set_style_pad_bottom(encRow, 4, LV_PART_MAIN);
  lv_obj_set_flex_flow(encRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(encRow, 8, LV_PART_MAIN);
  lv_obj_set_flex_align(encRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  // THE DOT IS ALWAYS IN THE LAYOUT AND ONLY ITS OPACITY CHANGES; hiding it would
  // move the figure beside it eight pixels on every press. BLUE, NOT RED: red is
  // what a dead platform is painted on this very page, and a knob press is the most
  // ordinary event the device has.
  encDot = lv_obj_create(encRow);
  styleFlat(encDot);
  lv_obj_set_size(encDot, 12, 12);
  lv_obj_set_style_radius(encDot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(encDot, lv_color_hex(C_PRESENT), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(encDot, LV_OPA_TRANSP, LV_PART_MAIN);

  lblFill = lv_label_create(encRow);
  lv_obj_set_style_text_font(lblFill, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblFill, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_text(lblFill, "");

  // --- the action row -----------------------------------------------------
  // TWO WAYS OFF THIS PAGE, and nothing that changes a measurement: a mis-tap costs
  // a page turn, not a zero. 56 px tall, buttons 56 square (a 64 px track in a 56 px
  // row once placed them at y = -4 and pushed the touch target past the row).
  // END, so the gear sits on the exact pixels a thumb has learnt.
  lv_obj_t *actions = lv_obj_create(scr);
  styleFlat(actions);
  lv_obj_set_width(actions, LV_PCT(100));
  lv_obj_set_height(actions, 56);
  lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(actions, 8, LV_PART_MAIN);
  lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  // TRIAL HARNESS: the page swap, to the left of the gear.
  lv_obj_t *btnSwap = lv_button_create(actions);
  lv_obj_set_size(btnSwap, 56, 56);
  lv_obj_set_style_radius(btnSwap, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_color(btnSwap, lv_color_hex(C_KEY), LV_PART_MAIN);
  lv_obj_add_event_cb(btnSwap, swapClicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *wl = lv_label_create(btnSwap);
  lv_obj_set_style_text_font(wl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(wl, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(wl, LV_SYMBOL_SHUFFLE);
  lv_obj_center(wl);

  btnSettings = lv_button_create(actions);
  lv_obj_set_size(btnSettings, 56, 56);
  lv_obj_set_style_radius(btnSettings, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_color(btnSettings, lv_color_hex(C_KEY), LV_PART_MAIN);
  lv_obj_add_event_cb(btnSettings, settingsClicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *sl = lv_label_create(btnSettings);
  lv_obj_set_style_text_font(sl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(sl, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(sl, LV_SYMBOL_SETTINGS);
  lv_obj_center(sl);
}

void updateWeight(const State &st) {
  const ScaleView &s = st.scale;
  char buf[40];

  // --- the knob row ---------------------------------------------------------
  if (lblFill != nullptr) {
    // TRIAL: the manual estimate, and it says when it is STALE rather than going
    // quiet -- an estimate nobody has refreshed is the failure mode a knob-based
    // system actually has.
    if (st.fillKnown) {
      if (st.fillReminderDue && !st.fillAgeKnown) {
        snprintf(buf, sizeof(buf), "%u%%  (unconfirmed)", (unsigned)st.fillPercent);
      } else if (st.fillReminderDue) {
        snprintf(buf, sizeof(buf), "%u%%  (%lu min old)", (unsigned)st.fillPercent,
                 (unsigned long)(st.fillAgeSec / 60));
      } else {
        snprintf(buf, sizeof(buf), "%u%%", (unsigned)st.fillPercent);
      }
    } else {
      buf[0] = '\0';
    }
    if (strcmp(buf, prevFill_) != 0) {
      snprintf(prevFill_, sizeof(prevFill_), "%s", buf);
      lv_label_set_text(lblFill, buf);
      lv_obj_set_style_text_color(
          lblFill, lv_color_hex(st.fillReminderDue ? C_WARN : C_TEXT), LV_PART_MAIN);
    }

    // A COUNTER COMPARISON, not an edge on a bool -- see State::encoderPressCount.
    // havePress_ suppresses the first frame after this page becomes visible, so a
    // press made inside the menu does not light the dot on the way back.
    if (!havePress_) {
      havePress_ = true;
      prevPressCount_ = st.encoderPressCount;
    } else if (st.encoderPressCount != prevPressCount_) {
      prevPressCount_ = st.encoderPressCount;
      encFlashUntil_ = lv_tick_get() + 500;
      encFlashArmed_ = true;
    }
    if (encFlashArmed_ && (int32_t)(encFlashUntil_ - lv_tick_get()) <= 0) {
      encFlashArmed_ = false;
    }
    if (encFlashArmed_ != encDotLit_) {
      encDotLit_ = encFlashArmed_;
      lv_obj_set_style_bg_opa(encDot, encDotLit_ ? LV_OPA_COVER : LV_OPA_TRANSP,
                              LV_PART_MAIN);
    }
  }

  // --- the total ---------------------------------------------------------------
  const PlatformTotal t = platformTotal(st);
  if (!t.any) {
    // Nothing has a weight, so there is no total. Dashes rather than a zero: on a
    // scale, zero is a measurement somebody might act on.
    setIfChanged(lblTotal, prevTotal_, sizeof(prevTotal_), "--");
    setTotalColor(C_MUTED);
    setIfChanged(lblCaption, prevCaption_, sizeof(prevCaption_), "total, kg");
  } else {
    formatKg(buf, sizeof(buf), t.grams, DECIMALS);
    setIfChanged(lblTotal, prevTotal_, sizeof(prevTotal_), buf);
    setTotalColor(C_TEXT);
    // "AT LEAST", NOT "TOTAL", WHEN SOMETHING IS LEFT OUT. A platform with no weight
    // is not added as zero, so the sum of the others is a genuine lower bound -- the
    // same word the counter has always used for a missing cell. The chip below names
    // what is missing.
    setIfChanged(lblCaption, prevCaption_, sizeof(prevCaption_),
                 t.partial ? "at least, kg" : "total, kg");
  }

  // The counter's vessel offset, only while it is actually being applied to C1.
  char off[16];
  if (s.calibrated && st.platforms[0].kgKnown &&
      lscale::vesselOnPlatform(s.totalGrams, s.vesselOffsetG))
    snprintf(off, sizeof(off), "C1 -%.1fkg", s.vesselOffsetG / 1000.0f);
  else
    off[0] = '\0';
  setIfChanged(lblOffset_, prevOffset_, sizeof(prevOffset_), off);

  // --- the rows ----------------------------------------------------------------
  for (uint8_t i = 0; i < PLATFORMS; i++) {
    const PlatformRow &r = st.platforms[i];
    // Shown/hidden only on a CHANGE: a flag toggle invalidates the object, and doing
    // it every frame for a value that changes when hardware is plugged in would be
    // ten redraws a second for nothing.
    if (prevRowShown_[i] != (int8_t)r.present) {
      prevRowShown_[i] = (int8_t)r.present;
      if (r.present) lv_obj_remove_flag(rowObj_[i], LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(rowObj_[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (!r.present) continue;

    setIfChanged(rowLbl_[i], prevRowLbl_[i], sizeof(prevRowLbl_[i]), r.label);

    uint32_t col;
    if (r.kgKnown) {
      formatKg(buf, sizeof(buf), r.grams, DECIMALS);
      const size_t n = strlen(buf);
      snprintf(buf + n, sizeof(buf) - n, " kg");
      // A partial figure is real and is in the total -- drawn in the caveat colour,
      // so the row the chip is talking about can be found.
      col = r.partial ? C_WARN_TEXT : C_TEXT;
    } else {
      // NO STALE OR ZERO FIGURE beside a state that says there is no weight -- the
      // reason instead, which is also what somebody needs to fix it.
      snprintf(buf, sizeof(buf), "%s", r.why[0] ? r.why : "--");
      col = (r.overRange || r.state == Cell::Offline) ? C_CELL_FAULT : C_MUTED;
    }
    setIfChanged(rowVal_[i], prevRowVal_[i], sizeof(prevRowVal_[i]), buf);
    if (!haveRowColor_[i] || prevRowColor_[i] != col) {
      haveRowColor_[i] = true;
      prevRowColor_[i] = col;
      lv_obj_set_style_text_color(rowVal_[i], lv_color_hex(col), LV_PART_MAIN);
    }

    // BOWLS, on buffer rows only, and only beside a WEIGHT: a platform that is
    // offline or uncalibrated has no reading, and a bowl count printed next to
    // "offline" would be a remembered number dressed as a measured one. The '?' is
    // the unconfirmed count after a power cycle -- see load_scale.h.
    if (r.role == PlatformRole::Buffer && r.kgKnown)
      snprintf(buf, sizeof(buf), "%u bw%s", (unsigned)r.bowls, r.bowlsConfirmed ? "" : "?");
    else
      buf[0] = '\0';
    setIfChanged(rowBowls_[i], prevRowBowls_[i], sizeof(prevRowBowls_[i]), buf);
  }

  // --- the chip ------------------------------------------------------------------
  // One chip, naming the most serious thing the total leaves out ("B3 offline +1").
  // Only one is ever shown: a stack of badges on a 240 px screen is a wall.
  char flag[24];
  platformChip(st, t, flag, sizeof(flag));
  const uint32_t flagColor = t.fault ? C_FAULT : C_WARN;
  if (strncmp(prevFlag_, flag, sizeof(prevFlag_) - 1) != 0 || prevFlagColor_ != flagColor) {
    snprintf(prevFlag_, sizeof(prevFlag_), "%s", flag);
    prevFlagColor_ = flagColor;
    if (flag[0]) {
      lv_label_set_text(lblFlag, flag);
      lv_obj_set_style_bg_color(lblFlag, lv_color_hex(flagColor), LV_PART_MAIN);
      lv_obj_remove_flag(lblFlag, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(lblFlag, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

}  // namespace ui
