#include "ui_scope.h"

#include "ui_perf.h"
#include "ui_state.h"

namespace ui {
namespace {

const uint32_t C_BG = 0x000000;
const uint32_t C_TEXT = 0xE6EDF3;
const uint32_t C_MUTED = 0x8B949E;
const uint32_t C_GRID = 0x232A31;
const uint32_t C_CURSOR = 0x4A5058;

// One colour per level, matched to the order used everywhere else: index 0 is
// f1, the bottom bowl.
const uint32_t C_SERIES[LEVELS] = {0x3FB950, 0x58A6FF, 0xD29922, 0xF85149};

lv_obj_t *canvas_ = nullptr;
lv_obj_t *lblFps_ = nullptr;
lv_obj_t *lblVals_ = nullptr;
void *buf_ = nullptr;

bool visible_ = false;
uint32_t lastSampleMs_ = 0;

// Set by the first scopeFeed(). After that the fabricated generator below never
// runs again -- real data outranks fixtures, and a scope that mixes the two is
// worse than either.
bool fedReal_ = false;
int16_t feedMm_[LEVELS] = {0, 0, 0, 0};
bool feedValid_[LEVELS] = {false, false, false, false};

// Stored for a level with no usable reading. Distinct from any real distance so
// drawColumn can leave a gap instead of plotting it.
const int16_t NO_READING = -1;

// The ring holds the DATA; the canvas holds the PICTURE. They exist separately
// because they answer different questions -- the ring is what lets sampling
// continue while the page is hidden, and the canvas is what lets a redraw touch
// one column instead of the whole plot.
int16_t ring_[LEVELS][SCOPE_W];
uint16_t sweep_ = 0;     // column the NEXT sample will occupy
uint16_t filled_ = 0;    // columns written since boot, capped at SCOPE_W
int16_t prevY_[LEVELS];  // last plotted row per series, for joining segments
bool havePrevCh_[LEVELS] = {false, false, false, false};
bool havePrev_ = false;
bool pendingDraw_ = false;

// A blanked run ahead of the cursor, so the wrap point reads as a moving head
// rather than as a discontinuity in the trace.
const uint16_t GAP = 4;

int16_t yForMm(int32_t mm) {
  if (mm < 0) mm = 0;
  if (mm > SCOPE_MAX_MM) mm = SCOPE_MAX_MM;
  // Inverted: 0 mm at the top. A bowl close to the sensor is a SMALL reading
  // and should sit high; getting this the wrong way up would make a filling
  // stack look like an emptying one.
  return (int16_t)((int32_t)(SCOPE_H - 1) * mm / SCOPE_MAX_MM);
}

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
  // lines at 1/3 and 2/3 -- one pixel each, so effectively free.
  for (uint8_t g = 1; g < 3; g++) {
    const int32_t gy = (int32_t)SCOPE_H * g / 3;
    vline(x, gy, gy, rgb565(C_GRID));
  }

