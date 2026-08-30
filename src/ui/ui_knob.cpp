// TRIAL HARNESS -- the blinded page. See include/ui_knob.h for why it hides
// things rather than shows them.

#include "ui_knob.h"

#include <stdio.h>
#include <string.h>

#include "ui_font.h"

namespace ui {
namespace {

// Duplicated per file, as everywhere else in src/ui/.
const uint32_t C_TEXT = 0xE6EDF3;
const uint32_t C_MUTED = 0x8B949E;
const uint32_t C_WARN = 0x9E6A03;
const uint32_t C_KEY = 0x21262D;
const uint32_t C_PANEL = 0x161B22;
const uint32_t C_BORDER = 0x30363D;
// The same blue ui_screens.cpp uses for an occupied level, and for the reason
// stated there: it says "occupied" without editorialising, which leaves green
// and red free to mean healthy and faulty. A vessel that is 30%% full is not a
// fault, it is a vessel with food in it.
const uint32_t C_PRESENT = 0x1F6FEB;

lv_obj_t *bar_ = nullptr;
lv_obj_t *lblPct_ = nullptr;
lv_obj_t *lblSign_ = nullptr;
lv_obj_t *lblCaption_ = nullptr;
lv_obj_t *lblAge_ = nullptr;

int16_t prevBar_ = -1;
char prevPct_[8] = {0};
char prevAge_[24] = {0};
bool prevStale_ = false;
bool haveStale_ = false;

void (*onSettings_)(void) = nullptr;
void settingsClicked(lv_event_t *) {
  if (onSettings_) onSettings_();
}

void flat(lv_obj_t *o) {
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(o, 0, LV_PART_MAIN);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

}  // namespace

void buildKnob(lv_obj_t *parent) {
  lv_obj_t *scr = lv_obj_create(parent);
  flat(scr);
  lv_obj_set_size(scr, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_pad_all(scr, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_top(scr, 30, LV_PART_MAIN);  // clear of the status bar
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(scr, 12, LV_PART_MAIN);
  lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  // --- the level, as a picture ---------------------------------------------
  // A VERTICAL BAR BESIDE THE NUMBER, because the two are read by different
  // people for different reasons. The attendant is comparing what is on screen
  // against what is in front of them, and a column of blue against a vessel of
  // food is a comparison the eye makes instantly; "62" is a comparison it has
  // to think about. The number stays because the trial needs a figure somebody
  // can read back, and because a bar alone cannot be dialled to a value.
  //
  // Filling from the BOTTOM is not decoration. It is the one orientation that
  // matches the thing being estimated -- food sits in the bottom of a vessel,
  // and a bar that drained downwards from the top would have to be mentally
  // inverted every time it was read.
  bar_ = lv_bar_create(scr);
  lv_obj_set_size(bar_, 44, 210);
  lv_bar_set_range(bar_, 0, 100);
  lv_bar_set_value(bar_, 0, LV_ANIM_OFF);
  lv_obj_set_style_radius(bar_, 6, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar_, lv_color_hex(C_PANEL), LV_PART_MAIN);
  lv_obj_set_style_border_color(bar_, lv_color_hex(C_BORDER), LV_PART_MAIN);
  lv_obj_set_style_border_width(bar_, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(bar_, 6, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(bar_, lv_color_hex(C_PRESENT), LV_PART_INDICATOR);

  // --- the figures ----------------------------------------------------------
  lv_obj_t *col = lv_obj_create(scr);
  flat(col);
  lv_obj_set_flex_grow(col, 1);
  lv_obj_set_height(col, LV_PCT(100));
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);

  // Above the number rather than below it. The attendant arrives holding a
  // spoon and needs to know what is being asked before the figure means
  // anything; a caption underneath gets read second, or not at all.
  lblCaption_ = lv_label_create(col);
  lv_obj_set_style_text_font(lblCaption_, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblCaption_, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(lblCaption_, "food left");

  // THE ONLY NUMBER ON THE PAGE, so it gets the whole type budget -- the same
  // 56 px face the weight page spends on its total. Nothing here competes with
  // it, which is the point of the page.
  //
  // TWO LABELS, because font_mass_56 is subset to digits, point, minus and
  // space; a per-cent sign in it renders as a blank. The sign sits beside the
  // number on the built-in face, exactly as the weight page's "kg" does.
  lv_obj_t *row = lv_obj_create(col);
  flat(row);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, 66);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 3, LV_PART_MAIN);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                        LV_FLEX_ALIGN_START);

  lblPct_ = lv_label_create(row);
  lv_obj_set_style_text_font(lblPct_, &font_mass_56, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblPct_, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_text(lblPct_, "--");

  lblSign_ = lv_label_create(row);
  lv_obj_set_style_text_font(lblSign_, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblSign_, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(lblSign_, 10, LV_PART_MAIN);
  lv_label_set_text(lblSign_, "%");

  // SAYS WHEN IT IS OLD rather than going quiet. A stale estimate is the
  // failure mode this trial exists to quantify, so the page names it instead of
  // presenting an old figure as a current one. Empty while fresh -- a line that
  // is always there teaches people to stop reading the area.
  lblAge_ = lv_label_create(col);
  lv_obj_set_style_text_font(lblAge_, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblAge_, lv_color_hex(C_WARN), LV_PART_MAIN);
  lv_label_set_text(lblAge_, "");

  lv_obj_t *gap = lv_obj_create(col);
  flat(gap);
  lv_obj_set_width(gap, LV_PCT(100));
  lv_obj_set_flex_grow(gap, 1);

  // NO TARE HERE. Taring is a scale action, and a button that zeroes the
  // reference the trial is measured against has no business within reach of the
  // one person who is deliberately not being shown that reference.
  lv_obj_t *btn = lv_button_create(col);
  lv_obj_set_size(btn, 64, 52);
  lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_color(btn, lv_color_hex(C_KEY), LV_PART_MAIN);
  lv_obj_add_event_cb(btn, settingsClicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *sl = lv_label_create(btn);
  lv_obj_set_style_text_font(sl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(sl, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(sl, LV_SYMBOL_SETTINGS);
  lv_obj_center(sl);
}

void updateKnob(const State &s) {
  if (lblPct_ == nullptr) return;

  char buf[24];

  // "--" rather than "0" when there is no estimate. A knobless board should not
  // claim an empty vessel, which is the same rule that keeps weight_g NULL
  // until a scale is calibrated.
  if (s.fillKnown) snprintf(buf, sizeof(buf), "%u", (unsigned)s.fillPercent);
  else snprintf(buf, sizeof(buf), "--");
  if (strcmp(buf, prevPct_) != 0) {
    snprintf(prevPct_, sizeof(prevPct_), "%s", buf);
    lv_label_set_text(lblPct_, buf);
  }

  // Guarded like every other write here: lv_bar_set_value invalidates the
  // object whether or not the value moved, and this runs at 20 Hz.
  const int16_t want = s.fillKnown ? (int16_t)s.fillPercent : 0;
  if (want != prevBar_) {
    prevBar_ = want;
    lv_bar_set_value(bar_, want, LV_ANIM_OFF);
  }

  const bool stale = s.fillKnown && s.fillReminderDue;
  if (stale) {
    const uint32_t mins = s.fillAgeSec / 60;
    snprintf(buf, sizeof(buf), "last set %lu min ago", (unsigned long)mins);
  } else {
    buf[0] = '\0';
  }
  if (strcmp(buf, prevAge_) != 0) {
    snprintf(prevAge_, sizeof(prevAge_), "%s", buf);
    lv_label_set_text(lblAge_, buf);
  }

  // The number itself goes amber with the warning, so a glance from across the
  // counter carries the same information as reading the line under it.
  if (!haveStale_ || stale != prevStale_) {
    haveStale_ = true;
    prevStale_ = stale;
    lv_obj_set_style_text_color(lblPct_, lv_color_hex(stale ? C_WARN : C_TEXT),
                                LV_PART_MAIN);
  }
}

void knobOnSettings(void (*cb)(void)) { onSettings_ = cb; }

}  // namespace ui
