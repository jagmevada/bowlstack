// Weight view -- the primary screen, and now ONLY that.
//
// It used to carry the per-cell breakdown as well: two panels with each cell's
// share, its rate and its own zero button. All of that is real information and
// none of it belongs here. A serving station's dashboard is read at a glance,
// from a distance, by somebody carrying a bowl -- and every extra figure on it
// competes with the one number they walked over to see.
//
// So the breakdown moved to Settings > Diagnose, where it is read deliberately
// by somebody who came looking for it, and what is left is the total.
//
// THE TARE BUTTON WENT THE SAME WAY, and for the opposite reason. Not that it
// was hard to read -- that it was too easy to hit. It was the widest target on
// the page, at thumb height, on the one screen used with a bowl in the other
// hand, and it was getting pressed by accident in service. A stray tap on it
// does not clutter the display or lose a keystroke: it silently redefines zero,
// and every reading afterwards inherits that. It is on Settings > Scale > Tare
// now, three taps deep, and that row navigates BACK here when it fires so the
// total on this page is the confirmation. See doTare() in ui_pages.cpp.
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
// The same blue ui_screens.cpp uses for an occupied level, and chosen here for
// the reason stated there: it "says occupied without editorialising, which
// leaves green and red free to mean healthy and faulty on the same screen".
// Duplicated rather than shared because these constants are already duplicated
// per file in src/ui/ -- see the block above.
const uint32_t C_PRESENT = 0x1F6FEB;

lv_obj_t *lblCaption;
// The vessel offset in force, shown at the left end of the caption row. Its own
// change guard: it moves only when somebody cycles the setting.
lv_obj_t *lblOffset_ = nullptr;
char prevOffset_[12] = {0};
lv_obj_t *lblTotal;

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
// The 200 kg buffer line, between the total and the per-cell rows. A separate
// instrument: nothing here is summed into, or scaled by, the counter's figures.
lv_obj_t *bufRow_ = nullptr;
lv_obj_t *weightScr_ = nullptr;  // the page, to close up its row gaps when it is crowded
bool prevCrowded_ = false;
int8_t prevBufShown_ = -1;       // -1 = never decided, so the first frame is logged
lv_obj_t *lblBufVal_ = nullptr;
char prevBuf_[24] = {0};
uint32_t prevBufColor_ = 0;
bool haveBufColor_ = false;

lv_obj_t *cellBox = nullptr;
lv_obj_t *lblCellVal[CELLS] = {nullptr, nullptr, nullptr};
char prevCell_[CELLS][20] = {{0}, {0}, {0}};
// Each corner's share of the load, beside the load itself. Guarded separately
// from the value: the two change on different frames -- a bowl slid sideways
// moves the shares and not the total -- and one string comparison is cheaper
// than the invalidate it prevents.
lv_obj_t *lblCellPct[CELLS] = {nullptr, nullptr, nullptr};
char prevPct_[CELLS][8] = {{0}, {0}, {0}};
uint32_t prevPctColor_[CELLS] = {0, 0, 0};
bool havePctColor_[CELLS] = {false, false, false};
lv_obj_t *lblFlag;
lv_obj_t *btnSettings;

// --- the knob row ----------------------------------------------------------
// One line directly above the action row: a press dot, then the encoder's
// position. It is bring-up instrumentation on the dashboard, deliberately, and
// deliberately quiet -- 14 px and muted, the diagnostics tier, so it reads as
// something to consult rather than something to watch.
lv_obj_t *encDot = nullptr;
lv_obj_t *lblEnc = nullptr;
char prevEnc_[20] = {0};

// TRIAL: the manual fill estimate, on the same row and larger than the raw
// encoder figure beside it -- it is the number the attendant sets and the one
// the experiment is about, where the detent count is bring-up instrumentation.
lv_obj_t *lblFill = nullptr;
// 32, not 16. The stale form is "37%  (14 min old)" -- eighteen characters plus
// a terminator -- so at 16 the snprintf truncated, the truncated string never
// equalled the freshly formatted one, and the guard below failed on EVERY frame
// once the estimate went stale. A label rewritten and invalidated twenty times
// a second, permanently, on the page most likely to be left open.
//
// The buffer that holds a cached string for comparison has to be able to hold
// the longest string it will ever compare.
char prevFill_[32] = {0};

