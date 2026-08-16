#include "ui_scope.h"

#include "ui_perf.h"
#include "ui_state.h"

namespace ui {
namespace {

const uint32_t C_BG = 0x000000;
const uint32_t C_TEXT = 0xE6EDF3;
const uint32_t C_MUTED = 0x8B949E;
const uint32_t C_GRID = 0x30363D;

// One colour per level, matched to the order used everywhere else: index 0 is
// f1, the bottom bowl.
const uint32_t C_SERIES[LEVELS] = {0x3FB950, 0x58A6FF, 0xD29922, 0xF85149};

lv_obj_t *chart_ = nullptr;
lv_chart_series_t *series_[LEVELS] = {nullptr, nullptr, nullptr, nullptr};
lv_obj_t *lblFps_ = nullptr;
lv_obj_t *lblVals_ = nullptr;

// Frame counting moved to ui_perf, which both targets and every page share.
// Two REFR_READY handlers counting the same event into two variables was
// duplicated work for one number.
uint32_t lastSampleMs_ = 0;
bool visible_ = false;

// The ring. Sampling writes here and nowhere else, so it costs a few
// multiplications and no LVGL work at all -- which is what makes it safe to run
// unconditionally no matter how many pages exist.
int16_t ring_[LEVELS][SCOPE_POINTS];
uint16_t ringHead_ = 0;
uint16_t ringFill_ = 0;
bool pendingRender_ = false;

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
  // this and does not slew, which matters -- a step invalidates a tall thin
  // column of the chart, and that is the redraw cost being measured.
  if ((int32_t)(nowMs - c.nextStepMs) >= 0) {
    c.nextStepMs = nowMs + 1500 + (nextRand() % 3000);
    c.target = (c.target > 250) ? (60 + (int32_t)(nextRand() % 30))
                                : (360 + (int32_t)(nextRand() % 60));
  }

  // Move toward the target over a few samples rather than teleporting: a real
  // bowl takes a moment to be placed, and the sensor's trimmed window smooths
  // the edge anyway.
  const int32_t delta = c.target - c.baseline;
  if (delta > 0) c.baseline += (delta > 40) ? 40 : delta;
  else if (delta < 0) c.baseline += (delta < -40) ? -40 : delta;

  // Ranging noise. The real part measures ~2.5 mm stdev at 1 m in the
  // RESPONSIVE preset, so +/-4 is the right order -- big enough to make the
  // trace look alive and force real redraws, small enough not to be the signal.
  int32_t v = c.baseline + ((int32_t)(nextRand() % 9) - 4);

  if (v < 0) v = 0;
  if (v > SCOPE_MAX_MM) v = SCOPE_MAX_MM;
  return v;
}

}  // namespace