  // The traces. A vertical run from the previous sample's row to this one, so a
  // step reads as a connected edge rather than two unrelated dots.
  for (uint8_t i = 0; i < LEVELS; i++) {
    // A level with no reading draws NOTHING. Clamping it to 0 mm would put a
    // solid line at the top of the plot, which reads as "bowl pressed against
    // the sensor" -- the most alarming possible misreport of "this sensor is
    // not talking". A gap is the honest mark.
    if (ring_[i][x] == NO_READING) {
      havePrevCh_[i] = false;
      continue;
    }
    const int16_t y = yForMm(ring_[i][x]);
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

// --- fabricated sensor data ------------------------------------------------
// A deterministic LCG rather than rand(): the sim and the device must produce
// the same trace, or comparing what they render means comparing two different
// signals.
uint32_t rnd_ = 0x1234567u;
uint16_t nextRand() {
  rnd_ = rnd_ * 1103515245u + 12345u;
  return (uint16_t)((rnd_ >> 16) & 0x7FFF);
}

struct Channel {
  int32_t baseline;
  int32_t target;
  uint32_t nextStepMs;
};
Channel ch_[LEVELS];
bool armed_ = false;

void armChannels(uint32_t nowMs) {
  // Staggered so all four do not step together, which would look like a
  // rendering artefact rather than four independent sensors.
  for (uint8_t i = 0; i < LEVELS; i++) {
    ch_[i].baseline = 380 - i * 20;
    ch_[i].target = ch_[i].baseline;
    ch_[i].nextStepMs = nowMs + 1200 + i * 700;
  }
  armed_ = true;
}

int32_t sampleChannel(uint8_t i, uint32_t nowMs) {
  Channel &c = ch_[i];

  // Bowl arrives or leaves: a step between "near" (present, well under
  // PRESENT_BELOW_MM) and "far" (absent, above ABSENT_ABOVE_MM). Real data does
  // this and does not slew.
  if ((int32_t)(nowMs - c.nextStepMs) >= 0) {
    c.nextStepMs = nowMs + 1500 + (nextRand() % 3000);
    c.target = (c.target > 250) ? (60 + (int32_t)(nextRand() % 30))
                                : (360 + (int32_t)(nextRand() % 60));
  }

  const int32_t delta = c.target - c.baseline;
  if (delta > 0) c.baseline += (delta > 40) ? 40 : delta;
  else if (delta < 0) c.baseline += (delta < -40) ? -40 : delta;

  // Ranging noise. The real part measures ~2.5 mm stdev at 1 m in the
  // RESPONSIVE preset, so +/-4 is the right order.
  int32_t v = c.baseline + ((int32_t)(nextRand() % 9) - 4);
  if (v < 0) v = 0;
  if (v > SCOPE_MAX_MM) v = SCOPE_MAX_MM;
  return v;
}

}  // namespace

void scopeSetBuffer(void *buf) { buf_ = buf; }

void buildScope(lv_obj_t *parent) {
  lv_obj_set_style_bg_color(parent, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(parent, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_row(parent, 2, LV_PART_MAIN);
  lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

  lblFps_ = lv_label_create(parent);
  lv_obj_set_style_text_font(lblFps_, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblFps_, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_obj_set_width(lblFps_, LV_PCT(100));
  lv_label_set_long_mode(lblFps_, LV_LABEL_LONG_WRAP);
  lv_label_set_text(lblFps_, "measuring...");

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
  lv_label_set_text(lblVals_, "0-500 mm");

  lv_obj_t *legend = lv_obj_create(parent);
  lv_obj_set_size(legend, LV_PCT(100), 20);
  lv_obj_set_style_bg_opa(legend, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(legend, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(legend, 0, LV_PART_MAIN);
  lv_obj_remove_flag(legend, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(legend, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  for (uint8_t i = 0; i < LEVELS; i++) {
    lv_obj_t *l = lv_label_create(legend);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(C_SERIES[i]), LV_PART_MAIN);
    lv_label_set_text_fmt(l, "f%u", i + 1);
  }
}

void scopeFeed(const int16_t mm[LEVELS], const bool valid[LEVELS]) {
  fedReal_ = true;
  for (uint8_t i = 0; i < LEVELS; i++) {
    feedMm_[i] = mm[i];
    feedValid_[i] = valid[i];
  }
}

void scopeSample(uint32_t nowMs) {
  if (!armed_) {
    armChannels(nowMs);
    lastSampleMs_ = nowMs;
  }

  // 10 Hz, matching what the real VL53L0X array produces in the RESPONSIVE
  // preset. The previous 50 Hz existed only to keep a scrolling chart
  // permanently dirty for measurement; with a sweeping cursor each sample costs
  // one column, so oversampling buys nothing and a real sensor rate is what the
  // page should be showing anyway.
  if ((int32_t)(nowMs - lastSampleMs_) < 100) return;
  lastSampleMs_ = nowMs;

  for (uint8_t i = 0; i < LEVELS; i++) {
    ring_[i][sweep_] = fedReal_ ? (feedValid_[i] ? feedMm_[i] : NO_READING)
                                : (int16_t)sampleChannel(i, nowMs);
  }
  if (filled_ < SCOPE_W) filled_++;

  // ADVANCED HERE, NOT IN scopeRender(). It used to advance during rendering,
  // which early-returns when the page is hidden -- so every sample landed in
  // column 0 while the stock page was up, and entering the scope after 23 s
  // painted 231 columns of zeros as a solid line pinned to 0 mm. The header of
  // this file promises data stays current whether or not anyone is looking;
  // that promise was only true of the ring's CONTENTS, not its cursor.
  sweep_ = (uint16_t)((sweep_ + 1) % SCOPE_W);
  pendingDraw_ = true;
}

void scopeRender() {
  if (!canvas_ || !visible_ || !pendingDraw_) return;
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
    nextVals = now + 1000;
    lv_label_set_text_fmt(lblVals_, "f1 %d  f2 %d  f3 %d  f4 %d mm", ring_[0][x],
                          ring_[1][x], ring_[2][x], ring_[3][x]);
  }
}

void scopeSetVisible(bool visible) {
  if (visible == visible_) return;
  visible_ = visible;
  if (!visible || !canvas_) return;

  // Repaint every column that holds data. This is the one expensive thing the
  // scope does, and it happens on a page TRANSITION rather than per sample --
  // the difference between paying it once and paying it ten times a second.
  if (px_) {
    for (uint16_t y = 0; y < SCOPE_H; y++) {
      uint16_t *row = px_ + (uint32_t)y * stridePx_;
      for (uint16_t x = 0; x < SCOPE_W; x++) row[x] = rgb565(C_BG);
    }
  }
  havePrev_ = false;
  const uint16_t start = (uint16_t)((sweep_ + SCOPE_W - filled_) % SCOPE_W);
  for (uint16_t n = 0; n < filled_; n++) {
    drawColumn((uint16_t)((start + n) % SCOPE_W), n > 0);
  }
  lv_obj_invalidate(canvas_);
}

uint16_t scopeFps() { return perfFps(); }

void scopeShowPerf() {
  if (!visible_ || !lblFps_) return;
  char buf[128];
  perfFormat(buf, sizeof(buf));
  lv_label_set_text(lblFps_, buf);
}

}  // namespace ui
