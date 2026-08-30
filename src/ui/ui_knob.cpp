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
const uint32_t C_EMPTY = 0x21262D;  // the vessel above the food
// The same blue ui_screens.cpp uses for an occupied level, and for the reason
// stated there: it says "occupied" without editorialising, which leaves green
// and red free to mean healthy and faulty. A vessel that is 30% full is not a
// fault, it is a vessel with food in it.
const uint32_t C_FOOD = 0x1F6FEB;

// --- the vessel ------------------------------------------------------------
// A CIRCULAR FRUSTUM SEEN SIDE-ON, which is a trapezoid, drawn as two triangles
// through LVGL's own rasteriser rather than into a canvas buffer. A 236x116
// canvas would be 55 KB of RGB565 on a board with about 100 KB of internal heap
// free; two triangles cost nothing and are re-rasterised only when the page is
// invalidated.
const int32_t BOWL_TOP_W = 236;  // the panel's full width, less a hair
// 138 rather than the 116 first tried, and the slack came from measuring the
// font instead of assuming it: lv_font_conv sets line_height from the actual
// glyph extents, and a digits-only subset at 100 px comes to 73 -- not the ~125
// a full face would need. Thirty spare pixels went into the vessel, which is
// the thing on this page worth making bigger.
const int32_t BOWL_H = 136;
// 1.6 : 1, top to base, as specified.
const int32_t BOWL_BASE_W = (int32_t)(BOWL_TOP_W / 1.6f);  // 147

lv_obj_t *bowl_ = nullptr;
lv_obj_t *lblPct_ = nullptr;
lv_obj_t *lblSign_ = nullptr;
lv_obj_t *lblAge_ = nullptr;

uint8_t bowlPct_ = 0;
int16_t prevBowl_ = -1;
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

// Half-width of the vessel at height h above the base, 0 <= h <= BOWL_H.
float halfWidthAt(float h) {
  const float rb = BOWL_BASE_W * 0.5f;
  const float rt = BOWL_TOP_W * 0.5f;
  return rb + (rt - rb) * (h / (float)BOWL_H);
}

// Volume of the frustum from the base up to height h, in arbitrary units. The
// pi/3 is dropped -- only the RATIO against the full vessel is ever used.
float volumeTo(float h) {
  const float rb = BOWL_BASE_W * 0.5f;
  const float r = halfWidthAt(h);
  return h * (rb * rb + rb * r + r * r);
}

// THE WATERLINE FOR A GIVEN FRACTION OF THE FOOD, AND IT IS NOT h = p * H.
//
// This is the one piece of arithmetic on the page that decides a RESULT rather
// than an appearance. A frustum's volume is cubic in its depth, so a vessel
// filled to half its depth holds well under half its contents -- at the 1.6 : 1
// ratio here, half depth is 38.7% of the food.
//
// The attendant sets the knob until the picture matches what is in front of
// them, which means the picture decides what the number means. Fill by depth
// and they are reporting a DEPTH fraction, while the dashboard multiplies it by
// a vessel capacity in kilograms as though it were a MASS fraction -- an eleven
// point bias, in the direction of under-reporting the food, baked into every
// row of the comparison the trial exists to produce.
//
// So the shaded region is solved to be a true fraction of the volume.
//
// Bisection rather than the closed-form cubic root: twenty-four iterations of a
// three-multiply polynomial is nothing beside a redraw, it runs only when the
// level moves, and it cannot be got subtly wrong the way a hand-rearranged
// cubic can.
float depthForFraction(float p) {
  if (p <= 0.0f) return 0.0f;
  if (p >= 1.0f) return (float)BOWL_H;
  const float target = p * volumeTo((float)BOWL_H);
  float lo = 0.0f, hi = (float)BOWL_H;
  for (int i = 0; i < 24; i++) {
    const float mid = (lo + hi) * 0.5f;
    if (volumeTo(mid) < target) lo = mid;
    else hi = mid;
  }
  return (lo + hi) * 0.5f;
}

