// Weight view -- the primary screen, and now ONLY that.
//
// It used to carry the per-cell breakdown as well: two panels with each cell's
// share, its rate and its own zero button. All of that is real information and
// none of it belongs here. A serving station's dashboard is read at a glance,
// from a distance, by somebody carrying a bowl -- and every extra figure on it
// competes with the one number they walked over to see.
//
// So the breakdown moved to Settings > Diagnose, where it is read deliberately
// by somebody who came looking for it, and what is left is the total and one
// button that zeroes every cell.
//
// LAID OUT WITH FLEX, like every other page here. Arithmetic against a fixed
// anchor cannot express "these must not overlap", it can only happen to satisfy
// it for one font and one string length -- and this page's number changes WIDTH
// as the load changes.

#include "ui_weight.h"

#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_font.h"

namespace ui {
namespace {

const uint32_t C_BG = 0x000000;
const uint32_t C_TEXT = 0xE6EDF3;
const uint32_t C_MUTED = 0x8B949E;
const uint32_t C_WARN = 0x9E6A03;
const uint32_t C_FAULT = 0xB62324;
const uint32_t C_CELL_FAULT = 0xE05C5C;
const uint32_t C_KEY = 0x21262D;

lv_obj_t *lblCaption;
lv_obj_t *lblTotal;
lv_obj_t *lblUnit;

// --- the per-cell breakdown ------------------------------------------------
// Off by default and shown from Settings > Scale > Cells. It is a SETUP and
// DIAGNOSIS view living on the dashboard, which is a deliberate exception to
// the rule that put the breakdown on the Diagnose page: watching three corners
// while levelling a platform means watching them WHILE loading it, and walking
// to a menu between adjustments is how a fault gets attributed to the wrong
// corner.
//
// Deliberately small. The total is the number this device exists to show; three
// supporting figures at 20 px read as what they are without competing with it.
lv_obj_t *cellBox = nullptr;
lv_obj_t *lblCellVal[CELLS] = {nullptr, nullptr, nullptr};
char prevCell_[CELLS][20] = {{0}, {0}, {0}};
lv_obj_t *lblFlag;
lv_obj_t *btnTare;
lv_obj_t *btnSettings;

void (*onTare_)(void) = nullptr;
void (*onSettings_)(void) = nullptr;

void tareClicked(lv_event_t *) {
  if (onTare_) onTare_();
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
// Held as the FORMATTED strings rather than the numbers behind them, which is
// the stricter test and the one that matches what the panel actually has to
// repaint. Two totals 0.4 g apart are a different float and the same pixels;
// comparing floats would invalidate the biggest label on the screen for a
// change nobody can see, ten times a second.
char prevTotal_[16] = {0};
char prevUnit_[8] = {0};
char prevCaption_[24] = {0};
char prevFlag_[16] = {0};
uint32_t prevTotalColor_ = 0;
bool haveTotalColor_ = false;
bool prevCalibrated_ = false;
bool haveCal_ = false;

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
// lroundf and then an integer split, rather than "%.*f" on the float: printf
// rounds in binary and 1.0005 is not representable, so the last digit could
// disagree with the same value shown elsewhere from the same counts.
//
// THE ROUNDING HAPPENS AT THE DISPLAYED PLACE, not at the gram and then again
// at the print. Rounding twice is how 1.4996 kg becomes 1.500 becomes 1.50 --
// each step defensible, the pair of them wrong. So the gram figure is divided
// by the size of one displayed step and rounded ONCE:
//
//     decimals   step     249 g reads
//     1          100 g    0.2 kg
//     2           10 g    0.25 kg
//     3            1 g    0.249 kg
//
// And it is ROUNDING, not truncation. Truncating would build a scale that
// reads consistently light -- by up to a whole unit of the last place shown,
// which at one decimal is 99 g of food that nobody is charged for.
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

}  // namespace

void weightOnTare(void (*cb)(void)) { onTare_ = cb; }
void weightOnSettings(void (*cb)(void)) { onSettings_ = cb; }

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

