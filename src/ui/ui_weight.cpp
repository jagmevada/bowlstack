// Weight view -- the primary screen.
//
// LAID OUT WITH FLEX, like every other page here, and for the reason
// ui_screens.cpp records: arithmetic against a fixed anchor cannot express
// "these must not overlap", it can only happen to satisfy it for one font and
// one string length. This page's strings change WIDTH as the load changes --
// "0" to "-12345" is a factor of six -- so a coordinate layout would be correct
// on an empty platform and broken on a full one.

#include "ui_weight.h"

#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_perf.h"

namespace ui {
namespace {

// The same palette as the rest of the UI, so the station and the dashboard read
// as one product.
const uint32_t C_BG = 0x000000;
const uint32_t C_PANEL = 0x161B22;
const uint32_t C_BORDER = 0x30363D;
const uint32_t C_TEXT = 0xE6EDF3;
const uint32_t C_MUTED = 0x8B949E;
const uint32_t C_WARN = 0x9E6A03;
const uint32_t C_FAULT = 0xB62324;
const uint32_t C_CELL_FAULT = 0xE05C5C;
const uint32_t C_KEY = 0x21262D;

lv_obj_t *lblCaption;
lv_obj_t *lblTotal;
lv_obj_t *lblUnit;
lv_obj_t *lblFlag;
lv_obj_t *cellBox[CELLS];
lv_obj_t *cellName[CELLS];
lv_obj_t *cellValue[CELLS];
lv_obj_t *cellSub[CELLS];
lv_obj_t *lblRate;
lv_obj_t *cellZero[CELLS];
void (*onTareCell_)(uint8_t) = nullptr;

void zeroClicked(lv_event_t *e) {
  const uint8_t i = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
  if (onTareCell_) onTareCell_(i);
}

void styleFlat(lv_obj_t *o) {
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(o, 0, LV_PART_MAIN);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

// --- change detection -------------------------------------------------------
// Held as the FORMATTED strings rather than as the numbers behind them, which
// is the stricter test and the one that matches what the panel actually has to
// repaint. Two totals 0.4 g apart are a different float and the same pixels;
// comparing floats would invalidate the biggest label on the screen for a
// change nobody can see, forty times a second.
char prevTotal_[16] = {0};
char prevUnit_[8] = {0};
char prevCaption_[24] = {0};
char prevFlag_[16] = {0};
char prevCellValue_[CELLS][16] = {{0}, {0}};
char prevCellSub_[CELLS][16] = {{0}, {0}};
char prevRate_[40] = {0};
// The total's colour, cached like everything else. lv_obj_set_style_text_color
// does not compare before acting, and this used to run unguarded on every
// update -- the same "write unconditionally" mistake the rest of this page was
// written to avoid, on the largest object on the screen.
uint32_t prevTotalColor_ = 0;
bool haveTotalColor_ = false;
bool prevCalibrated_ = false;
bool haveCal_ = false;
Cell prevCellState_[CELLS] = {Cell::Offline, Cell::Offline};
bool haveCellState_ = false;

// Writes only when the text actually differs. Every label on this page goes
// through it.
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

lv_obj_t *makeCell(lv_obj_t *parent, uint8_t i, const char *name) {
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_set_height(box, LV_PCT(100));
  lv_obj_set_flex_grow(box, 1);
  lv_obj_set_style_radius(box, 6, LV_PART_MAIN);
  lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(box, lv_color_hex(C_BORDER), LV_PART_MAIN);
  lv_obj_set_style_bg_color(box, lv_color_hex(C_PANEL), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(box, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_row(box, 1, LV_PART_MAIN);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  cellName[i] = lv_label_create(box);
  lv_obj_set_style_text_font(cellName[i], &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(cellName[i], lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(cellName[i], name);

  // Fixed width and centred, for the reason spelled out at lblTotal: these two
  // change as often as the total does, and a content-sized label that changes
  // width re-lays-out its parent every time.
  cellValue[i] = lv_label_create(box);
  lv_obj_set_style_text_font(cellValue[i], &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(cellValue[i], lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_obj_set_width(cellValue[i], LV_PCT(100));
  lv_obj_set_style_text_align(cellValue[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(cellValue[i], LV_LABEL_LONG_CLIP);
  lv_label_set_text(cellValue[i], "--");

  cellSub[i] = lv_label_create(box);
  lv_obj_set_style_text_font(cellSub[i], &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(cellSub[i], lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_obj_set_width(cellSub[i], LV_PCT(100));
  lv_obj_set_style_text_align(cellSub[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(cellSub[i], LV_LABEL_LONG_CLIP);
  lv_label_set_text(cellSub[i], "");

  // The cell's own zero button, INSIDE its panel. Which button belongs to which
  // cell then needs no label, which is the whole reason it is here rather than
  // in a row of two identical buttons underneath.
  //
  // 32 px rather than the 44 the menu rows settled on. It is a deliberate step
  // down: this is a setup control rather than something touched during a
  // service, it is 100 px wide which buys back most of what the height gives
  // up, and the alternative was taking the height out of the readings above it.
  cellZero[i] = lv_button_create(box);
  lv_obj_set_width(cellZero[i], LV_PCT(100));
  lv_obj_set_height(cellZero[i], 32);
  lv_obj_set_style_radius(cellZero[i], 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(cellZero[i], lv_color_hex(C_KEY), LV_PART_MAIN);
  lv_obj_set_style_margin_top(cellZero[i], 4, LV_PART_MAIN);
  lv_obj_add_event_cb(cellZero[i], zeroClicked, LV_EVENT_CLICKED,
                      (void *)(uintptr_t)i);
  lv_obj_t *zl = lv_label_create(cellZero[i]);
  lv_obj_set_style_text_font(zl, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(zl, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_text(zl, "TARE 0");
  lv_obj_center(zl);

  cellBox[i] = box;
  return box;
}

}  // namespace

void buildWeight(lv_obj_t *parent) {
  lv_obj_t *scr = parent ? parent : lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_text_color(scr, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(scr, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(scr, 4, LV_PART_MAIN);
  lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lblCaption = lv_label_create(scr);
  lv_obj_set_style_text_font(lblCaption, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblCaption, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_obj_set_width(lblCaption, LV_PCT(100));
  lv_obj_set_style_text_align(lblCaption, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(lblCaption, LV_LABEL_LONG_CLIP);
  lv_label_set_text(lblCaption, "total");

  // The number and its unit are ONE ROW with their bottoms aligned, not a
  // centred stack. A unit floating beside the vertical middle of a 48 px glyph
  // reads as a separate fact; sitting on the baseline it reads as part of the
  // same figure, which is what it is.
  lv_obj_t *row = lv_obj_create(scr);
  styleFlat(row);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 4, LV_PART_MAIN);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                        LV_FLEX_ALIGN_CENTER);

  // EVERY LABEL ON THIS PAGE HAS A FIXED WIDTH, AND THAT IS THE WHOLE
  // PERFORMANCE STORY.
  //
  // A content-sized label re-measures itself on every lv_label_set_text and,
  // when the width comes out different, calls lv_obj_mark_layout_as_dirty on
  // its parent. The parent here is a flex row inside a flex column, so one
  // changed digit re-runs the layout, MOVES the siblings -- a centred row
  // shifts everything when its content grows -- and invalidates the old and new
  // position of each. On the bowl branch that happened when a bowl moved, a few
  // times per service. Here the number is live ADC counts, so it happened
  // twenty times a second, and the panel measured:
  //
  //     fps 3  ui 86% (flush 3% touch 0%)  worst 301ms | 11574px/f
  //
  // Three per cent in the flush and nothing in the touch read, yet 250 ms a
  // frame to push eleven thousand pixels. That is not blitting cost and it is
  // not the bus; it is the layout being recomputed and the page being
  // re-invalidated, over and over, for a digit.
  //
  // A fixed width removes the trigger entirely: the text changes, the box does
  // not, so nothing above the label learns about it and only the label's own
  // rectangle is invalidated.
  //
  // It is also simply better to look at. Right-aligned digits in a fixed box
  // grow leftward from a stationary edge, the way every scale and every meter
  // displays a number, instead of jittering horizontally as the value moves.
  //
  //   186 + 4 (column pad) + 34 = 224, which is the 240 px panel less the 8 px
  //   page padding on each side. 186 px holds six 48 px digits; the largest
  //   honest reading on a 2 x 20 kg platform is 40000, five.
  lblTotal = lv_label_create(row);
  lv_obj_set_style_text_font(lblTotal, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_set_width(lblTotal, 186);
  lv_obj_set_style_text_align(lblTotal, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
  // CLIP rather than the default WRAP. Wrapping would put a second line under
  // the first, which changes the label's HEIGHT -- and a height change marks
  // the layout dirty exactly the way the width change this fix removes. A
  // clipped digit would be a bug worth seeing; a relayout storm would not be.
  lv_label_set_long_mode(lblTotal, LV_LABEL_LONG_CLIP);
  lv_label_set_text(lblTotal, "--");

  lblUnit = lv_label_create(row);
  lv_obj_set_style_text_font(lblUnit, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblUnit, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_obj_set_width(lblUnit, 34);
  lv_obj_set_style_text_align(lblUnit, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  lv_label_set_long_mode(lblUnit, LV_LABEL_LONG_CLIP);
  // 6 px of bottom padding rather than a margin, so the label's own box carries
  // the offset and the row's END alignment still has something to align to.
  lv_obj_set_style_pad_bottom(lblUnit, 6, LV_PART_MAIN);
  lv_label_set_text(lblUnit, "");

  // Shown only when something is wrong or unproven -- no calibration, no tare,
  // a cell missing. A permanent "OK" badge teaches people to stop reading the
  // area, which is the opposite of what a status chip is for.
  lblFlag = lv_label_create(scr);
  lv_obj_set_style_text_font(lblFlag, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(lblFlag, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(lblFlag, lv_color_hex(C_WARN), LV_PART_MAIN);
  lv_obj_set_style_radius(lblFlag, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_all(lblFlag, 3, LV_PART_MAIN);
  lv_label_set_text(lblFlag, "");
  lv_obj_add_flag(lblFlag, LV_OBJ_FLAG_HIDDEN);

  // --- the two cells -------------------------------------------------------
  lv_obj_t *cells = lv_obj_create(scr);
  styleFlat(cells);
  lv_obj_set_width(cells, LV_PCT(100));
  lv_obj_set_flex_grow(cells, 1);
  lv_obj_set_style_pad_column(cells, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_top(cells, 4, LV_PART_MAIN);
  lv_obj_set_flex_flow(cells, LV_FLEX_FLOW_ROW);

  makeCell(cells, 0, "A");
  makeCell(cells, 1, "B");

  // Diagnostics rather than information, so it sits at the bottom of the type
  // scale where it cannot compete with the figure above it. It earns its place
  // on this branch specifically: "as fast as possible" is a claim, and this is
  // the line that either supports it or does not.
  lblRate = lv_label_create(scr);
  lv_obj_set_style_text_font(lblRate, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblRate, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_obj_set_width(lblRate, LV_PCT(100));
  lv_obj_set_style_text_align(lblRate, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(lblRate, LV_LABEL_LONG_CLIP);
  lv_label_set_text(lblRate, "");
}

void weightOnTareCell(void (*cb)(uint8_t)) { onTareCell_ = cb; }

void updateWeight(const State &st) {
  const ScaleView &s = st.scale;
  char buf[40];

  // --- the total -----------------------------------------------------------
  // TWO DISPLAY MODES, AND THE UNCALIBRATED ONE IS NOT A DEGRADED VERSION OF
  // THE OTHER. Without a known mass there is no counts-to-grams factor, so
  // there is no weight to show -- only the converter's own output. Printing
  // counts and labelling them "cts" says exactly that; printing them as grams
  // with an assumed factor would be a confident wrong number, which is the one
  // thing this codebase refuses everywhere.
  const bool calChanged = !haveCal_ || prevCalibrated_ != s.calibrated;
  if (calChanged) {
    haveCal_ = true;
    prevCalibrated_ = s.calibrated;
    // Counts run to seven digits and a sign; grams to five. They cannot share
    // a size, so the size follows the mode -- set here, on the transition,
    // rather than per frame.
    lv_obj_set_style_text_font(lblTotal,
                               s.calibrated ? &lv_font_montserrat_48 : &lv_font_montserrat_28,
                               LV_PART_MAIN);
  }

  if (s.online == 0) {
    // Nothing is converting, so there is no total. Dashes rather than a zero:
    // on a scale, zero is a measurement someone might act on.
    setIfChanged(lblTotal, prevTotal_, sizeof(prevTotal_), "--");
    setIfChanged(lblUnit, prevUnit_, sizeof(prevUnit_), "");
    setTotalColor(C_MUTED);
    setIfChanged(lblCaption, prevCaption_, sizeof(prevCaption_), "total");
  } else {
    if (s.calibrated) {
      // KILOGRAMS TO THREE DECIMALS -- so the resolution on the screen is one
      // gram, which is roughly what the hardware actually delivers. The measured
      // noise floor is about +/-2 g, so the last digit moves by a couple either
      // way and that is the measurement rather than a rendering artefact.
      //
      // Not grams-as-an-integer, which would read "6820" and make a kitchen
      // quantity look like a part number, and not two decimals, which would
      // throw away a digit the cells can genuinely resolve.
      //
      // lroundf on the GRAM value before dividing, rather than "%.3f" on the
      // float: printf rounds in binary and 1.0005 is not representable, so the
      // last digit could disagree with the per-cell figures derived from the
      // same counts.
      const long mg = (long)lroundf(s.totalGrams);
      snprintf(buf, sizeof(buf), "%s%ld.%03ld", mg < 0 ? "-" : "",
               (long)(mg < 0 ? -mg : mg) / 1000L, (long)(mg < 0 ? -mg : mg) % 1000L);
    } else {
      snprintf(buf, sizeof(buf), "%ld", (long)s.totalCounts);
    }
    setIfChanged(lblTotal, prevTotal_, sizeof(prevTotal_), buf);
    setIfChanged(lblUnit, prevUnit_, sizeof(prevUnit_), s.calibrated ? "kg" : "cts");
    setTotalColor(s.overRange ? C_CELL_FAULT : C_TEXT);

    // "AT LEAST", NOT "TOTAL", WHEN A CELL IS MISSING -- the same word the bowl
    // page uses for a degraded count, and for the same reason. The sum of the
    // cells that ARE working is a genuine lower bound on the load, because the
    // missing corner cannot be carrying a negative share of it. What it is not
    // is the total, and a caption that said so would turn a useful partial
    // measurement into a wrong complete one.
    const bool partial = s.online < CELLS;
    const char *cap = s.calibrated ? (partial ? "at least" : "total")
                                   : (partial ? "at least, uncalibrated"
                                              : "total, uncalibrated");
    setIfChanged(lblCaption, prevCaption_, sizeof(prevCaption_), cap);
  }

  // --- the flag ------------------------------------------------------------
  // One chip, most serious first. A missing cell outranks a missing
  // calibration, which outranks a missing tare -- and only one of the three is
  // ever shown, because a stack of badges on a 240 px screen is a wall rather
  // than a warning.
  bool anyWarming = false;
  for (uint8_t i = 0; i < CELLS; i++)
    if (s.cell[i].state == Cell::Warming) anyWarming = true;

  const char *flag = "";
  uint32_t flagColor = C_WARN;
  if (s.overRange) {
    // ABOVE the calibration and tare warnings, because it invalidates the
    // number rather than qualifying it. A saturated converter has stopped
    // measuring; everything below is about a reading that is merely unproven.
    flag = "OVER RANGE";
    flagColor = C_FAULT;
  } else if (s.online == 0 && anyWarming) {
    // Configured and not yet converting. Distinct from silence, and it resolves
    // itself within a sample period -- telling someone to check their wiring
    // for a second and a half is how a working device gets taken apart.
    flag = "warming up";
  } else if (s.online < CELLS) {
    flag = (s.online == 0) ? "no cell talking" : "one cell down";
    flagColor = C_FAULT;
  } else if (!s.tared) {
    // NO "no calibration" CHIP. The caption under the number already reads
    // "total, uncalibrated" and the unit already reads "cts" rather than "kg",
    // so a badge saying it a third time was noise -- and a chip that is lit in
    // the ordinary state teaches people to stop reading the area, which is the
    // opposite of what a chip is for. The two remaining ones both mean
    // something is WRONG.
    flag = "not tared";
  }
  if (strncmp(prevFlag_, flag, sizeof(prevFlag_) - 1) != 0) {
    snprintf(prevFlag_, sizeof(prevFlag_), "%s", flag);
    if (flag[0]) {
      lv_label_set_text(lblFlag, flag);
      lv_obj_set_style_bg_color(lblFlag, lv_color_hex(flagColor), LV_PART_MAIN);
      lv_obj_remove_flag(lblFlag, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(lblFlag, LV_OBJ_FLAG_HIDDEN);
    }
  }

  // --- per cell ------------------------------------------------------------
  for (uint8_t i = 0; i < CELLS; i++) {
    const CellView &c = s.cell[i];

    if (!haveCellState_ || prevCellState_[i] != c.state) {
      prevCellState_[i] = c.state;
      // Colour carries cell health, the way it carries level state on the bowl
      // page. A dead cell is the one thing here that has to be noticed without
      // reading anything.
      const bool bad = c.state != Cell::Online;
      lv_obj_set_style_border_color(
          cellBox[i], lv_color_hex(bad ? C_CELL_FAULT : C_BORDER), LV_PART_MAIN);
    }

    if (c.state == Cell::Online) {
      if (c.overRange) {
        // The number this cell would print is its own ceiling, so it does not
        // get printed. One unit on the screen, and no figure that looks like a
        // weight when it is not one.
        snprintf(buf, sizeof(buf), "OVER");
      } else if (s.calibrated) {
        // The same unit as the total, deliberately. Two units on one screen is
        // how a 3 kg reading gets read as 3 g, and these two figures are meant
        // to be added up by eye against the number above them.
        const long mg = (long)lroundf(c.grams);
        snprintf(buf, sizeof(buf), "%s%ld.%03ld", mg < 0 ? "-" : "",
                 (long)(mg < 0 ? -mg : mg) / 1000L, (long)(mg < 0 ? -mg : mg) % 1000L);
      } else {
        snprintf(buf, sizeof(buf), "%ld", (long)c.counts);
      }
      setIfChanged(cellValue[i], prevCellValue_[i], sizeof(prevCellValue_[i]), buf);

      // The MEASURED rate, not the configured one. On a bit-banged bus polled
      // from a task that shares a core with the renderer, those are different
      // claims, and this is the one that can be checked.
      snprintf(buf, sizeof(buf), "%u/s", c.sps);
      setIfChanged(cellSub[i], prevCellSub_[i], sizeof(prevCellSub_[i]), buf);
    } else {
      setIfChanged(cellValue[i], prevCellValue_[i], sizeof(prevCellValue_[i]), "--");
      setIfChanged(cellSub[i], prevCellSub_[i], sizeof(prevCellSub_[i]),
                   c.state == Cell::Warming ? "warming" : "offline");
    }
  }
  haveCellState_ = true;

  // --- the rate line -------------------------------------------------------
  // fps comes from LV_EVENT_REFR_READY, so it counts COMPLETED REFRESHES rather
  // than loop iterations. A low figure here with a steady weight is the renderer
  // correctly declining to redraw what has not changed, not a stall -- see
  // ui_perf.h. It only means something while the number is moving.
  snprintf(buf, sizeof(buf), "%u fps   ui %u%%", perfFps(), perfUiPct());
  setIfChanged(lblRate, prevRate_, sizeof(prevRate_), buf);
}

}  // namespace ui