// The dot is held for half a second after each press. LVGL's own tick is the
// clock rather than anything passed in: src/ui/ must compile on a desktop that
// has no millis(), and lv_tick_get() is the one time source both targets share.
//
// ARMED IS NOT OPTIONAL, and the first version of this got it wrong in a way
// worth recording. It kept only the deadline and tested
// `(int32_t)(deadline - now) > 0`, with a comment claiming the signed
// difference made it wrap-safe. It does not. A signed tick comparison is only
// correct while the deadline is within +/-24.9 days of now, and an unset or
// long-expired deadline has UNBOUNDED age: with the sentinel 0 still in place,
// `(int32_t)(0 - now)` turns positive the moment uptime passes 2^31 ms and the
// dot latches solid for the next 24.9 days, asserting a press nobody made.
//
// A mains-fed station runs for months. scale_telemetry.cpp publishes uptime_s
// precisely because long uptimes are the normal case, and nothing in this image
// reboots on a schedule -- so day 25 arrives.
//
// Disarming on expiry bounds the deadline's age and makes the compare correct
// again. It is the same reason demoTick() arms nextAt_ rather than counting
// from zero.
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

// --- how the load is spread across the corners ------------------------------
//
// Three cells hold a plane, so an evenly loaded platform puts a third on each.
// The operator cannot see that from three kilogram figures -- comparing 4.031,
// 3.887 and 4.102 in your head, live, while serving, is not a thing anybody
// does -- but they can see 33 / 32 / 34 at a glance, and they can see one of
// them turn red.
//
// WHAT IT IS FOR: a bowl set down off-centre loads one corner and unloads
// another. The total is still right -- that is the whole point of three cells
// -- but a badly off-centre load is closer to the end of a cell's range, and
// it is the thing an operator can actually fix by nudging the bowl.
const float SHARE_EVEN_PCT = 100.0f / CELLS;  // 33.3 for three

// PERCENTAGE POINTS FROM EVEN, not a percentage OF even. A corner is flagged at
// 23.3% or 43.4%, not at 30% or 36.7% -- the tighter reading would sit red
// almost permanently on a real platform and teach the operator to ignore it,
// which is the one failure mode a warning colour cannot survive. Raise or lower
// this single number if the field disagrees.
const float SHARE_TOL_PCT = 10.0f;

// BELOW THIS THERE IS NOTHING TO BE OFF-CENTRE. An empty platform's corners sit
// at a few counts of noise either side of zero, and a ratio of noise to noise
// is a random number that would flash red at an empty scale all day.
//
// IN COUNTS, NOT GRAMS, because noise is fixed in counts -- it is the
// converter's, not the load's -- so a counts floor holds on a unit whose
// calibration factor is nothing like this one's. 20000 counts is about 200 g
// here, which is well under an empty serving bowl: the shares appear as soon as
// the bowl lands, which is when the operator wants to centre it, and not before.
const int32_t SHARE_FLOOR_COUNTS = 20000;

// Whether the shares can be believed at all this frame.
//
// EVERY CELL ONLINE, NOT MOST. A share is a fraction of the total, and with a
// corner missing the "total" is a sum of the corners that answered -- so two
// live cells carrying 2 kg each would read 50 / 50 and look perfectly balanced
// on a platform that is missing a third of its measurement.
//
// AND NONE SATURATED. An over-range cell has stopped measuring and reports its
// own ceiling, which is a large steady plausible number -- so the corner
// carrying the MOST load would show the SMALLEST share. That is not a degraded
// reading, it is a confident inversion of the one fact this row exists to give.
bool sharesKnowable(const ScaleView &s) {
  if (s.online < CELLS) return false;
  for (uint8_t i = 0; i < CELLS; i++) {
    if (s.cell[i].state != Cell::Online) return false;
    if (s.cell[i].overRange) return false;
  }
  const int32_t t = s.totalCounts;
  return (t >= SHARE_FLOOR_COUNTS) || (t <= -SHARE_FLOOR_COUNTS);
}

// --- change detection -------------------------------------------------------
// Held as the FORMATTED strings rather than the numbers behind them, which is
// the stricter test and the one that matches what the panel actually has to
// repaint. Two totals 0.4 g apart are a different float and the same pixels;
// comparing floats would invalidate the biggest label on the screen for a
// change nobody can see, ten times a second.
char prevTotal_[16] = {0};
char prevCaption_[32] = {0};  // wider since it carries the unit too
char prevFlag_[16] = {0};
uint32_t prevTotalColor_ = 0;
bool haveTotalColor_ = false;
bool prevCalibrated_ = false;
uint8_t prevDecimals_ = 0;  // 0 is not a legal setting, so the first frame differs
bool haveCal_ = false;

