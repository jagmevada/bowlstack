// Stock view -- the primary screen.
//
// LAID OUT WITH FLEX CONTAINERS, NOT COORDINATES. The first version positioned
// everything with hand-computed offsets against two different anchors, and the
// result on real hardware was exactly what that method produces: the battery
// line ran off the right edge, the touch line was sliced in half by the button,
// and the right-hand column sat mostly empty while the left one was cramped.
//
// Arithmetic like `48 + i * 50` cannot express "these must not overlap" -- it
// can only happen to satisfy it, and only for one font, one string length and
// one screen size. Flex states the intent instead, so a longer battery string
// or a bigger font reflows rather than collides.

#include "ui_screens.h"

#include <lvgl.h>
#include <stdio.h>

namespace ui {
namespace {

// Dark palette. Values are the same family the web UI uses, so a person moving
// between the station and the dashboard sees one product.
//
// C_BG is pure black, not the near-black 0x0D1117 it started as.
//
// The panel renders it as dark GREY and the monitor renders it as black, and
// that gap is not something code can close: an IPS LCD lights every pixel from
// behind, so its black is backlight leaking through a closed shutter. Both are
// being sent the identical value. Going to 0x000000 does not equalise them --
// nothing will -- but it does give the panel the deepest black it has, which is
// the most contrast the level cells and the count can get.
//
// It also lowers average luminance, which is the cheapest of the image-retention
// mitigations (see todo.md).
const uint32_t C_BG = 0x000000;
const uint32_t C_PANEL = 0x161B22;
const uint32_t C_BORDER = 0x30363D;
const uint32_t C_TEXT = 0xE6EDF3;
const uint32_t C_MUTED = 0x8B949E;
// Blue for a present bowl, not green. Green reads as "OK" -- a judgement --
// and a full stack is not better than an empty one, it is just a different
// amount of food. Blue says "occupied" without editorialising, which leaves
// green and red free to mean healthy and faulty on the same screen.
const uint32_t C_PRESENT = 0x1F6FEB;

// Light red fills the whole cell for a dead sensor, rather than the thin red
// border it had before. A 1 px border on a 240 px panel is not something anyone
// notices across a kitchen, and a sensor fault is the one thing on this screen
// that needs to be noticed from a distance.
const uint32_t C_CELL_FAULT = 0xE05C5C;
const uint32_t C_WARN = 0x9E6A03;
const uint32_t C_FAULT = 0xB62324;

lv_obj_t *lblDevice;
lv_obj_t *lblWifi;
lv_obj_t *lblCount;
lv_obj_t *lblCountCap;
lv_obj_t *lblStatus;
lv_obj_t *cells[LEVELS];
lv_obj_t *cellLabels[LEVELS];
lv_obj_t *lblBattery;
lv_obj_t *barBattery;

// Everything the UI draws lives inside this, so that image-retention shifting
// can move the whole layout with one translate. Applying the offset to the
// screen object itself would not work: the screen is the thing the shift is
// meant to move content ACROSS, and it is also what paints the background,
// which must stay put.
lv_obj_t *root;

void styleFlat(lv_obj_t *o) {
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(o, 0, LV_PART_MAIN);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

}  // namespace

void build(lv_obj_t *parent) {
  lv_obj_t *outer = parent ? parent : lv_screen_active();
  lv_obj_set_style_bg_color(outer, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(outer, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(outer, 0, LV_PART_MAIN);
  lv_obj_remove_flag(outer, LV_OBJ_FLAG_SCROLLABLE);

  // The shiftable root. Transparent, so the background stays with `outer` and
  // does not travel with the content -- otherwise the shift would drag a black
  // rectangle across a black screen and leave an uncovered edge.
  root = lv_obj_create(outer);
  lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(root, 0, LV_PART_MAIN);

  lv_obj_t *scr = root;
  lv_obj_set_style_text_color(scr, lv_color_hex(C_TEXT), LV_PART_MAIN);

  // An LVGL screen is scrollable by default, and this one has no business
  // being. It is a fixed readout: there is nothing below the fold to reach, so
  // a drag can only displace a correct layout into an incorrect one, and a
  // person prodding at a bowl count has no reason to expect it to move.
  //
  // (This was originally added chasing a flicker that turned out to be
  // capacitive coupling from a hand on the panel edge -- a hardware and
  // handling matter, not a rendering one. The change is kept because it is
  // right on its own terms, not because it fixed that.)
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  // Back to 8 now that the pixel shift is parked (todo.md); the extra 2 px of
  // margin existed only to absorb its excursion, and the status bar has since
  // taken height off this page.
  lv_obj_set_style_pad_all(scr, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(scr, 6, LV_PART_MAIN);

  // NO HEADER. The device id and the WiFi state used to sit here, and both are
  // now in the permanent status bar directly above -- so this row was showing
  // the same two facts twice, 22 px apart, and spending the height twice too.
  //
  // The labels themselves stay allocated but unparented to nothing: update()
  // writes to them, and deleting them would mean threading null checks through
  // every call site for no gain. They are simply never shown.
  lblDevice = lv_label_create(scr);
  lv_obj_add_flag(lblDevice, LV_OBJ_FLAG_HIDDEN);
  lblWifi = lv_label_create(scr);
  lv_obj_add_flag(lblWifi, LV_OBJ_FLAG_HIDDEN);

  // --- body: stack column | count panel -----------------------------------
  lv_obj_t *body = lv_obj_create(scr);
  styleFlat(body);
  lv_obj_set_width(body, LV_PCT(100));
  lv_obj_set_flex_grow(body, 1);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(body, 8, LV_PART_MAIN);

  lv_obj_t *column = lv_obj_create(body);
  styleFlat(column);
  lv_obj_set_height(column, LV_PCT(100));
  lv_obj_set_flex_grow(column, 3);
  lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(column, 5, LV_PART_MAIN);

  // Built top-down as f4..f1 so the array index still means what it says: the
  // widget for levels[0] is created last and sits at the bottom, matching the
  // pipe. Reversing the array instead would put f1 at index 3 everywhere else.
  for (int8_t i = LEVELS - 1; i >= 0; i--) {
    lv_obj_t *cell = lv_obj_create(column);
    lv_obj_set_width(cell, LV_PCT(100));
    lv_obj_set_flex_grow(cell, 1);
    lv_obj_set_style_radius(cell, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(cell, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(cell, lv_color_hex(C_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(cell, lv_color_hex(C_PANEL), LV_PART_MAIN);
    lv_obj_set_style_pad_all(cell, 4, LV_PART_MAIN);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    // The f1..f4 tags are gone. Their job was to say which level a cell is, and
    // the column's own order already says that -- bottom cell is the bottom
    // bowl, which is the physical arrangement and needs no caption once you
    // have looked at it twice. Removing them leaves the state word centred with
    // the whole cell width to itself.
    //
    // Worth noting what is lost: a photograph of this screen no longer labels
    // its own rows. If a level ever has to be named to someone not standing in
    // front of it, that belongs in the health page rather than back here.
    lv_obj_t *l = lv_label_create(cell);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(l, "--");
    lv_obj_center(l);

    cells[i] = cell;
    cellLabels[i] = l;
  }

  lv_obj_t *panel = lv_obj_create(body);
  styleFlat(panel);
  lv_obj_set_height(panel, LV_PCT(100));
  lv_obj_set_flex_grow(panel, 2);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lblCount = lv_label_create(panel);
  lv_obj_set_style_text_font(lblCount, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_label_set_text(lblCount, "-");

  lblCountCap = lv_label_create(panel);
  lv_obj_set_style_text_font(lblCountCap, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblCountCap, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(lblCountCap, "bowls");

  // Status chip. Hidden when everything is fine -- a permanent "OK" badge
  // teaches people to stop reading the area, which is the opposite of what a
  // fault indicator is for.
  lblStatus = lv_label_create(panel);
  lv_obj_set_style_text_font(lblStatus, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(lblStatus, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(lblStatus, lv_color_hex(C_WARN), LV_PART_MAIN);
  lv_obj_set_style_radius(lblStatus, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_all(lblStatus, 3, LV_PART_MAIN);
  lv_obj_set_style_margin_top(lblStatus, 6, LV_PART_MAIN);
  lv_label_set_text(lblStatus, "");
  lv_obj_add_flag(lblStatus, LV_OBJ_FLAG_HIDDEN);

  // NO FOOTER. The battery line and its bar are gone: the status bar shows the
  // band permanently, and tapping it opens a page with the voltage, the
  // percentage and the charge state. This row was a third rendering of the same
  // fact, and the least precise of the three.
  //
  // Hidden rather than deleted, like the header labels above -- update() still
  // writes to them, and null-guarding every call site buys nothing. Both sets
  // should go properly when the health page takes over their content.
  lblBattery = lv_label_create(scr);
  lv_obj_add_flag(lblBattery, LV_OBJ_FLAG_HIDDEN);
  barBattery = lv_obj_create(scr);
  lv_obj_add_flag(barBattery, LV_OBJ_FLAG_HIDDEN);
}

// EVERY WRITE BELOW IS GUARDED, and that is the difference between a UI that
// costs nothing and one that repaints the whole panel eight times a second.
//
// LVGL's setters do not compare before acting: lv_label_set_text() reallocates
// its buffer and lv_obj_set_style_bg_color() marks the object dirty, whether or
// not the value changed. This function runs every loop iteration, so writing
// unconditionally invalidated all four cells, the count and the status chip on
// every pass -- a full-screen redraw, ~125 ms of render and SPI, for a screen
// whose content changes every three seconds.
//
// The per-frame reallocation is also the likeliest cause of the desktop preview
// slowing and eventually hanging after a while: label buffers churned fifty
// times a second fragment the LVGL pool, and lv_mem_monitor's frag figure in
// the perf line is there to confirm or refute that.
namespace {
State prev_;
bool havePrev_ = false;

bool levelChanged(const State &s, uint8_t i) {
  return !havePrev_ || prev_.levels[i] != s.levels[i] ||
         prev_.sensorOnline[i] != s.sensorOnline[i];
}
}  // namespace

void update(const State &s) {
  const bool first = !havePrev_;

  if (first || prev_.deviceId != s.deviceId) {
    lv_label_set_text(lblDevice, s.deviceId ? s.deviceId : "BWL-000");
  }
  if (first || prev_.wifiConnected != s.wifiConnected) {
    lv_label_set_text(lblWifi, s.wifiConnected ? "wifi ok" : "wifi down");
    lv_obj_set_style_text_color(lblWifi,
                                lv_color_hex(s.wifiConnected ? C_MUTED : C_FAULT),
                                LV_PART_MAIN);
  }

  static const char *levelText[] = {"unknown", "absent", "present"};
  for (uint8_t i = 0; i < LEVELS; i++) {
    if (!levelChanged(s, i)) continue;
    const Level lv = s.levels[i];
    // Three states, three fills: occupied, empty, faulty. Colour carries this
    // now that the level tags are gone, so it has to be unambiguous at a glance
    // -- the word beside it is the confirmation, not the signal.
    uint32_t bg = C_PANEL;
    if (!s.sensorOnline[i]) bg = C_CELL_FAULT;
    else if (lv == Level::Present) bg = C_PRESENT;
    lv_obj_set_style_bg_color(cells[i], lv_color_hex(bg), LV_PART_MAIN);

    // A dead sensor is called out on the cell it belongs to, rather than only
    // in an aggregate count, because "which one" is the actionable part. The
    // level tag stays put; only the state word changes.
    if (!s.sensorOnline[i]) {
      lv_obj_set_style_border_color(cells[i], lv_color_hex(C_CELL_FAULT), LV_PART_MAIN);
      lv_label_set_text(cellLabels[i], "offline");
    } else {
      lv_obj_set_style_border_color(cells[i], lv_color_hex(C_BORDER), LV_PART_MAIN);
      lv_label_set_text(cellLabels[i], levelText[(uint8_t)lv]);
    }
  }

  // FRONTEND_HANDOFF.md is explicit and the local UI must not contradict it:
  // discontiguous means a bowl was seen ABOVE an empty level, which is
  // physically impossible, so there is no trustworthy count to show. Rendering
  // "2 bowls" there would be worse than rendering an error.
  if (first || prev_.stack != s.stack || prev_.stackCount != s.stackCount) {
  if (s.stack == Stack::Discontiguous) {
    lv_label_set_text(lblCount, "!");
    lv_obj_set_style_text_color(lblCount, lv_color_hex(C_FAULT), LV_PART_MAIN);
    lv_label_set_text(lblCountCap, "fault");
    lv_obj_set_style_bg_color(lblStatus, lv_color_hex(C_FAULT), LV_PART_MAIN);
    lv_label_set_text(lblStatus, "impossible stack");
    lv_obj_remove_flag(lblStatus, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_label_set_text_fmt(lblCount, "%u", s.stackCount);
    lv_obj_set_style_text_color(lblCount, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_label_set_text(lblCountCap, "bowls");
    if (s.stack == Stack::Degraded) {
      // Shown WITH the number, not instead of it: a degraded count is a lower
      // bound, which is still useful, unlike a discontiguous one.
      lv_obj_set_style_bg_color(lblStatus, lv_color_hex(C_WARN), LV_PART_MAIN);
      lv_label_set_text(lblStatus, "at least");
      lv_obj_remove_flag(lblStatus, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(lblStatus, LV_OBJ_FLAG_HIDDEN);
    }
  }
  }

  static const char *battText[] = {"no cell", "critical", "low", "medium", "good"};
  const uint32_t battColor[] = {C_MUTED, C_FAULT, C_FAULT, C_WARN, C_PRESENT};
  const uint8_t bi = (uint8_t)s.battery;

  // "no cell" carries no millivolts on purpose. Unknown means the measurement
  // is not trustworthy, and printing a number beside that word would invite
  // exactly the confidence the word exists to withhold.
  if (first || prev_.battery != s.battery || prev_.batteryMv != s.batteryMv ||
      prev_.charging != s.charging || prev_.chargingKnown != s.chargingKnown) {
    if (s.battery == Battery::Unknown) {
      lv_label_set_text(lblBattery, "battery  no cell");
    } else if (s.chargingKnown && s.charging) {
      lv_label_set_text_fmt(lblBattery, "chg  %u mV  %s", s.batteryMv, battText[bi]);
    } else {
      lv_label_set_text_fmt(lblBattery, "batt  %u mV  %s", s.batteryMv, battText[bi]);
    }
    lv_obj_set_style_bg_color(barBattery, lv_color_hex(battColor[bi]), LV_PART_MAIN);
  }

  prev_ = s;
  havePrev_ = true;
}

// --- image-retention pixel shift -------------------------------------------
// Nudges the whole layout around a small ring so no pixel holds the same
// high-contrast value indefinitely. This is what televisions and station clocks
// do, and this device has the same problem for the same reason: README.md has
// units powered ~8 h/day showing a near-static digit.
//
// INTEGER offsets only. A sub-pixel shift would resample the glyphs and soften
// every edge -- trading a retention problem for a legibility one on a panel
// that has already shown it has no legibility to spare. Whole pixels move the
// image without touching a single rendered value.
//
// A RING rather than a random walk: every position is visited equally often, so
// the time-averaged luminance of each pixel converges, which is the whole
// mechanism. A random walk can dwell.
namespace {
const int8_t SHIFT_RING_X[] = {0, 1, 2, 2, 2, 1, 0, 0};
const int8_t SHIFT_RING_Y[] = {0, 0, 0, 1, 2, 2, 2, 1};
const uint8_t SHIFT_STEPS = 8;

uint8_t shiftIdx_ = 0;
uint32_t shiftNextAt_ = 0;
bool shiftArmed_ = false;
}  // namespace

void applyPixelShift(int8_t dx, int8_t dy) {
  if (!root) return;
  // translate_x/y move the object at DRAW time without re-running the flex
  // layout, so a shift costs a redraw and not a relayout of every child.
  lv_obj_set_style_translate_x(root, dx, LV_PART_MAIN);
  lv_obj_set_style_translate_y(root, dy, LV_PART_MAIN);
}

bool pixelShiftTick(uint32_t nowMs, uint32_t periodMs) {
  if (!shiftArmed_) {
    shiftArmed_ = true;
    shiftNextAt_ = nowMs;
  }
  if ((int32_t)(nowMs - shiftNextAt_) < 0) return false;
  shiftNextAt_ = nowMs + periodMs;

  applyPixelShift(SHIFT_RING_X[shiftIdx_], SHIFT_RING_Y[shiftIdx_]);
  shiftIdx_ = (uint8_t)((shiftIdx_ + 1) % SHIFT_STEPS);
  return true;
}

uint8_t pixelShiftIndex() { return shiftIdx_; }
void pixelShiftOffset(int8_t *dx, int8_t *dy) {
  const uint8_t i = (uint8_t)((shiftIdx_ + SHIFT_STEPS - 1) % SHIFT_STEPS);
  *dx = SHIFT_RING_X[i];
  *dy = SHIFT_RING_Y[i];
}

State unknownState() {
  State s{};
  for (uint8_t i = 0; i < LEVELS; i++) {
    s.levels[i] = Level::Unknown;
    s.sensorOnline[i] = false;
  }
  s.stackCount = 0;
  s.stack = Stack::Degraded;
  s.sensorsOnline = 0;
  s.battery = Battery::Unknown;
  s.batteryMv = 0;
  s.batteryPinMv = 0;
  // -1 rather than the 0 that zero-initialisation would leave. A freshly
  // constructed state knows nothing about the cell, and 0 would say it is flat.
  s.batteryPercent = -1;
  s.chargingKnown = false;
  s.charging = false;
  s.wifiConnected = false;
  s.uptimeSec = 0;
  s.deviceId = "BWL-000";
  s.firmware = "0.0.0";
  return s;
}

}  // namespace ui