void buildScope(lv_obj_t *parent) {
  lv_obj_set_style_bg_color(parent, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(parent, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_row(parent, 4, LV_PART_MAIN);
  lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

  lblFps_ = lv_label_create(parent);
  lv_obj_set_style_text_font(lblFps_, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblFps_, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_text(lblFps_, "-- fps");

  lblVals_ = lv_label_create(parent);
  lv_obj_set_style_text_font(lblVals_, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblVals_, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(lblVals_, "0-500 mm");

  chart_ = lv_chart_create(parent);
  lv_obj_set_width(chart_, LV_PCT(100));
  lv_obj_set_flex_grow(chart_, 1);
  lv_chart_set_type(chart_, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(chart_, SCOPE_POINTS);
  lv_chart_set_range(chart_, LV_CHART_AXIS_PRIMARY_Y, 0, SCOPE_MAX_MM);

  // SHIFT: each new value scrolls the series left, which is what makes this a
  // scope rather than a bar of numbers.
  lv_chart_set_update_mode(chart_, LV_CHART_UPDATE_MODE_SHIFT);

  lv_obj_set_style_bg_color(chart_, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_border_color(chart_, lv_color_hex(C_GRID), LV_PART_MAIN);
  lv_obj_set_style_border_width(chart_, 1, LV_PART_MAIN);
  lv_obj_set_style_pad_all(chart_, 2, LV_PART_MAIN);
  lv_chart_set_div_line_count(chart_, 5, 0);
  lv_obj_set_style_line_color(chart_, lv_color_hex(C_GRID), LV_PART_MAIN);

  // No point markers. At 115 points x 4 series that is 460 little circles to
  // draw every frame, for a plot whose shape is carried entirely by its lines.
  lv_obj_set_style_size(chart_, 0, 0, LV_PART_INDICATOR);
  lv_obj_set_style_line_width(chart_, 2, LV_PART_ITEMS);

  for (uint8_t i = 0; i < LEVELS; i++) {
    series_[i] = lv_chart_add_series(chart_, lv_color_hex(C_SERIES[i]),
                                     LV_CHART_AXIS_PRIMARY_Y);
  }

  // Legend, coloured to match the traces. f1 first, bottom bowl first, the same
  // order as levels[] and the same order as the stack column on the other page.
  lv_obj_t *legend = lv_obj_create(parent);
  lv_obj_set_size(legend, LV_PCT(100), 22);
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

void scopeSample(uint32_t nowMs) {
  if (!armed_) {
    armChannels(nowMs);
    lastSampleMs_ = nowMs;
  }

  // One sample per 20 ms, i.e. 50 Hz -- deliberately FASTER than the real
  // sensors' 10 Hz. The point is to keep the chart permanently dirty while it
  // is visible, so the FPS figure reports what the renderer can sustain rather
  // than how often the data happened to change. At 10 Hz the answer would
  // mostly have been "10".
  if ((int32_t)(nowMs - lastSampleMs_) < 20) return;
  lastSampleMs_ = nowMs;

  for (uint8_t i = 0; i < LEVELS; i++) ring_[i][ringHead_] = (int16_t)sampleChannel(i, nowMs);
  ringHead_ = (uint16_t)((ringHead_ + 1) % SCOPE_POINTS);
  if (ringFill_ < SCOPE_POINTS) ringFill_++;
  pendingRender_ = true;

}

void scopeRender() {
  if (!chart_ || !visible_ || !pendingRender_) return;
  pendingRender_ = false;

  // Only the newest sample is pushed. The chart already holds the history --
  // SHIFT mode scrolls it -- so re-pushing the whole ring every frame would
  // multiply the work by SCOPE_POINTS for an identical picture.
  const uint16_t newest = (uint16_t)((ringHead_ + SCOPE_POINTS - 1) % SCOPE_POINTS);
  for (uint8_t i = 0; i < LEVELS; i++) {
    lv_chart_set_next_value(chart_, series_[i], ring_[i][newest]);
  }
  if (lblVals_) {
    lv_label_set_text_fmt(lblVals_, "f1 %d  f2 %d  f3 %d  f4 %d mm", ring_[0][newest],
                          ring_[1][newest], ring_[2][newest], ring_[3][newest]);
  }
}

void scopeSetVisible(bool visible) {
  if (visible == visible_) return;
  visible_ = visible;
  if (!visible || !chart_) return;

  // Repopulate from the ring in one pass, oldest first, so the trace picks up
  // exactly where the data did rather than scrolling in from the right as
  // though sampling had just started.
  const uint16_t start =
      (uint16_t)((ringHead_ + SCOPE_POINTS - ringFill_) % SCOPE_POINTS);
  for (uint16_t n = 0; n < ringFill_; n++) {
    const uint16_t idx = (uint16_t)((start + n) % SCOPE_POINTS);
    for (uint8_t i = 0; i < LEVELS; i++) {
      lv_chart_set_next_value(chart_, series_[i], ring_[i][idx]);
    }
  }
}

uint16_t scopeFps() { return perfFps(); }

// Refreshes the on-screen readout. Separate from sampling so it happens once a
// second rather than fifty times, and only while the page is being looked at.
void scopeShowPerf() {
  if (!visible_ || !lblFps_) return;
  char buf[96];
  perfFormat(buf, sizeof(buf));
  lv_label_set_text(lblFps_, buf);
}

}  // namespace ui