// A RECTANGLE AND TWO FLARES, not two triangles sharing a diagonal.
//
// The first version split the trapezoid corner to corner, and the shared edge
// showed as a hairline across the vessel at exactly that angle. The cause is
// antialiasing, and it is unavoidable for that split: the rasteriser softens
// each triangle's diagonal independently, so along the seam each contributes
// partial coverage and the two never sum to a full pixel. A diagonal join in an
// antialiased renderer always leaves a line.
//
// So the shape is cut on VERTICAL boundaries instead -- a rectangle spanning the
// base width, with a triangular flare either side. An edge that lies on an
// integer x has no partial coverage to soften, so those joins are exact and
// leave nothing to see. The flares still have soft outer slopes, which is
// wanted: that is the vessel's silhouette rather than a join.
//
// The flares overlap the rectangle by a pixel as well. Belt to the braces --
// same colour, fully opaque, so an overlap is invisible where a one-pixel gap
// from a rounding disagreement would not be.
void fillTrapezoid(lv_layer_t *layer, int32_t cx, int32_t yTop, int32_t yBot,
                   float halfTop, float halfBot, uint32_t rgb) {
  const int32_t hb = (int32_t)(halfBot + 0.5f);
  const int32_t ht = (int32_t)(halfTop + 0.5f);

  lv_draw_rect_dsc_t r;
  lv_draw_rect_dsc_init(&r);
  r.bg_color = lv_color_hex(rgb);
  r.bg_opa = LV_OPA_COVER;
  r.radius = 0;
  lv_area_t mid;
  mid.x1 = cx - hb;
  mid.y1 = yTop;
  mid.x2 = cx + hb;
  mid.y2 = yBot;
  lv_draw_rect(layer, &r, &mid);

  if (ht <= hb) return;  // no flare to draw -- the shape is a rectangle

  lv_draw_triangle_dsc_t d;
  lv_draw_triangle_dsc_init(&d);
  d.color = lv_color_hex(rgb);
  d.opa = LV_OPA_COVER;

  d.p[0].x = cx - ht;      d.p[0].y = yTop;
  d.p[1].x = cx - hb + 1;  d.p[1].y = yTop;
  d.p[2].x = cx - hb + 1;  d.p[2].y = yBot;
  lv_draw_triangle(layer, &d);

  d.p[0].x = cx + ht;      d.p[0].y = yTop;
  d.p[1].x = cx + hb - 1;  d.p[1].y = yTop;
  d.p[2].x = cx + hb - 1;  d.p[2].y = yBot;
  lv_draw_triangle(layer, &d);
}

void bowlDraw(lv_event_t *e) {
  lv_obj_t *o = (lv_obj_t *)lv_event_get_target(e);
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);

  const int32_t cx = a.x1 + lv_area_get_width(&a) / 2;
  const int32_t yTop = a.y1;
  const int32_t yBot = a.y1 + BOWL_H;

  // The whole vessel first in the empty colour, then the food over it. Drawing
  // the empty part as its own shape would need a second trapezoid whose bottom
  // edge is the waterline, and would leave a hairline of background wherever
  // the two rounded to different pixels.
  fillTrapezoid(layer, cx, yTop, yBot, BOWL_TOP_W * 0.5f, BOWL_BASE_W * 0.5f,
                C_EMPTY);

  if (bowlPct_ > 0) {
    const float depth = depthForFraction(bowlPct_ / 100.0f);
    const int32_t yWater = yBot - (int32_t)(depth + 0.5f);
    fillTrapezoid(layer, cx, yWater, yBot, halfWidthAt(depth),
                  BOWL_BASE_W * 0.5f, C_FOOD);
  }
}

}  // namespace