  // The number and its unit are ONE ROW with their bottoms aligned. A unit
  // floating beside the vertical middle of a 48 px glyph reads as a separate
  // fact; on the baseline it reads as part of the same figure, which it is.
  lv_obj_t *row = lv_obj_create(scr);
  styleFlat(row);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 4, LV_PART_MAIN);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                        LV_FLEX_ALIGN_CENTER);

  // FIXED WIDTH, and it is the whole performance story of this page. A
  // content-sized label re-measures on every set_text and, when the width comes
  // out different, marks its flex parent's layout dirty -- which re-runs the
  // layout, MOVES the siblings in a centred row and invalidates their old and
  // new positions. With a live number that happened ten times a second and cost
  // 250 ms a frame to push eleven thousand pixels.
  //
  // Right-aligned digits in a fixed box also grow leftward from a stationary
  // edge, the way every scale displays a number, instead of jittering sideways.
  // 56 px, which is LARGER THAN LVGL PROVIDES -- its built-in Montserrat stops
  // at 48 and this label was already there, so the size had to be generated.
  // See ui_font.h for the subset and for why 56 and not 64.
  //
  // The box grew from 186 to 194 with it, taking the 8 px from the unit label
  // beside it: at 56 px the worst three-decimal reading is ~193 px wide, and a
  // number that clips its leading digit is worse than a smaller number.
  lblTotal = lv_label_create(row);
  lv_obj_set_style_text_font(lblTotal, &font_mass_56, LV_PART_MAIN);
  lv_obj_set_width(lblTotal, 194);
  lv_obj_set_style_text_align(lblTotal, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
  // CLIP rather than WRAP: wrapping puts a second line underneath, which
  // changes the label's HEIGHT and marks the layout dirty exactly the way the
  // width change this avoids.
  lv_label_set_long_mode(lblTotal, LV_LABEL_LONG_CLIP);
  lv_label_set_text(lblTotal, "--");

  lblUnit = lv_label_create(row);
  lv_obj_set_style_text_font(lblUnit, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblUnit, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_obj_set_width(lblUnit, 26);
  lv_obj_set_style_text_align(lblUnit, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  lv_label_set_long_mode(lblUnit, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_pad_bottom(lblUnit, 6, LV_PART_MAIN);
  lv_label_set_text(lblUnit, "");

  // --- the per-cell rows --------------------------------------------------
  // Hidden unless the Cells setting is on. Built either way, because building
  // them on demand would mean a layout change on a page that is already redrawn
  // ten times a second -- and nine small labels cost about nothing, unlike the
  // keyboard that justified lazy construction elsewhere.
  cellBox = lv_obj_create(scr);
  styleFlat(cellBox);
  lv_obj_set_width(cellBox, LV_PCT(100));
  lv_obj_set_height(cellBox, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(cellBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(cellBox, 2, LV_PART_MAIN);
  lv_obj_add_flag(cellBox, LV_OBJ_FLAG_HIDDEN);

  for (uint8_t i = 0; i < CELLS; i++) {
    lv_obj_t *r = lv_obj_create(cellBox);
    styleFlat(r);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r, 4, LV_PART_MAIN);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // TWO FIXED-WIDTH LABELS, not one string with padding. A proportional font
    // does not line up columns that were aligned with spaces, and the total
    // above it already documents what a content-sized label costs on a page
    // that redraws at 10 Hz.
    lv_obj_t *name = lv_label_create(r);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, lv_color_hex(C_MUTED), LV_PART_MAIN);
    lv_obj_set_width(name, 22);
    lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
    const char nm[2] = {(char)('A' + i), '\0'};
    lv_label_set_text(name, nm);

    lblCellVal[i] = lv_label_create(r);
    lv_obj_set_style_text_font(lblCellVal[i], &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_width(lblCellVal[i], 194);
    lv_obj_set_style_text_align(lblCellVal[i], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_long_mode(lblCellVal[i], LV_LABEL_LONG_CLIP);
    lv_label_set_text(lblCellVal[i], "--");
  }

  // Shown only when something is wrong or unproven. A chip that is lit in the
  // ordinary state teaches people to stop reading the area, which is the
  // opposite of what a status chip is for.
  lblFlag = lv_label_create(scr);
  lv_obj_set_style_text_font(lblFlag, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(lblFlag, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(lblFlag, lv_color_hex(C_WARN), LV_PART_MAIN);
  lv_obj_set_style_radius(lblFlag, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_all(lblFlag, 3, LV_PART_MAIN);
  lv_label_set_text(lblFlag, "");
  lv_obj_add_flag(lblFlag, LV_OBJ_FLAG_HIDDEN);

  // A spacer that eats the slack, so the button sits at the bottom of the page
  // rather than floating directly under a number whose height varies with the
  // font the calibration state selects.
  lv_obj_t *gap = lv_obj_create(scr);
  styleFlat(gap);
  lv_obj_set_width(gap, LV_PCT(100));
  lv_obj_set_flex_grow(gap, 1);

  // --- the action row -----------------------------------------------------
  // TARE and a way off this page, side by side at the bottom where a thumb
  // reaches. Both are 64 px tall, which is well over the 44 px the menu rows
  // settled on -- this is the one screen used with a bowl in the other hand,
  // and there is nothing else on it to spend the room on.
  lv_obj_t *actions = lv_obj_create(scr);
  styleFlat(actions);
  lv_obj_set_width(actions, LV_PCT(100));
  lv_obj_set_height(actions, 64);
  lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(actions, 8, LV_PART_MAIN);
  lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  // TARES EVERY CELL. Zeroing one corner against the others is a setup job and
  // lives on Diagnose with the figures that make it meaningful.
  btnTare = lv_button_create(actions);
  lv_obj_set_flex_grow(btnTare, 1);
  lv_obj_set_height(btnTare, 64);
  lv_obj_set_style_radius(btnTare, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_color(btnTare, lv_color_hex(C_KEY), LV_PART_MAIN);
  lv_obj_add_event_cb(btnTare, tareClicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *tl = lv_label_create(btnTare);
  lv_obj_set_style_text_font(tl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(tl, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_text(tl, "TARE");
  lv_obj_center(tl);

  // A BUTTON RATHER THAN A SWIPE, and the swipe still works alongside it.
  //
  // Dragging across this panel has to be deliberate to register: the touch
  // controller is polled at roughly 30 Hz through a driver that retries on
  // every read, so a quick flick does not produce enough samples to be read as
  // a drag at all. That is fine for a gesture somebody discovers once and
  // tolerable for one they use occasionally; it is the wrong primary way to
  // reach settings from a screen used one-handed.
  btnSettings = lv_button_create(actions);
  lv_obj_set_size(btnSettings, 64, 64);
  lv_obj_set_style_radius(btnSettings, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_color(btnSettings, lv_color_hex(C_KEY), LV_PART_MAIN);
  lv_obj_add_event_cb(btnSettings, settingsClicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *sl = lv_label_create(btnSettings);
  lv_obj_set_style_text_font(sl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(sl, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(sl, LV_SYMBOL_SETTINGS);
  lv_obj_center(sl);

  // NO PERF LINE. "fps 13  ui 62%" was here while the frame rate was the thing
  // being worked on, and it earned its place then -- it is what caught the
  // 3 fps relayout storm. It has no business on a serving station's dashboard:
  // it is a number about the SOFTWARE on a screen whose whole job is to show
  // one number about the FOOD. The same figures are still measured and still go
  // to the console every five seconds, and the scope page shows them live.
}

void updateWeight(const State &st) {
  const ScaleView &s = st.scale;
  char buf[40];

  // TWO DISPLAY MODES, and the uncalibrated one is not a degraded version of
  // the other. Without a known mass there is no counts-to-grams factor, so
  // there is no weight to show -- only the converter's own output. Printing
  // counts and labelling them "cts" says exactly that; printing them as
  // kilograms with an assumed factor would be a confident wrong number.
  const bool calChanged = !haveCal_ || prevCalibrated_ != s.calibrated;
  if (calChanged) {
    haveCal_ = true;
    prevCalibrated_ = s.calibrated;
    // Counts run to seven digits and a sign; kilograms to six characters. They
    // cannot share a size, so the size follows the mode -- set here, on the
    // transition, rather than per frame.
    //
    // The uncalibrated side stays on the BUILT-IN Montserrat 28 rather than a
    // generated size, because eight glyphs at 56 px do not fit a 240 px panel
    // under any label width. It is also the size that keeps the subset font
    // honest: font_mass_56 has digits and nothing else, and counts need nothing
    // else either, but there is no reason to spend a second generated size on a
    // display mode that exists only until somebody calibrates the unit.
    lv_obj_set_style_text_font(
        lblTotal, s.calibrated ? &font_mass_56 : &lv_font_montserrat_28, LV_PART_MAIN);
  }

  if (s.online == 0) {
    // Nothing is converting, so there is no total. Dashes rather than a zero:
    // on a scale, zero is a measurement somebody might act on.
    setIfChanged(lblTotal, prevTotal_, sizeof(prevTotal_), "--");
    setIfChanged(lblUnit, prevUnit_, sizeof(prevUnit_), "");
    setTotalColor(C_MUTED);
    setIfChanged(lblCaption, prevCaption_, sizeof(prevCaption_), "total");
  } else {
    if (s.calibrated) formatKg(buf, sizeof(buf), s.totalGrams, s.decimals);
    else snprintf(buf, sizeof(buf), "%ld", (long)s.totalCounts);
    setIfChanged(lblTotal, prevTotal_, sizeof(prevTotal_), buf);
    setIfChanged(lblUnit, prevUnit_, sizeof(prevUnit_), s.calibrated ? "kg" : "cts");
    setTotalColor(s.overRange ? C_CELL_FAULT : C_TEXT);

    // "AT LEAST", NOT "TOTAL", WHEN A CELL IS MISSING. The sum of the cells
    // that ARE working is a genuine lower bound on the load, because the
    // missing corner cannot carry a negative share of it. What it is not is the
    // total, and a caption saying so would turn a useful partial measurement
    // into a wrong complete one.
    const bool partial = s.online < CELLS;
    const char *cap = s.calibrated
                          ? (partial ? "at least" : "total")
                          : (partial ? "at least, uncalibrated" : "total, uncalibrated");
    setIfChanged(lblCaption, prevCaption_, sizeof(prevCaption_), cap);
  }

  // --- the flag ------------------------------------------------------------
  // One chip, most serious first. Only one is ever shown: a stack of badges on
  // a 240 px screen is a wall rather than a warning.
  bool anyWarming = false;
  for (uint8_t i = 0; i < CELLS; i++)
    if (s.cell[i].state == Cell::Warming) anyWarming = true;

  const char *flag = "";
  uint32_t flagColor = C_WARN;
  if (s.overRange) {
    // Above the calibration and tare warnings, because it invalidates the
    // number rather than qualifying it. A saturated converter has stopped
    // measuring; everything below is about a reading that is merely unproven.
    flag = "OVER RANGE";
    flagColor = C_FAULT;
  } else if (s.online == 0 && anyWarming) {
    flag = "warming up";
  } else if (s.online < CELLS) {
    // COUNTED, not named. "one cell down" was right for two cells and would be
    // wrong for three in the case that matters most -- two corners lost out of
    // three, reported as one.
    static char down[16];
    if (s.online == 0) {
      flag = "no cell talking";
    } else {
      const uint8_t missing = (uint8_t)(CELLS - s.online);
      snprintf(down, sizeof(down), "%u cell%s down", missing, missing == 1 ? "" : "s");
      flag = down;
    }
    flagColor = C_FAULT;
  } else if (!s.tared) {
    // The automatic power-up tare normally clears this within a few seconds of
    // boot. Seeing it persist means the platform never held still long enough,
    // which is worth knowing before trusting the number above it.
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

  // --- the per-cell rows ---------------------------------------------------
  // The visibility is driven every frame but only WRITTEN on a change, the same
  // discipline every other widget on this page follows: a flag toggle
  // invalidates the object, and doing it ten times a second for a value that
  // changes when somebody opens a menu is ten redraws a second for nothing.
  if (cellBox) {
    const bool wantCells = s.showCells;
    const bool shown = !lv_obj_has_flag(cellBox, LV_OBJ_FLAG_HIDDEN);
    if (wantCells != shown) {
      if (wantCells) lv_obj_remove_flag(cellBox, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(cellBox, LV_OBJ_FLAG_HIDDEN);
    }
    if (wantCells) {
      for (uint8_t i = 0; i < CELLS; i++) {
        const CellView &c = s.cell[i];
        char cb[20];
        if (c.state != Cell::Online) {
          // The SAME refusal the total makes, per corner. A cell that is not
          // converting has no share -- printing its last one, or a zero, is the
          // one thing this dashboard exists not to do.
          snprintf(cb, sizeof(cb), "%s", c.state == Cell::Warming ? "warming" : "offline");
        } else if (s.calibrated) {
          formatKg(cb, sizeof(cb), c.grams, s.decimals);
          const size_t n = strlen(cb);
          snprintf(cb + n, sizeof(cb) - n, " kg%s", c.overRange ? " !" : "");
        } else {
          snprintf(cb, sizeof(cb), "%ld cts", (long)c.counts);
        }
        setIfChanged(lblCellVal[i], prevCell_[i], sizeof(prevCell_[i]), cb);
      }
    }
  }
}

}  // namespace ui
