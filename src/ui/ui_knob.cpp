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
void (*onSwap_)(void) = nullptr;
void swapClicked(lv_event_t *) {
  if (onSwap_) onSwap_();
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
  // TIGHT, because every pixel taken here comes off the number. 30 at the top
  // is the status bar's height and nothing more; the bottom is flush.
  lv_obj_set_style_pad_all(scr, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_top(scr, 30, LV_PART_MAIN);  // exactly clears the status bar
  lv_obj_set_style_pad_bottom(scr, 0, LV_PART_MAIN);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(scr, 8, LV_PART_MAIN);
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
  // FULL HEIGHT, from directly under the status bar to the bottom edge. A bar
  // that stopped short would be read as a vessel that stopped short -- the
  // whole point of drawing it as a column is that its extent means something,
  // so the extent has to be the whole screen.
  //
  // 30 px wide rather than 44: narrow enough to leave the number 190 px, wide
  // enough to read as a level indicator rather than a scrollbar.
  bar_ = lv_bar_create(scr);
  lv_obj_set_width(bar_, 30);
  lv_obj_set_height(bar_, LV_PCT(100));
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
  // THE CAPTION CARRIES THE PER-CENT SIGN, so the number does not have to. A
  // "%" beside a 100 px figure costs about 60 px of width -- a third of the
  // column -- to repeat what this line already says.
  lv_label_set_text(lblCaption_, "% food left");

  // THE ONLY NUMBER ON THE PAGE, so it gets the whole type budget -- the same
  // 56 px face the weight page spends on its total. Nothing here competes with
  // it, which is the point of the page.
  //
  // TWO LABELS, because font_mass_56 is subset to digits, point, minus and
  // space; a per-cent sign in it renders as a blank. The sign sits beside the
  // number on the built-in face, exactly as the weight page's "kg" does.
  // NO WRAP, EVER. The label is given the column's full width and told to clip
  // rather than wrap: a "100" that folded onto two lines would be read as
  // "10" over "0" for the fraction of a second that matters, and the whole
  // reason this page exists is that the figure is glanced at rather than
  // studied. It has been sized so the widest possible string fits -- see
  // ui_font.h -- and the clip is the belt to that braces.
  lblPct_ = lv_label_create(col);
  lv_obj_set_style_text_font(lblPct_, &font_pct_100, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblPct_, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_obj_set_width(lblPct_, LV_PCT(100));
  lv_label_set_long_mode(lblPct_, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_align(lblPct_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  lv_label_set_text(lblPct_, "--");

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
  lv_obj_t *acts = lv_obj_create(col);
  flat(acts);
  lv_obj_set_width(acts, LV_PCT(100));
  lv_obj_set_height(acts, 52);
  lv_obj_set_style_pad_bottom(acts, 6, LV_PART_MAIN);
  lv_obj_set_flex_flow(acts, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(acts, 8, LV_PART_MAIN);
  lv_obj_set_flex_align(acts, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  // TRIAL: back to the weight page. Still no TARE here -- see above.
  lv_obj_t *btnSwap = lv_button_create(acts);
  lv_obj_set_size(btnSwap, 56, 52);
  lv_obj_set_style_radius(btnSwap, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_color(btnSwap, lv_color_hex(C_KEY), LV_PART_MAIN);
  lv_obj_add_event_cb(btnSwap, swapClicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *wl = lv_label_create(btnSwap);
  lv_obj_set_style_text_font(wl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(wl, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(wl, LV_SYMBOL_SHUFFLE);
  lv_obj_center(wl);

  lv_obj_t *btn = lv_button_create(acts);
  lv_obj_set_size(btn, 56, 52);
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
void knobOnSwap(void (*cb)(void)) { onSwap_ = cb; }

}  // namespace ui