void buildKnob(lv_obj_t *parent) {
  lv_obj_t *scr = lv_obj_create(parent);
  flat(scr);
  lv_obj_set_size(scr, LV_PCT(100), LV_PCT(100));
  // NO TOP PAD FOR THE STATUS BAR. The tileview is ALREADY positioned at
  // y = STATUS_H with height SCREEN_H - STATUS_H (see ui_pages.cpp), so the
  // tile's content area begins below the bar before this page does anything.
  //
  // Padding another 26 px on top of that pushed the whole page down by a bar's
  // height: a visible gap under the status row, and the button row hanging off
  // the bottom edge. One mistake, two symptoms that looked unrelated -- and the
  // weight page, which pads uniformly and never clears the bar itself, was
  // sitting right there as the counter-example.
  lv_obj_set_style_pad_all(scr, 2, LV_PART_MAIN);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(scr, 0, LV_PART_MAIN);
  lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  // --- the vessel -----------------------------------------------------------
  bowl_ = lv_obj_create(scr);
  flat(bowl_);
  lv_obj_set_size(bowl_, BOWL_TOP_W, BOWL_H);
  lv_obj_add_event_cb(bowl_, bowlDraw, LV_EVENT_DRAW_MAIN, nullptr);

  // --- the number, below the vessel -----------------------------------------
  lv_obj_t *row = lv_obj_create(scr);
  flat(row);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, 76);  // font_pct_100 measures 73 -- see ui_font.h
  // 10 px of air between the vessel and the figure. Paid for by two pixels
  // off the bowl and the eight that were spare -- see the budget below.
  lv_obj_set_style_pad_top(row, 10, LV_PART_MAIN);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 4, LV_PART_MAIN);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  // NO WRAP, EVER. Told to clip rather than wrap: a "100" folded onto two lines
  // reads as "10" over "0" for exactly the fraction of a second that matters on
  // a page designed to be glanced at. It is sized so the widest possible string
  // fits -- see ui_font.h -- and the clip is the belt to those braces.
  lblPct_ = lv_label_create(row);
  lv_obj_set_style_text_font(lblPct_, &font_pct_100, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblPct_, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_long_mode(lblPct_, LV_LABEL_LONG_CLIP);
  lv_label_set_text(lblPct_, "--");

  lblSign_ = lv_label_create(row);
  lv_obj_set_style_text_font(lblSign_, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblSign_, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_obj_set_style_pad_top(lblSign_, 34, LV_PART_MAIN);
  lv_label_set_text(lblSign_, "%");

  // SAYS WHEN IT IS OLD rather than going quiet. A stale estimate is the
  // failure mode this trial exists to quantify, so the page names it instead of
  // presenting an old figure as a current one. Empty while fresh -- a line that
  // is always there teaches people to stop reading the area.
  lblAge_ = lv_label_create(scr);
  lv_obj_set_style_text_font(lblAge_, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblAge_, lv_color_hex(C_WARN), LV_PART_MAIN);
  lv_label_set_text(lblAge_, "");

  lv_obj_t *gap = lv_obj_create(scr);
  flat(gap);
  lv_obj_set_width(gap, LV_PCT(100));
  lv_obj_set_flex_grow(gap, 1);

  // --- the two buttons ------------------------------------------------------
  // NO TARE HERE. Taring zeroes the reference the trial is measured against,
  // and that button has no business within reach of the one person deliberately
  // not being shown that reference.
  lv_obj_t *acts = lv_obj_create(scr);
  flat(acts);
  lv_obj_set_width(acts, LV_PCT(100));
  lv_obj_set_height(acts, 48);
  lv_obj_set_flex_flow(acts, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(acts, 8, LV_PART_MAIN);
  lv_obj_set_flex_align(acts, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lv_obj_t *btnSwap = lv_button_create(acts);
  lv_obj_set_size(btnSwap, 56, 46);
  lv_obj_set_style_radius(btnSwap, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_color(btnSwap, lv_color_hex(C_KEY), LV_PART_MAIN);
  lv_obj_add_event_cb(btnSwap, swapClicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *wl = lv_label_create(btnSwap);
  lv_obj_set_style_text_font(wl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(wl, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(wl, LV_SYMBOL_SHUFFLE);
  lv_obj_center(wl);

  lv_obj_t *btn = lv_button_create(acts);
  lv_obj_set_size(btn, 56, 46);
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

  // The vessel is redrawn by invalidating it, and only when the level actually
  // moved -- an invalidate at 20 Hz would re-rasterise four triangles for a
  // picture that had not changed.
  const int16_t want = s.fillKnown ? (int16_t)s.fillPercent : 0;
  if (want != prevBowl_) {
    prevBowl_ = want;
    bowlPct_ = (uint8_t)want;
    lv_obj_invalidate(bowl_);
  }

  const bool stale = s.fillKnown && s.fillReminderDue;
  if (stale) {
    snprintf(buf, sizeof(buf), "set %lu min ago",
             (unsigned long)(s.fillAgeSec / 60));
  } else {
    buf[0] = '\0';
  }
  if (strcmp(buf, prevAge_) != 0) {
    snprintf(prevAge_, sizeof(prevAge_), "%s", buf);
    lv_label_set_text(lblAge_, buf);
  }

  // The number goes amber with the warning, so a glance from across the counter
  // carries the same information as reading the line under it.
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
