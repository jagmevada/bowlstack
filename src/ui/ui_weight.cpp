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

lv_obj_t *lblCaption;
lv_obj_t *lblTotal;
lv_obj_t *lblUnit;
lv_obj_t *lblFlag;
lv_obj_t *cellBox[CELLS];
lv_obj_t *cellName[CELLS];
lv_obj_t *cellValue[CELLS];
lv_obj_t *cellSub[CELLS];
lv_obj_t *lblRate;

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

  cellValue[i] = lv_label_create(box);
  lv_obj_set_style_text_font(cellValue[i], &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(cellValue[i], lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_text(cellValue[i], "--");

  cellSub[i] = lv_label_create(box);
  lv_obj_set_style_text_font(cellSub[i], &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(cellSub[i], lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(cellSub[i], "");

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

  lblTotal = lv_label_create(row);
  lv_obj_set_style_text_font(lblTotal, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_label_set_text(lblTotal, "--");

  lblUnit = lv_label_create(row);
  lv_obj_set_style_text_font(lblUnit, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblUnit, lv_color_hex(C_MUTED), LV_PART_MAIN);
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
  lv_label_set_text(lblRate, "");
}

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
    lv_obj_set_style_text_color(lblTotal, lv_color_hex(C_MUTED), LV_PART_MAIN);
    setIfChanged(lblCaption, prevCaption_, sizeof(prevCaption_), "total");
  } else {
    if (s.calibrated) {
      snprintf(buf, sizeof(buf), "%ld", (long)lroundf(s.totalGrams));
    } else {
      snprintf(buf, sizeof(buf), "%ld", (long)s.totalCounts);
    }
    setIfChanged(lblTotal, prevTotal_, sizeof(prevTotal_), buf);
    setIfChanged(lblUnit, prevUnit_, sizeof(prevUnit_), s.calibrated ? "g" : "cts");
    lv_obj_set_style_text_color(lblTotal, lv_color_hex(C_TEXT), LV_PART_MAIN);

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
  if (s.online == 0 && anyWarming) {
    // Configured and not yet converting. Distinct from silence, and it resolves
    // itself within a sample period -- telling someone to check their wiring
    // for a second and a half is how a working device gets taken apart.
    flag = "warming up";
  } else if (s.online < CELLS) {
    flag = (s.online == 0) ? "no cell talking" : "one cell down";
    flagColor = C_FAULT;
  } else if (!s.calibrated) {
    flag = "no calibration";
  } else if (!s.tared) {
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
      if (s.calibrated) snprintf(buf, sizeof(buf), "%ld g", (long)lroundf(c.grams));
      else snprintf(buf, sizeof(buf), "%ld", (long)c.counts);
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
