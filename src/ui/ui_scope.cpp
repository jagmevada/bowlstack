#include "ui_scope.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_perf.h"
#include "ui_state.h"

namespace ui {
namespace {

const uint32_t C_BG = 0x000000;
const uint32_t C_TEXT = 0xE6EDF3;
const uint32_t C_MUTED = 0x8B949E;
const uint32_t C_GRID = 0x232A31;
const uint32_t C_ZERO = 0x3A424B;  // the zero line, brighter than the thirds
const uint32_t C_CURSOR = 0x4A5058;

// One colour per cell, matched to the order used everywhere else: index 0 is A.
//
// Green, blue, amber -- the same three the battery and status bar already use
// for ok / info / warn, borrowed here purely as a palette that is known to
// separate on this panel. Three traces on a 232-column plot is the point at
// which "pick another colour" stops being free: they have to stay apart at
// 200 DPI, on an IPS black that is really dark grey, through whatever the
// kitchen lighting is.
const uint32_t C_SERIES[CELLS] = {0x3FB950, 0x58A6FF, 0xD29922};
const char *SERIES_NAME[CELLS] = {"A", "B", "C"};

lv_obj_t *canvas_ = nullptr;
lv_obj_t *lblFps_ = nullptr;
lv_obj_t *lblVals_ = nullptr;
void *buf_ = nullptr;
void (*onClose_)(void) = nullptr;

void closeClicked(lv_event_t *) {
  if (onClose_) onClose_();
}

bool visible_ = false;
uint32_t lastSampleMs_ = 0;

// Set by the first scopeFeed(). After that the fabricated generator below never
// runs again -- real data outranks fixtures, and a scope that mixes the two is
// worse than either.
bool fedReal_ = false;
int32_t feedValue_[CELLS] = {0, 0, 0};
bool feedValid_[CELLS] = {false, false, false};
const char *unit_ = "cts";

// The ring holds the DATA; the canvas holds the PICTURE. They exist separately
// because they answer different questions -- the ring is what lets sampling
// continue while the page is hidden, and the canvas is what lets a redraw touch
// one column instead of the whole plot.
//
// Validity is a SEPARATE array rather than a sentinel value. The ToF version
// used -1 for "no reading", which was safe because a distance is never
// negative; a converter count is signed and -1 is an ordinary value, so a
// sentinel here would silently blank a real sample.
int32_t ring_[CELLS][SCOPE_W];
bool ringOk_[CELLS][SCOPE_W];
uint16_t sweep_ = 0;     // column the NEXT sample will occupy
uint16_t filled_ = 0;    // columns written since boot, capped at SCOPE_W
int16_t prevY_[CELLS];   // last plotted row per series, for joining segments
bool havePrevCh_[CELLS] = {false, false, false};
bool havePrev_ = false;
bool pendingDraw_ = false;

// A blanked run ahead of the cursor, so the wrap point reads as a moving head
// rather than as a discontinuity in the trace.
const uint16_t GAP = 4;

// --- the auto-range --------------------------------------------------------
int32_t rangeLo_ = -100;
int32_t rangeHi_ = 100;
uint32_t nextRefitMs_ = 0;
bool needFullRepaint_ = false;

// A floor on the window height, so a perfectly still trace does not get
// magnified until the converter's last bit fills the screen. Below this the
// range is padded out symmetrically instead.
const int32_t MIN_SPAN = 40;

// How often a settled trace is allowed to zoom back IN. Zooming out happens
// immediately -- a sample off the top of the screen is not something to sit on
// -- but zooming in costs a full repaint for a purely cosmetic gain, so it is
// rate-limited and conditional on the trace actually having shrunk a lot.
const uint32_t REFIT_MS = 5000;

int16_t yForValue(int32_t v) {
  const int32_t span = rangeHi_ - rangeLo_;
  if (span <= 0) return SCOPE_H / 2;
  if (v < rangeLo_) v = rangeLo_;
  if (v > rangeHi_) v = rangeHi_;
  // Inverted: the largest value at the top, which is the direction every plot
  // is read in. The ToF version was inverted the other way on purpose, because
  // there a SMALL number meant a bowl close to the sensor; weight has no such
  // reversal, and copying it across would have drawn a filling platform as an
  // emptying one.
  return (int16_t)((int32_t)(SCOPE_H - 1) - (int32_t)(SCOPE_H - 1) * (v - rangeLo_) / span);
}

// What the window WOULD be if it were fitted to the ring right now. Kept
// separate from applying it, because two of the three callers want to look at
// the answer before deciding whether it is worth a full repaint.
bool computeFit(int32_t *outLo, int32_t *outHi) {
  int32_t lo = 0, hi = 0;
  bool any = false;
  for (uint8_t i = 0; i < CELLS; i++) {
    for (uint16_t x = 0; x < SCOPE_W; x++) {
      if (!ringOk_[i][x]) continue;
      const int32_t v = ring_[i][x];
      if (!any) {
        lo = hi = v;
        any = true;
      } else {
        if (v < lo) lo = v;
        if (v > hi) hi = v;
      }
    }
  }
  if (!any) return false;

  int32_t span = hi - lo;
  if (span < MIN_SPAN) {
    const int32_t pad = (MIN_SPAN - span) / 2 + 1;
    lo -= pad;
    hi += pad;
    span = hi - lo;
  }
  // 10% of headroom above and below, so a trace that is merely wobbling at the
  // top of its range does not retrigger a refit on every other sample.
  const int32_t margin = span / 10 + 1;
  *outLo = lo - margin;
  *outHi = hi + margin;
  return true;
}

// Widen only, never narrow. Used on the out-of-range path, where the point is
// to get the sample on screen without also re-scaling for a transient.
bool growRange() {
  int32_t lo, hi;
  if (!computeFit(&lo, &hi)) return false;
  const bool wider = lo < rangeLo_ || hi > rangeHi_;
  if (!wider) return false;
  if (lo < rangeLo_) rangeLo_ = lo;
  if (hi > rangeHi_) rangeHi_ = hi;
  return true;
}

// Fit exactly, in both directions. Only called where a full repaint is already
// acceptable: entering the page, or the settled-trace timer below.
bool fitRange() {
  int32_t lo, hi;
  if (!computeFit(&lo, &hi)) return false;
  if (lo == rangeLo_ && hi == rangeHi_) return false;
  rangeLo_ = lo;
  rangeHi_ = hi;
  return true;
}

// --- direct pixel writes ---------------------------------------------------
// PIXELS ARE WRITTEN DIRECTLY, not through lv_draw_* on a canvas layer.
//
// That is not a micro-optimisation, it is the only way this works.
// lv_canvas_finish_layer() ends with an unconditional lv_obj_invalidate(canvas)
// -- read it in lv_canvas.c -- so every draw through the layer API marks the
// WHOLE canvas dirty no matter how few pixels it touched. Measured, the sweep
// still cost ~38,000 px per frame with the layer API: all of the redesign's
// complexity and none of its benefit.
//
// Writing into the buffer ourselves and calling lv_obj_invalidate_area() for
// just the affected column is what makes the invalidated area actually match
// the changed area. The canvas is ours, the format is one we chose, and a
// vertical run is a strided loop.
uint16_t *px_ = nullptr;
uint32_t stridePx_ = 0;

inline uint16_t rgb565(uint32_t rgb) { return lv_color_to_u16(lv_color_hex(rgb)); }

void vline(uint16_t x, int32_t y0, int32_t y1, uint16_t c) {
  if (!px_ || x >= SCOPE_W) return;
  if (y0 > y1) { const int32_t t = y0; y0 = y1; y1 = t; }
  if (y0 < 0) y0 = 0;
  if (y1 > SCOPE_H - 1) y1 = SCOPE_H - 1;
  uint16_t *p = px_ + (uint32_t)y0 * stridePx_ + x;
  for (int32_t y = y0; y <= y1; y++, p += stridePx_) *p = c;
}

void drawColumn(uint16_t x, bool joinToPrev) {
  if (!px_) return;

  // Clear this column, then paint the cursor head and blank the rest of the gap
  // ahead of it, so the wrap point reads as a moving head.
  vline(x, 0, SCOPE_H - 1, rgb565(C_BG));
  for (uint16_t g = 1; g <= GAP; g++) {
    const uint16_t gx = (uint16_t)((x + g) % SCOPE_W);
    vline(gx, 0, SCOPE_H - 1, rgb565(g == 1 ? C_CURSOR : C_BG));
  }

  // Grid, redrawn per column because the clear above wiped it. Two interior
  // lines at 1/3 and 2/3, one pixel each, so effectively free.
  for (uint8_t g = 1; g < 3; g++) {
    const int32_t gy = (int32_t)SCOPE_H * g / 3;
    vline(x, gy, gy, rgb565(C_GRID));
  }

  // ZERO GETS ITS OWN LINE, brighter than the thirds, whenever it is inside the
  // window. On a scale it is the only y value that means anything by itself --
  // drift away from it is the whole reason to look at this page -- and with an
  // auto-ranging axis there is otherwise no fixed reference at all.
  if (rangeLo_ <= 0 && rangeHi_ >= 0) {
    const int16_t zy = yForValue(0);
    vline(x, zy, zy, rgb565(C_ZERO));
  }

  // The traces. A vertical run from the previous sample's row to this one, so a
  // step reads as a connected edge rather than two unrelated dots.
  for (uint8_t i = 0; i < CELLS; i++) {
    // A cell with no reading draws NOTHING. Clamping it into the window would
    // put a line somewhere plausible, which reads as a measurement -- the most
    // alarming possible misreport of "this cell is not talking". A gap is the
    // honest mark.
    if (!ringOk_[i][x]) {
      havePrevCh_[i] = false;
      continue;
    }
    const int16_t y = yForValue(ring_[i][x]);
    int16_t y0 = y, y1 = y;
    if (joinToPrev && havePrev_ && havePrevCh_[i]) {
      y0 = prevY_[i] < y ? prevY_[i] : y;
      y1 = prevY_[i] < y ? y : prevY_[i];
    }
    vline(x, y0, y1, rgb565(C_SERIES[i]));
    prevY_[i] = y;
    havePrevCh_[i] = true;
  }
  havePrev_ = true;
}

void repaintAll() {
  if (!px_ || !canvas_) return;
  for (uint16_t y = 0; y < SCOPE_H; y++) {
    uint16_t *row = px_ + (uint32_t)y * stridePx_;
    for (uint16_t x = 0; x < SCOPE_W; x++) row[x] = rgb565(C_BG);
  }
  havePrev_ = false;
  const uint16_t start = (uint16_t)((sweep_ + SCOPE_W - filled_) % SCOPE_W);
  for (uint16_t n = 0; n < filled_; n++) {
    drawColumn((uint16_t)((start + n) % SCOPE_W), n > 0);
  }
  lv_obj_invalidate(canvas_);
}

// --- fabricated cell data ---------------------------------------------------
// A deterministic LCG rather than rand(): the sim and the device must produce
// the same trace, or comparing what they render means comparing two different
// signals.
//
// Shaped like a LOAD CELL rather than a rangefinder: a noisy baseline that
// steps when something is put on the platform and steps back when it is taken
// off, with a short settle rather than a slew. Counts, at roughly the order of
// magnitude a 20 kg cell at gain 128 actually produces -- about a hundred per
// gram, so a 500 g bowl is ~50,000.
uint32_t rnd_ = 0x1234567u;
uint16_t nextRand() {
  rnd_ = rnd_ * 1103515245u + 12345u;
  return (uint16_t)((rnd_ >> 16) & 0x7FFF);
}

struct Channel {
  int32_t value;
  int32_t target;
  uint32_t nextStepMs;
};
Channel ch_[CELLS];
bool armed_ = false;

void armChannels(uint32_t nowMs) {
  // Staggered so they do not step together, which would look like a rendering
  // artefact rather than three independent converters.
  for (uint8_t i = 0; i < CELLS; i++) {
    ch_[i].value = 0;
    ch_[i].target = 0;
    ch_[i].nextStepMs = nowMs + 2000 + i * 900;
  }
  armed_ = true;
}

int32_t sampleChannel(uint8_t i, uint32_t nowMs) {
  Channel &c = ch_[i];
  if ((int32_t)(nowMs - c.nextStepMs) >= 0) {
    c.nextStepMs = nowMs + 2500 + (nextRand() % 4000);
    // Loaded or empty. The cells take unequal shares, which is what a bowl
    // sitting off-centre looks like and is the reason all three are drawn.
    c.target = c.target > 10000 ? 0 : (int32_t)(20000 + (nextRand() % 40000) + i * 9000);
  }
  const int32_t delta = c.target - c.value;
  const int32_t step = (delta > 0) ? 1 : -1;
  const int32_t mag = (delta < 0 ? -delta : delta);
  c.value += (mag > 12000) ? step * 12000 : delta;

  // Converter noise, at the order a NAU7802 at gain 128 shows on a quiet bench.
  return c.value + ((int32_t)(nextRand() % 121) - 60);
}

}  // namespace

void scopeSetBuffer(void *buf) { buf_ = buf; }
void scopeOnClose(void (*cb)(void)) { onClose_ = cb; }

void buildScope(lv_obj_t *parent) {
  lv_obj_set_style_bg_color(parent, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(parent, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_row(parent, 2, LV_PART_MAIN);
  lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

  // The back button shares the perf line's row rather than taking a header of
  // its own. A 30 px header would have pushed the canvas past the page height
  // and cost plot area; this costs nothing vertical, and the perf text is the
  // one element here that was never going to fill the width.
  lv_obj_t *top = lv_obj_create(parent);
  lv_obj_set_size(top, LV_PCT(100), 30);
  lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(top, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(top, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(top, 6, LV_PART_MAIN);
  lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lblFps_ = lv_label_create(top);
  lv_obj_set_style_text_font(lblFps_, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblFps_, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_obj_set_flex_grow(lblFps_, 1);
  lv_label_set_long_mode(lblFps_, LV_LABEL_LONG_WRAP);
  lv_label_set_text(lblFps_, "measuring...");

  lv_obj_t *back = lv_button_create(top);
  lv_obj_set_size(back, 42, 28);
  lv_obj_set_style_radius(back, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x21262D), LV_PART_MAIN);
  lv_obj_add_event_cb(back, closeClicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *bl = lv_label_create(back);
  lv_label_set_text(bl, LV_SYMBOL_LEFT);
  lv_obj_center(bl);

  if (buf_) {
    canvas_ = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas_, buf_, SCOPE_W, SCOPE_H, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(canvas_, lv_color_hex(C_BG), LV_OPA_COVER);

    // Cache the pixel pointer and the STRIDE, which is not necessarily the
    // width: LVGL aligns each row, so assuming w*2 bytes would shear the image
    // on any build where the alignment differs.
    lv_draw_buf_t *db = lv_canvas_get_draw_buf(canvas_);
    if (db) {
      // SCOPE_BUF_BYTES assumes stride == width * 2, which holds only while
      // LV_DRAW_BUF_STRIDE_ALIGN is 1. Setting it to 64 for DMA -- a plausible
      // future change -- would make LVGL compute a 512-byte stride and the
      // clear loop below would run ~9 KB past the allocation. Checked rather
      // than assumed, and the scope disables itself rather than corrupting
      // whatever lives after the buffer.
      const uint32_t need = (uint32_t)db->header.stride * SCOPE_H;
      if (need > SCOPE_BUF_BYTES) {
        LV_LOG_ERROR("scope: buffer too small for stride");
        canvas_ = nullptr;
      } else {
        px_ = (uint16_t *)db->data;
        stridePx_ = db->header.stride / 2;
      }
    }
  } else {
    // Loud rather than blank. A scope with no buffer is a platform-layer bug,
    // and an empty black rectangle would read as a scope with no signal.
    lv_obj_t *err = lv_label_create(parent);
    lv_obj_set_style_text_color(err, lv_color_hex(0xF85149), LV_PART_MAIN);
    lv_label_set_text(err, "no canvas buffer\n(scopeSetBuffer not called)");
  }

  lblVals_ = lv_label_create(parent);
  lv_obj_set_style_text_font(lblVals_, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblVals_, lv_color_hex(C_MUTED), LV_PART_MAIN);
  // Width-bound and wrapping. Counts run to seven digits and a sign, so a
  // single-line readout of two of them plus the range is 40 characters -- which
  // does not fit in 232 px at 14 px and, without a width, does not wrap either:
  // it just runs off the right edge.
  lv_obj_set_width(lblVals_, LV_PCT(100));
  lv_label_set_long_mode(lblVals_, LV_LABEL_LONG_WRAP);
  lv_label_set_text(lblVals_, "waiting for a sample");

  lv_obj_t *legend = lv_obj_create(parent);
  lv_obj_set_size(legend, LV_PCT(100), 20);
  lv_obj_set_style_bg_opa(legend, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(legend, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(legend, 0, LV_PART_MAIN);
  lv_obj_remove_flag(legend, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(legend, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  for (uint8_t i = 0; i < CELLS; i++) {
    lv_obj_t *l = lv_label_create(legend);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(C_SERIES[i]), LV_PART_MAIN);
    lv_label_set_text(l, SERIES_NAME[i]);
  }
}

void scopeFeed(const int32_t value[CELLS], const bool valid[CELLS], const char *unit) {
  fedReal_ = true;
  for (uint8_t i = 0; i < CELLS; i++) {
    feedValue_[i] = value[i];
    feedValid_[i] = valid[i];
  }
  if (unit) unit_ = unit;
}

void scopeSample(uint32_t nowMs) {
  if (!armed_) {
    armChannels(nowMs);
    lastSampleMs_ = nowMs;
    nextRefitMs_ = nowMs + REFIT_MS;
  }

  // 10 Hz, matching the rate scale.cpp publishes at AND the rate the converters
  // deliver. Sampling faster than data arrives does not add resolution, it
  // duplicates columns -- the trace would show each reading twice and the
  // 232-column history would cover half as much time while looking the same.
  if ((int32_t)(nowMs - lastSampleMs_) < 100) return;
  lastSampleMs_ = nowMs;

  bool outside = false;
  for (uint8_t i = 0; i < CELLS; i++) {
    const bool ok = fedReal_ ? feedValid_[i] : true;
    const int32_t v = fedReal_ ? feedValue_[i] : sampleChannel(i, nowMs);
    ring_[i][sweep_] = v;
    ringOk_[i][sweep_] = ok;
    if (ok && (v < rangeLo_ || v > rangeHi_)) outside = true;
  }
  if (filled_ < SCOPE_W) filled_++;

  // ADVANCED HERE, NOT IN scopeRender(). It used to advance during rendering,
  // which early-returns when the page is hidden -- so every sample landed in
  // column 0 while the home page was up, and entering the scope after twelve
  // seconds painted 231 columns of one value as a flat line. The header of this
  // file promises data stays current whether or not anyone is looking; that
  // promise was only true of the ring's CONTENTS, not its cursor.
  sweep_ = (uint16_t)((sweep_ + 1) % SCOPE_W);
  pendingDraw_ = true;

  // Zoom OUT at once: a sample off the top of the plot is not something to sit
  // on for five seconds. Zoom IN only on the timer, and only when the trace has
  // settled into less than half the window -- each refit repaints every column,
  // so a range that chased the data would give back exactly what the sweep
  // buys.
  bool ranged = false;
  if (outside) {
    ranged = growRange();
  } else if ((int32_t)(nowMs - nextRefitMs_) >= 0) {
    nextRefitMs_ = nowMs + REFIT_MS;
    // Asked BEFORE it is applied. A trace that has settled into more than half
    // the window is close enough; rescaling it would repaint 232 columns for a
    // cosmetic gain, which is exactly the cost the sweep exists to avoid.
    int32_t lo, hi;
    if (computeFit(&lo, &hi) && (hi - lo) * 2 < (rangeHi_ - rangeLo_)) ranged = fitRange();
  }
  if (ranged) needFullRepaint_ = true;
}

void scopeRender() {
  if (!canvas_ || !visible_) return;

  if (needFullRepaint_) {
    needFullRepaint_ = false;
    pendingDraw_ = false;
    repaintAll();
    return;
  }

  if (!pendingDraw_) return;
  pendingDraw_ = false;

  // sweep_ points at the column the NEXT sample will use, so the one just
  // written -- and the one to draw -- is the previous.
  const uint16_t x = (uint16_t)((sweep_ + SCOPE_W - 1) % SCOPE_W);
  drawColumn(x, true);

  // THE POINT OF THE REDESIGN: invalidate the column and its gap, not the
  // canvas. lv_obj_invalidate() here would hand back every pixel of savings the
  // sweep just bought.
  lv_area_t a;
  lv_obj_get_coords(canvas_, &a);
  const int32_t left = a.x1 + x;
  const int32_t wantRight = left + (int32_t)GAP + 1;
  lv_area_t dirty = {left, a.y1, wantRight, a.y2};
  lv_obj_invalidate_area(canvas_, &dirty);

  // THE GAP WRAPS AND THE RECTANGLE DOES NOT. drawColumn() blanks
  // (x + g) % SCOPE_W, so near the right edge it writes columns at the LEFT
  // edge -- but lv_obj_invalidate_area clips to the object, so those pixels
  // were correct in the buffer and never pushed to the panel. The visible
  // effect was the cursor disappearing from the left edge for the last four
  // columns of every sweep, then snapping back.
  const int32_t overflow = wantRight - (a.x1 + (int32_t)SCOPE_W - 1);
  if (overflow > 0) {
    lv_area_t wrapped = {a.x1, a.y1, a.x1 + overflow, a.y2};
    lv_obj_invalidate_area(canvas_, &wrapped);
  }

  static uint32_t nextVals = 0;
  const uint32_t now = lv_tick_get();
  if (lblVals_ && (int32_t)(now - nextVals) >= 0) {
    nextVals = now + 500;
    // The window is printed alongside the values, because an auto-ranging plot
    // whose scale is not stated is a shape rather than a measurement -- the
    // same trace can be a gram of drift or a kilogram of bowl.
    char buf[96];
    // BUILT IN A LOOP, not from one format string per cell. The two-cell
    // version named A and B in its format and would have shown exactly those
    // two with a third trace on the plot above it -- a readout quietly
    // describing less than the picture, on the page whose whole job is to say
    // what the picture is.
    int k = 0;
    for (uint8_t i = 0; i < CELLS && k < (int)sizeof(buf); i++) {
      if (ringOk_[i][x])
        k += snprintf(buf + k, sizeof(buf) - k, "%s%s %ld", i ? "  " : "", SERIES_NAME[i],
                      (long)ring_[i][x]);
      else
        k += snprintf(buf + k, sizeof(buf) - k, "%s%s --", i ? "  " : "", SERIES_NAME[i]);
    }
    // The window on its own line, because an auto-ranging plot whose scale is
    // not stated is a shape rather than a measurement -- the same trace can be
    // a gram of drift or a kilogram of bowl.
    snprintf(buf + k, sizeof(buf) - k, " %s\nscale %ld .. %ld", unit_, (long)rangeLo_,
             (long)rangeHi_);
    if (strcmp(lv_label_get_text(lblVals_), buf) != 0) lv_label_set_text(lblVals_, buf);
  }
}

void scopeSetVisible(bool visible) {
  if (visible == visible_) return;
  visible_ = visible;
  if (!visible || !canvas_) return;

  // Fit before repainting, so entering the page shows the history at a scale
  // that suits it rather than at whatever the range happened to be when it was
  // last looked at.
  fitRange();
  needFullRepaint_ = false;
  repaintAll();
}

uint16_t scopeFps() { return perfFps(); }

void scopeShowPerf() {
  if (!visible_ || !lblFps_) return;
  char buf[64];
  perfFormatShort(buf, sizeof(buf));
  // Compared before writing, like every other update path here. The caller
  // already gates this to once a second; this makes a future caller that
  // forgets cost nothing rather than 60% of a core.
  if (strcmp(lv_label_get_text(lblFps_), buf) == 0) return;
  lv_label_set_text(lblFps_, buf);
}

}  // namespace ui