void setIfChanged(lv_obj_t *label, char *prev, uint32_t prevLen, const char *text) {
  if (strncmp(prev, text, prevLen - 1) == 0) return;
  snprintf(prev, prevLen, "%s", text);
  lv_label_set_text(label, text);
}

// WHICH FACE THE TOTAL IS DRAWN IN, decided by how wide the widest reading in
// the current mode can get. The box is 224 px; every one of these was measured
// against that rather than guessed:
//
//   mode                 worst reading   at 84    at 56    face
//   uncalibrated         "-1234567"      --       --       montserrat_28
//   calibrated, 1 dp     "-19.9"         186 ok   124      font_mass_84
//   calibrated, 2 dp     "-19.99"        237 NO   159      font_mass_56
//   calibrated, 3 dp     "-19.999"       290 NO   193      font_mass_56
//
// Only one place fits the big face, and one place is the setting a unit ships
// on and very nearly the only one anybody uses -- 0.1 kg is already finer than
// a serving. The other two are not degraded; they are the size that has always
// been there, still the largest that fits what they have to print.
//
// The uncalibrated side stays on the BUILT-IN Montserrat 28: counts run to
// seven digits and a sign, which no generated size fits, and spending a third
// generated face on a mode that lasts until somebody calibrates the unit is
// not worth the flash. font_mass_* has digits and nothing else, which is all
// counts need too.
const lv_font_t *pickTotalFont(bool calibrated, uint8_t decimals) {
  if (!calibrated) return &lv_font_montserrat_28;
  return (decimals <= 1) ? &font_mass_84 : &font_mass_56;
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
  weightScr_ = scr;
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

  // THE OFFSET IN FORCE, at the left end of the caption's row. A figure that is
  // being subtracted from the big number below has to be visible beside it --
  // otherwise a station reading 8.0 kg is indistinguishable from one reading
  // 10.5 kg with a 2.5 kg vessel taken off, and the operator has no way to tell
  // which they are looking at.
  //
  // IGNORE_LAYOUT and positioned rather than added to the flex column, because
  // the caption is a full-width centred label and this has to sit beside it
  // without moving it. (0,0) is the top-left of the page's CONTENT box, which
  // is exactly where the caption's own row starts.
  //
  // 14 px and muted: it is a standing condition, not a reading. Blank when the
  // offset is off, so the ordinary case carries no extra ink at all.
  lblOffset_ = lv_label_create(scr);
  lv_obj_add_flag(lblOffset_, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_set_style_text_font(lblOffset_, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblOffset_, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_obj_set_pos(lblOffset_, 0, 1);
  lv_obj_set_width(lblOffset_, 74);
  lv_obj_set_style_text_align(lblOffset_, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  lv_label_set_long_mode(lblOffset_, LV_LABEL_LONG_CLIP);
  lv_label_set_text(lblOffset_, "");

  // The number ALONE, and it used to share this row with a "kg" on the same
  // baseline -- which read better, and cost 30 of the 224 px the page has.
  // At 84 px that 30 px is the difference between showing "100.0" and clipping
  // it, so the unit moved up into the caption, where a chart puts it. The knob
  // page makes the same trade with its per-cent sign for the same reason.
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
  // 224 IS THE WHOLE CONTENT WIDTH -- 240 less the page's 8 px of padding
  // either side. It is the widest this box can be, which is the point: the
  // face that fits inside it is chosen from that number, not the other way
  // round. See pickTotalFont().
  //
  // CENTRED, WHERE IT USED TO BE RIGHT-ALIGNED. Right alignment was there to
  // seat the digits against the unit standing beside them; with the unit in
  // the caption there is nothing to seat them against, and a number hugging
  // the right margin under a centred caption just looks misplaced. The reason
  // for the FIXED width is untouched by that -- it is the relayout above, not
  // the alignment.
  lblTotal = lv_label_create(row);
  lv_obj_set_style_text_font(lblTotal, &font_mass_84, LV_PART_MAIN);
  lv_obj_set_width(lblTotal, 224);
  lv_obj_set_style_text_align(lblTotal, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  // CLIP rather than WRAP: wrapping puts a second line underneath, which
  // changes the label's HEIGHT and marks the layout dirty exactly the way the
  // width change this avoids.
  lv_label_set_long_mode(lblTotal, LV_LABEL_LONG_CLIP);
  lv_label_set_text(lblTotal, "--");

  // --- the 200 kg buffer line ---------------------------------------------
  // DIRECTLY UNDER THE COUNTER'S TOTAL AND ABOVE THE A/B/C ROWS, where it was
  // asked for, and independent of the Cells setting -- it is the stock figure, not
  // a setup aid. Hidden until a module is reported fitted, so a unit without one
  // draws exactly what it always drew.
  //
  // Built like the cell rows beside it: a fixed-width name and a fixed-width,
  // right-aligned value, never a content-sized label (see the total above for what
  // that costs on a page redrawn ten times a second). 80 + 4 + 140 = 224, the
  // page's whole content width. 24 px high, the 20 px figure's line box.
  bufRow_ = lv_obj_create(scr);
  styleFlat(bufRow_);
  lv_obj_set_width(bufRow_, LV_PCT(100));
  lv_obj_set_height(bufRow_, 24);
  lv_obj_set_flex_flow(bufRow_, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(bufRow_, 4, LV_PART_MAIN);
  lv_obj_set_flex_align(bufRow_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(bufRow_, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *bufName = lv_label_create(bufRow_);
  lv_obj_set_style_text_font(bufName, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(bufName, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_obj_set_width(bufName, 80);
  lv_label_set_long_mode(bufName, LV_LABEL_LONG_CLIP);
  lv_label_set_text(bufName, "Buffer");

  // 140 holds the widest thing this can print: "-1234567 cts" is 121 px at this
  // size, and "-123.4 kg" is well under it.
  lblBufVal_ = lv_label_create(bufRow_);
  lv_obj_set_style_text_font(lblBufVal_, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_width(lblBufVal_, 140);
  lv_obj_set_style_text_align(lblBufVal_, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
  lv_label_set_long_mode(lblBufVal_, LV_LABEL_LONG_CLIP);
  lv_label_set_text(lblBufVal_, "--");

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

    // 138, DOWN FROM 194, and the 52 went to the share column beside it.
    // Measured in the simulator rather than apportioned by eye: at Montserrat
    // 20 the widest thing this column can ever hold is "-1234567 cts" at 121 px
    // -- the uncalibrated worst case, wider than "-19.999 kg !" at 108 -- so
    // 138 keeps 17 px in hand. The label is LONG_CLIP, so getting this wrong
    // does not wrap, it silently eats a digit off a weight.
    lblCellVal[i] = lv_label_create(r);
    lv_obj_set_style_text_font(lblCellVal[i], &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_width(lblCellVal[i], 138);
    lv_obj_set_style_text_align(lblCellVal[i], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_long_mode(lblCellVal[i], LV_LABEL_LONG_CLIP);
    lv_label_set_text(lblCellVal[i], "--");

    // THE SHARE. 56 px: "100%" measures 50 at this size, and the extra 6 covers
    // "-99%" and "199%", which are the ends of the range the formatter allows.
    // 22 + 4 + 138 + 4 + 56 = 224, the page's whole content width, exactly.
    //
    // Muted until it is wrong. This is a check somebody makes occasionally, not
    // the number they walked over to read, and three coloured figures competing
    // with the total would undo the point of making the total big.
    lblCellPct[i] = lv_label_create(r);
    lv_obj_set_style_text_font(lblCellPct[i], &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(lblCellPct[i], lv_color_hex(C_MUTED), LV_PART_MAIN);
    lv_obj_set_width(lblCellPct[i], 56);
    lv_obj_set_style_text_align(lblCellPct[i], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_long_mode(lblCellPct[i], LV_LABEL_LONG_CLIP);
    lv_label_set_text(lblCellPct[i], "");
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

  // THE SPACER SITS ABOVE THE PER-CELL ROWS, not below the flag chip where it was
  // built. With it here the slack opens up between the 200 kg buffer line and the
  // A/B/C rows, so the stock figure sits directly under the counter's total and the
  // A/B/C rows plus the warning chip ride down against the knob row -- which is the
  // arrangement asked for, and the buffer line is the one that keeps its room.
  // Moved by index rather than by building it earlier, so every other child stays
  // exactly where it was declared. The page's total height is unchanged: the spacer
  // is the same size, it has only changed seats.
  lv_obj_move_to_index(gap, lv_obj_get_index(cellBox));

  // --- the knob row -------------------------------------------------------
  // Above the buttons, because it is a readout and they are controls, and a
  // readout that sits below the thing you press gets covered by the hand that
  // presses it.
  lv_obj_t *encRow = lv_obj_create(scr);
  styleFlat(encRow);
  lv_obj_set_width(encRow, LV_PCT(100));
  lv_obj_set_height(encRow, 22);  // TRIAL: exactly the 20 px figure's line box
  lv_obj_set_style_pad_bottom(encRow, 4, LV_PART_MAIN);
  lv_obj_set_flex_flow(encRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(encRow, 8, LV_PART_MAIN);
  lv_obj_set_flex_align(encRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  // THE DOT IS ALWAYS IN THE LAYOUT AND ONLY ITS OPACITY CHANGES. Hiding it
  // with LV_OBJ_FLAG_HIDDEN would take it out of the flex row, and the position
  // figure beside it would jump eight pixels left every time the dot expired --
  // a twitch, twice a second, on the one screen that is watched continuously.
  // BLUE, NOT RED, and the palette comment forty lines up is the argument:
  // "Blue says occupied without editorialising, which leaves green and red free
  // to mean healthy and faulty on the same screen." C_CELL_FAULT is what a DEAD
  // CELL is painted on this very page. A knob press is the most ordinary event
  // the device has, and colouring it with the fault colour twice a second is
  // how staff learn to stop reading red -- so that when the total really does
  // go over-range, the one thing on screen that needed noticing is the colour
  // they have been trained to ignore.
  encDot = lv_obj_create(encRow);
  styleFlat(encDot);
  lv_obj_set_size(encDot, 12, 12);
  lv_obj_set_style_radius(encDot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(encDot, lv_color_hex(C_PRESENT), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(encDot, LV_OPA_TRANSP, LV_PART_MAIN);

  // "enc", NOT "pot". This is a screen about food in kilograms, and on it "pot"
  // is a cooking vessel -- `pot 3` under a weight reads as three pots, or as a
  // pot id, by anyone who has not been told otherwise. The rest of the codebase
  // already calls this an encoder (PIN_ENC_CLK, State::encoderPos), so the
  // panel may as well use the same word as the header somebody will check it
  // against.
  // TRIAL: 20 px against the raw count's 14. The type scale reserves 18 as the
  // floor for anything glanceable, and this has to be read by somebody standing
  // at the counter deciding whether it still matches the vessel -- where the
  // detent count beside it is for whoever is debugging the knob.
  lblFill = lv_label_create(encRow);
  lv_obj_set_style_text_font(lblFill, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblFill, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_text(lblFill, "");

  // NO RAW DETENT COUNT. It was bring-up instrumentation and it earned its
  // place while the encoder was unproven; now that the knob works it is a
  // number with no reader, on the one screen that should carry only figures
  // somebody acts on. The console still prints it -- `i` on the terminal --
  // which is where instrumentation belongs.

  // --- the action row -----------------------------------------------------
  // TWO WAYS OFF THIS PAGE, and nothing that changes a measurement. Everything
  // in this row is navigation now; the destructive control that used to sit
  // here is on Settings > Scale > Tare. That is the whole point of the row as
  // it stands -- a mis-tap costs you a page turn, not a zero.
  lv_obj_t *actions = lv_obj_create(scr);
  styleFlat(actions);
  lv_obj_set_width(actions, LV_PCT(100));
  // 56, DOWN FROM 64, and the 8 px went to the total above. Measured, not
  // trimmed on taste: with the cell rows shown AND a flag chip up, the page's
  // children came to 283 px inside a 278 px content box once the number went
  // to 84 px -- and this tile does not scroll, so the overflow is not a
  // scrollbar, it is the bottom of these buttons gone. 56 is still well past
  // any touch-target minimum; 64 was generous rather than necessary.
  lv_obj_set_height(actions, 56);
  lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(actions, 8, LV_PART_MAIN);
  // END, NOT CENTER, AND THAT CHANGED WITH THE TARE BUTTON. Main-axis CENTER
  // was a no-op while TARE was here: it had flex_grow 1, ate every pixel of
  // slack, and pinned the gear against the right margin. Delete it and 96 px
  // of slack appears -- measured, in the simulator -- so CENTER would have
  // floated the surviving pair into the middle of the row and moved the gear
  // 52 px left of where a thumb has learnt to find it. END puts both buttons
  // back on the exact pixels they occupied before: swap at 104, gear at 168.
  lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  // A BUTTON RATHER THAN A SWIPE, and the swipe still works alongside it.
  //
  // Dragging across this panel has to be deliberate to register: the touch
  // controller is polled at roughly 30 Hz through a driver that retries on
  // every read, so a quick flick does not produce enough samples to be read as
  // a drag at all. That is fine for a gesture somebody discovers once and
  // tolerable for one they use occasionally; it is the wrong primary way to
  // reach settings from a screen used one-handed.
  // TRIAL HARNESS: the page swap, to the left of the gear.
  //
  // 56 SQUARE, NOT 56 x 64, AND THAT IS A FIX. Both buttons were declared 64
  // tall inside a row that had just been shortened to 56 to make room for the
  // bigger total -- so LVGL centred a 64 px track in a 56 px box and placed
  // them at y = -4. Measured in the simulator, not inferred: the row reported
  // h=56 and every child y=-4 h=64. Four pixels of each rounded corner were
  // clipped top and bottom, and worse, hit-testing does NOT clip, so the touch
  // target ran 4 px past the row at both ends. On the page whose complaint was
  // accidental presses that is the wrong direction to be wrong in.
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

  // --- the knob row ---------------------------------------------------------
  if (lblFill != nullptr) {
    // TRIAL: the manual estimate, and it says when it is STALE rather than
    // going quiet. An estimate nobody has refreshed for a quarter of an hour is
    // the failure mode a knob-based system actually has, so the screen names it
    // instead of presenting an old number as a current one.
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
    //
    // havePress_ suppresses the first frame after this page becomes visible,
    // and weightPageHidden() is what re-arms it. Both halves are needed. This
    // function only runs while the home tile is showing, but the switch is
    // being read the whole time -- so pressing the knob inside the menu and
    // swiping back would otherwise arrive as a moved counter and light the dot
    // for a press that finished a minute ago. During bring-up that is the
    // COMMON path, not an edge case: the knob is exactly what you press while
    // looking at the Scale page.
    if (!havePress_) {
      havePress_ = true;
      prevPressCount_ = st.encoderPressCount;
    } else if (st.encoderPressCount != prevPressCount_) {
      prevPressCount_ = st.encoderPressCount;
      encFlashUntil_ = lv_tick_get() + 500;
      encFlashArmed_ = true;
    }

    // Disarm on expiry, so the signed compare above is only ever applied to a
    // deadline of bounded age -- see the declaration for what an unbounded one
    // does on day 25 of uptime.
    if (encFlashArmed_ && (int32_t)(encFlashUntil_ - lv_tick_get()) <= 0) {
      encFlashArmed_ = false;
    }
    if (encFlashArmed_ != encDotLit_) {
      encDotLit_ = encFlashArmed_;
      lv_obj_set_style_bg_opa(encDot, encDotLit_ ? LV_OPA_COVER : LV_OPA_TRANSP,
                              LV_PART_MAIN);
    }
  }

  // TWO DISPLAY MODES, and the uncalibrated one is not a degraded version of
  // the other. Without a known mass there is no counts-to-grams factor, so
  // there is no weight to show -- only the converter's own output. Printing
  // counts and labelling them "cts" says exactly that; printing them as
  // kilograms with an assumed factor would be a confident wrong number.
  // KEYED ON THE PRECISION SETTING AS WELL AS THE MODE, because the size now
  // follows both. Still a transition test and not a per-frame assignment: a
  // font change alters the label's height, which dirties the flex layout and
  // moves every sibling -- doing that ten times a second is the cost this page
  // was built to avoid.
  const bool calChanged = !haveCal_ || prevCalibrated_ != s.calibrated
                          || prevDecimals_ != s.decimals;
  if (calChanged) {
    haveCal_ = true;
    prevCalibrated_ = s.calibrated;
    prevDecimals_ = s.decimals;
    // Counts run to seven digits and a sign; kilograms to six characters. They
    // cannot share a size, so the size follows the mode -- set here, on the
    // transition, rather than per frame.
    //
    lv_obj_set_style_text_font(lblTotal, pickTotalFont(s.calibrated, s.decimals),
                               LV_PART_MAIN);
  }

  if (s.online == 0) {
    // Nothing is converting, so there is no total. Dashes rather than a zero:
    // on a scale, zero is a measurement somebody might act on.
    setIfChanged(lblTotal, prevTotal_, sizeof(prevTotal_), "--");
    setTotalColor(C_MUTED);
    setIfChanged(lblCaption, prevCaption_, sizeof(prevCaption_), "total");
  } else {
    // NET OF THE EMPTY VESSEL, when that is switched on. Subtracted here and
    // in the uplink rather than in scale.cpp, because the same snapshot feeds
    // the per-cell share arithmetic -- whose denominator must match the
    // per-cell grams beside it -- and the tare, which has to zero the REAL
    // platform or the offset lands twice.
    if (s.calibrated)
      formatKg(buf, sizeof(buf), s.totalGrams - s.vesselOffsetG, s.decimals);
    else snprintf(buf, sizeof(buf), "%ld", (long)s.totalCounts);
    setIfChanged(lblTotal, prevTotal_, sizeof(prevTotal_), buf);
    setTotalColor(s.overRange ? C_CELL_FAULT : C_TEXT);

    // "AT LEAST", NOT "TOTAL", WHEN A CELL IS MISSING. The sum of the cells
    // that ARE working is a genuine lower bound on the load, because the
    // missing corner cannot carry a negative share of it. What it is not is the
    // total, and a caption saying so would turn a useful partial measurement
    // into a wrong complete one.
    // THE UNIT RIDES HERE NOW, after the word, the way a chart labels an axis.
    // It is the same fact it was when it stood beside the number; what it no
    // longer is, is 30 px of the number's line.
    const bool partial = s.online < CELLS;
    const char *cap =
        s.calibrated ? (partial ? "at least, kg" : "total, kg")
                     : (partial ? "at least, uncalibrated cts"
                                : "total, uncalibrated cts");
    setIfChanged(lblCaption, prevCaption_, sizeof(prevCaption_), cap);

    // Only when it is actually being applied -- and only when calibrated, since
    // an uncalibrated station is showing counts and a kilogram offset means
    // nothing against them.
    char off[12];
    if (s.calibrated && s.vesselOffsetG > 0.0f)
      snprintf(off, sizeof(off), "-%.1f kg", s.vesselOffsetG / 1000.0f);
    else
      off[0] = ' ';
    setIfChanged(lblOffset_, prevOffset_, sizeof(prevOffset_), off);
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
    } else if (s.online == 0) {
      // NAMES THE CABLE, because that is the fix nine times in ten. The cells
      // live in a separate housing on a USB-C lead carrying 3V3 and the I2C
      // pair, so "0 cells" almost always means a connector, not a converter --
      // and "3 cells down" sends somebody to open the wrong enclosure.
      flag = "connect load-cell cable";
    } else {
      const uint8_t missing = (uint8_t)(CELLS - s.online);
      snprintf(down, sizeof(down), "%u cell%s down", missing, missing == 1 ? "" : "s");
      flag = down;
    }
    flagColor = C_FAULT;
  } else if (!s.tared && !s.platformZeroed) {
    // The automatic power-up tare normally clears this within a few seconds of
    // boot. Seeing it persist means the platform never held still long enough,
    // which is worth knowing before trusting the number above it.
    // ONLY WHEN THERE IS NO ZERO AT ALL. This fired on `!s.tared` alone, and
    // since the power-up auto-tare was compiled out that is every boot -- so a
    // correctly-reading station wore a red warning permanently, and the same
    // condition nulled weight_g upstream and emptied the dashboard.
    //
    // platformZero is a commissioned zero restored from NVS; a session tare is
    // a refinement on top of it. Warn when neither exists.
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
      // Decided ONCE for the row set, not per cell: the shares are knowable or
      // they are not, and three corners cannot disagree about it.
      const bool shares = sharesKnowable(s);
      // The same quantity the column beside it is printing, so the two can
      // never tell different stories. It makes no arithmetic difference -- one
      // calibration factor scales every cell, so the grams ratio and the counts
      // ratio are the same number -- but a reader checking the percentage
      // against the kilograms should be checking it against THOSE kilograms.
      const float denom = s.calibrated ? s.totalGrams : (float)s.totalCounts;

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

        // --- the share -----------------------------------------------------
        char pb[8] = {0};
        uint32_t pcol = C_MUTED;
        if (shares && denom != 0.0f) {
          const float mine = s.calibrated ? c.grams : (float)c.counts;
          const float pct = 100.0f * mine / denom;
          const bool off = (pct < SHARE_EVEN_PCT - SHARE_TOL_PCT) ||
                           (pct > SHARE_EVEN_PCT + SHARE_TOL_PCT);
          // OUTSIDE -99..199 THE FIGURE STOPS BEING INFORMATION. A share that
          // far out means a corner is being lifted while another is pressed --
          // real, and worth the red, but the digits are not something anybody
          // acts on and they would not fit the column anyway. The colour still
          // carries, because "this is badly wrong" is the whole message.
          if (pct < -99.0f || pct > 199.0f) snprintf(pb, sizeof(pb), "--");
          else snprintf(pb, sizeof(pb), "%d%%", (int)(pct < 0 ? pct - 0.5f : pct + 0.5f));
          if (off) pcol = C_CELL_FAULT;
        }
        setIfChanged(lblCellPct[i], prevPct_[i], sizeof(prevPct_[i]), pb);
        if (!havePctColor_[i] || prevPctColor_[i] != pcol) {
          havePctColor_[i] = true;
          prevPctColor_[i] = pcol;
          lv_obj_set_style_text_color(lblCellPct[i], lv_color_hex(pcol), LV_PART_MAIN);
        }
      }
    }
  }

  // --- the 200 kg buffer line ---------------------------------------------
  if (bufRow_) {
    const ScaleView::BufferView &b = s.buffer;

    // THE STOCK FIGURE IS NEVER HIDDEN TO MAKE ROOM. This used to hide the line
    // when the per-cell rows were shown AND a flag chip was up, to keep the buttons
    // on the panel -- and that is precisely the state of a unit with the counter's
    // lead unplugged and the Cells setting on, so on the bench the one figure that
    // was wanted vanished. A guard that fires in a common state is not a guard.
    //
    // The page's own note records that in that state its children come to within
    // ~3 px of the content box, and the tile does not scroll, so one more row would
    // push the bottom of the gear off the panel. The row adds 24 px plus one 4 px
    // gap; closing the page's seven 4 px row gaps gives back 28. So in exactly that
    // state the gaps close and nothing is lost -- cramped for as long as the chip
    // is up, but nothing hidden and no button clipped.
    const bool flagUp = !lv_obj_has_flag(lblFlag, LV_OBJ_FLAG_HIDDEN);
    const bool crowded = b.fitted && s.showCells && flagUp;
    if (crowded != prevCrowded_) {
      prevCrowded_ = crowded;
      if (weightScr_) lv_obj_set_style_pad_row(weightScr_, crowded ? 0 : 4, LV_PART_MAIN);
    }

    const bool want = b.fitted;
    const bool shown = !lv_obj_has_flag(bufRow_, LV_OBJ_FLAG_HIDDEN);
    if (want != shown) {
      if (want) lv_obj_remove_flag(bufRow_, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(bufRow_, LV_OBJ_FLAG_HIDDEN);
    }
    // ON A DECISION CHANGE ONLY, and at warn level because that is the lowest this
    // build prints: it is the one way to see from the console why a line is or is
    // not on the glass -- the panel itself cannot be read remotely.
    if (prevBufShown_ != (int8_t)want) {
      prevBufShown_ = (int8_t)want;
      LV_LOG_WARN("buffer row %s (fitted %d, cells shown %d, flag up %d, state %d)",
                  want ? "SHOWN" : "hidden", (int)b.fitted, (int)s.showCells, (int)flagUp,
                  (int)b.state);
    }

    if (want) {
      char bb[24];
      uint32_t col = C_TEXT;
      if (b.state == Cell::Offline) {
        // Said, not shown as a stale or zero figure: the refusal the total makes.
        snprintf(bb, sizeof(bb), "offline");
        col = C_CELL_FAULT;
      } else if (b.state == Cell::Warming) {
        snprintf(bb, sizeof(bb), "warming");
        col = C_MUTED;
      } else if (b.overRange) {
        snprintf(bb, sizeof(bb), "OVER");  // a saturated cell reports its ceiling, not a mass
        col = C_CELL_FAULT;
      } else if (b.kgKnown) {
        // ONE DECIMAL, FIXED -- not the counter's Precision setting. That setting
        // exists for a 20 kg cell whose third decimal is a gram; this is a 200 kg
        // cell whose own noise is several grams, so 000.0 kg is the honest
        // resolution and 0.000 kg would be digits that will not sit still.
        formatKg(bb, sizeof(bb), b.grams, 1);
        const size_t n = strlen(bb);
        snprintf(bb + n, sizeof(bb) - n, " kg");
      } else {
        // No zero or no factor yet: counts, exactly as the counter does until it is
        // calibrated -- there is no unit behind the number, so none is drawn.
        snprintf(bb, sizeof(bb), "%ld cts", (long)b.counts);
        col = C_MUTED;
      }
      setIfChanged(lblBufVal_, prevBuf_, sizeof(prevBuf_), bb);
      if (!haveBufColor_ || prevBufColor_ != col) {
        haveBufColor_ = true;
        prevBufColor_ = col;
        lv_obj_set_style_text_color(lblBufVal_, lv_color_hex(col), LV_PART_MAIN);
      }
    }
  }
}

}  // namespace ui
