#include "ui_pages.h"

#include <lvgl.h>
#include <stdio.h>

#include "ui_battery.h"
#include "ui_demo.h"
#include "ui_calib.h"
#include "ui_device.h"
#include "ui_menu.h"
#include "ui_perf.h"
#include "ui_scope.h"
#include "ui_screens.h"
#include "ui_status.h"
#include "ui_weight.h"
#include "ui_wifi.h"

namespace ui {
namespace {

const uint32_t C_BG = 0x000000;
const uint32_t C_MUTED = 0x8B949E;

lv_obj_t *tv_ = nullptr;
lv_obj_t *tileMenu_ = nullptr;
lv_obj_t *tileHome_ = nullptr;
lv_obj_t *lastActive_ = nullptr;
lv_obj_t *menuRoot_ = nullptr;

// --- the navigation tree ---------------------------------------------------
//
//   [menu] <-swipe-> [weight]         two pages; weight is home
//     |
//     +-- Settings --+-- WiFi
//     |              +-- Battery
//     |              +-- Scale --+-- Calibrate  (keypad, arbitrary mass)
//     |                          (tare, clear, average)
//     +-- Sensors  (the live cell scope)
//     +-- Device   (raw counts, tare, rate, settings)
//
// Sub-pages are OVERLAYS rather than more tiles, because a tileview is a flat
// sequence and this is a tree: Settings > WiFi has to go BACK to Settings, not
// two swipes leftward past it. It also keeps the swipe gesture meaning exactly
// one thing -- move between the two top-level pages -- rather than sometimes
// meaning "leave a sub-page".
//
// The status-bar icons are no longer buttons. Hanging pages off them worked for
// exactly two and had nowhere to put a third; navigation now has one entrance,
// and every future setting is a ROW rather than a screen with its own gesture.
lv_obj_t *detailSettings_ = nullptr;
lv_obj_t *detailWifi_ = nullptr;
lv_obj_t *detailBatt_ = nullptr;
lv_obj_t *detailScale_ = nullptr;
lv_obj_t *detailCalib_ = nullptr;
lv_obj_t *detailSensor_ = nullptr;
lv_obj_t *detailDevice_ = nullptr;

// Depth 3 covers menu > Settings > WiFi, the deepest the tree goes; 4 leaves a
// slot spare. A stack rather than a single "previous" pointer, so Back stays
// unambiguous once a page can be reached from more than one place -- which
// Battery already could be, the moment anything else links to it.
lv_obj_t *stack_[4] = {nullptr, nullptr, nullptr, nullptr};
uint8_t depth_ = 0;

// Installed by the firmware; absent in the desktop preview, where there are no
// cells to tare. A null handler leaves the row inert rather than pretending.
void (*onTare_)(void) = nullptr;
void (*onClearCal_)(void) = nullptr;
void (*onCycleAvg_)(void) = nullptr;
void (*onCyclePrecision_)(void) = nullptr;
void (*onRestore_)(void) = nullptr;
void (*onPlatformZero_)(void) = nullptr;
void (*onToggleCells_)(void) = nullptr;

// The last mass the snapshot carried, cached so showOnly() can re-prefill the
// keypad without a State to hand. pagesTick() keeps it current.
float lastCalMassG_ = 0.0f;

lv_obj_t *settingsMenu_ = nullptr;
lv_obj_t *scaleMenu_ = nullptr;

// ROW POSITIONS, NAMED. menuSetHint() addresses rows by index, and inserting
// "Set platform zero" at position 1 silently moved the calibration mass onto it
// and the averaging figure onto "Clear calibration" -- two hints attached to
// two labels that had nothing to do with them, with nothing to fail. Naming the
// positions next to the rows themselves does not make that impossible, but it
// puts the two things a reader has to keep in step within a screen of each
// other.
enum : uint8_t {
  ROW_SET_WIFI = 0,
  ROW_SET_BATTERY,
  ROW_SET_SCALE,
  ROW_SET_DIAGNOSE,
  ROW_SET_PRECISION,
};
enum : uint8_t {
  ROW_SCALE_TARE = 0,
  ROW_SCALE_PLATFORM_ZERO,
  ROW_SCALE_CALIBRATE,
  ROW_SCALE_CLEAR,
  ROW_SCALE_AVERAGE,
  ROW_SCALE_CELLS,
  ROW_SCALE_RESTORE,
};

void doTare() { if (onTare_) onTare_(); }
void doClearCal() { if (onClearCal_) onClearCal_(); }
void doCycleAvg() { if (onCycleAvg_) onCycleAvg_(); }
void doCyclePrecision() { if (onCyclePrecision_) onCyclePrecision_(); }
void doRestore() { if (onRestore_) onRestore_(); }
void doPlatformZero() { if (onPlatformZero_) onPlatformZero_(); }
void doToggleCells() { if (onToggleCells_) onToggleCells_(); }

// --- the two navigation buttons -------------------------------------------
// Forward declared because both live on pages built further down, and both do
// the same thing the swipe does. The swipe is untouched; these exist because a
// drag on this panel has to be slow and deliberate to register at all, which is
// the wrong thing to require of somebody holding a bowl.
void goToMenu() {
  if (tv_ && tileMenu_) lv_tileview_set_tile(tv_, tileMenu_, LV_ANIM_OFF);
}
void goToHome() {
  if (tv_ && tileHome_) lv_tileview_set_tile(tv_, tileHome_, LV_ANIM_OFF);
}

void showOnly(lv_obj_t *which) {
  lv_obj_t *all[] = {detailSettings_, detailWifi_,   detailBatt_,   detailScale_,
                     detailCalib_,    detailSensor_, detailDevice_};
  for (uint8_t i = 0; i < 7; i++) {
    if (!all[i]) continue;
    if (all[i] == which) lv_obj_remove_flag(all[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(all[i], LV_OBJ_FLAG_HIDDEN);
  }
  // The scope repaints its whole trace from the ring on entry, so telling it on
  // TRANSITION rather than polling is what keeps that a once-per-visit cost.
  scopeSetVisible(which == detailSensor_);

  // THE KEYPAD IS RE-PREFILLED ON EVERY ENTRY, and it belongs here rather than
  // in openCalib() precisely because this is the one function every route goes
  // through: opening it, coming back to it, closeAll(), and the 60 s idle
  // timeout that fires pagesGoHome() while somebody is mid-typing.
  //
  // Without it the entry is boot-scoped. Type half a mass, get called away, let
  // the idle timer take the screen home -- and the next person to open the page
  // finds "17550" sitting there looking like a deliberate value.
  if (which == detailCalib_) calibSetMass(lastCalMassG_);
}

void closeAll() {
  wifiResetView();
  depth_ = 0;
  showOnly(nullptr);
}

// --- lazy page construction ------------------------------------------------
// THE DETAIL PAGES ARE BUILT ON FIRST OPEN, NOT AT BOOT, and the numbers are
// why. Measured on the panel, buildPages() took 3.27 s, of which:
//
//     menu + home + weight   ~1000 ms   <- the first screen, must be eager
//     WiFi                    1343 ms   <- a full keyboard, ~40 widgets
//     Calibrate                343 ms   <- a keypad
//     Battery                  230 ms
//     Diagnose                  86 ms
//     Scope                    134 ms
//
// That is 2.1 s of a boot spent constructing screens nobody has asked for, on a
// device whose splash is what somebody is watching while it happens. The WiFi
// page is the extreme case: over a second to build a passphrase keyboard that
// most units will never show, because the network is configured once.
//
// The CONTAINER is still created eagerly -- makeDetail() is a single object and
// showOnly() needs something to hide -- so only the contents move.
//
// The cost does not vanish, it moves to the first open of each page: ~1.3 s the
// first time somebody taps WiFi. That is the right place for it. A deliberate
// tap on a settings row can afford a second; a boot cannot, and the person
// paying at boot is usually not the person who wanted the page.
bool built_[7] = {false, false, false, false, false, false, false};

// Forward declared: every page's close handler is back(), and back() is defined
// below because it is part of the navigation rather than of construction.
void back();

void ensureBuilt(lv_obj_t *page) {
  if (page == detailCalib_ && !built_[4]) {
    built_[4] = true;
    buildCalibPage(detailCalib_);
    calibOnClose(back);
    // The keypad is prefilled by showOnly() on every entry, including this one,
    // so a page built moments ago still opens with the stored mass.
  } else if (page == detailWifi_ && !built_[1]) {
    built_[1] = true;
    buildWifiPage(detailWifi_);
    wifiOnClose(back);
  } else if (page == detailBatt_ && !built_[2]) {
    built_[2] = true;
    buildBatteryPage(detailBatt_);
    batteryOnClose(back);
  } else if (page == detailSensor_ && !built_[5]) {
    built_[5] = true;
    buildScope(detailSensor_);
    scopeOnClose(back);
  } else if (page == detailDevice_ && !built_[6]) {
    built_[6] = true;
    buildDevicePage(detailDevice_);
    deviceOnClose(back);
  }
}

void push(lv_obj_t *page) {
  if (!page) return;
  // BEFORE showOnly(), because showOnly() reaches into the calibration page to
  // re-prefill the keypad and there has to be a keypad to reach into.
  ensureBuilt(page);
  if (depth_ < 4) stack_[depth_++] = page;
  showOnly(page);
}

void back() {
  if (depth_ == 0) return;
  // Leaving the WiFi page clears the passphrase field at any depth. It is
  // deliberately unmasked, so it must not survive navigation away from it.
  if (stack_[depth_ - 1] == detailWifi_) wifiResetView();
  depth_--;
  if (depth_ == 0) showOnly(nullptr);
  else showOnly(stack_[depth_ - 1]);
}

void openSettings() { push(detailSettings_); }
void openWifi() { push(detailWifi_); }
void openBattery() { push(detailBatt_); }
void openScale() { push(detailScale_); }
void openCalib() { push(detailCalib_); }
void openSensor() { push(detailSensor_); }
void openDevice() { push(detailDevice_); }

// Every overlay is the same shape: pinned below the status bar and taken out of
// the screen's flex flow. Without IGNORE_LAYOUT it is a flex ITEM placed after
// the tileview, so it starts STATUS_H down and hangs that far off the bottom --
// which presented as a keyboard with its last row sliced off.
lv_obj_t *makeDetail(lv_obj_t *scr) {
  lv_obj_t *d = lv_obj_create(scr);
  lv_obj_add_flag(d, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_set_pos(d, 0, STATUS_H);
  lv_obj_set_size(d, SCREEN_W, SCREEN_H - STATUS_H);
  lv_obj_set_style_pad_all(d, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(d, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(d, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(d, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(d, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
  return d;
}

}  // namespace

void buildPages() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_clean(scr);
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // The screen is a COLUMN: status bar, then pages. The bar is a SIBLING of the
  // tileview rather than a child, which is the whole reason it survives a
  // swipe -- a tileview scrolls its children.
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(scr, 0, LV_PART_MAIN);

  lv_obj_t *status = lv_obj_create(scr);
  buildStatus(status);

  tv_ = lv_tileview_create(scr);
  lv_obj_set_width(tv_, LV_PCT(100));
  lv_obj_set_flex_grow(tv_, 1);
  lv_obj_set_style_bg_color(tv_, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_border_width(tv_, 0, LV_PART_MAIN);

  // Menu on the LEFT, stock on the right and home. That way home is where a
  // swipe RIGHT lands, matching the back-toward-home direction used everywhere
  // else in the tree.
  tileMenu_ = lv_tileview_add_tile(tv_, 0, 0, LV_DIR_RIGHT);
  menuRoot_ = menuCreate(tileMenu_, "Menu", nullptr);
  menuAddRow(menuRoot_, "Settings", nullptr, openSettings);
  menuAddRow(menuRoot_, "Sensors", nullptr, openSensor);

  // HOME, ON THE GEAR'S ROW BUT MIRRORED TO THE LEFT MARGIN. The dashboard's
  // action row is 64 px tall against the bottom padding, and the gear is the
  // right-hand 64 px of it:
  //
  //     gear   x = 8 (page pad) + 152 (TARE) + 8 (gap) = 168, w = 64
  //     home   x = 8 (the same page pad, from the left)
  //     both   y = 294 - 8 (page pad) - 64             = 222, h = 64
  //
  // SAME BAND, OPPOSITE END. Sharing the exact coordinates put both buttons
  // under one thumb, which is quick but means the pixel that leaves a page is
  // the pixel that returns to it -- a mis-timed double tap goes out and back
  // and looks like the tap did nothing. At opposite ends of the same row the
  // travel is still one movement, and the two directions can never be confused
  // for each other. x = 8 is the dashboard's own page padding, so the home
  // button lines up with the left edge of TARE rather than against the bezel.
  //
  // IGNORE_LAYOUT takes it out of the menu's flex column so those numbers mean
  // what they say. It sits below both rows -- the list ends at 162 -- so it
  // covers nothing, and a third row would still clear it.
  //
  // AND THE TILE'S PADDING IS SUBTRACTED, because lv_obj_set_pos is relative to
  // the CONTENT box rather than the border box. Passing dashboard coordinates
  // straight in put the button six pixels down and right of where they said,
  // which is exactly the tile's padding and exactly the kind of near-miss
  // nothing reports. The positions are read back with lv_obj_get_coords in the
  // simulator rather than derived on paper.
  //
  // A HOUSE, not the wordmark it briefly carried. DBF is Dadabhagwan
  // Foundation; it names who made the device, which is not what a navigation
  // control should say. The glyph says where the button goes.
  {
    const int32_t padL = lv_obj_get_style_pad_left(tileMenu_, LV_PART_MAIN);
    const int32_t padT = lv_obj_get_style_pad_top(tileMenu_, LV_PART_MAIN);
    lv_obj_t *home = lv_button_create(tileMenu_);
    lv_obj_add_flag(home, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_pos(home, 8 - padL, 222 - padT);
    lv_obj_set_size(home, 64, 64);
    lv_obj_set_style_radius(home, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(home, lv_color_hex(0x21262D), LV_PART_MAIN);
    lv_obj_add_event_cb(home, [](lv_event_t *) { goToHome(); }, LV_EVENT_CLICKED,
                        nullptr);
    lv_obj_t *hl = lv_label_create(home);
    lv_obj_set_style_text_font(hl, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(hl, lv_color_hex(C_MUTED), LV_PART_MAIN);
    lv_label_set_text(hl, LV_SYMBOL_HOME);
    lv_obj_center(hl);
  }

  tileHome_ = lv_tileview_add_tile(tv_, 1, 0, LV_DIR_LEFT);
  lv_obj_set_style_pad_all(tileHome_, 0, LV_PART_MAIN);
  buildWeight(tileHome_);
  weightOnSettings(goToMenu);

  // Overlays, created AFTER the tileview so they stack above it.
  detailSettings_ = makeDetail(scr);
  settingsMenu_ = menuCreate(detailSettings_, "Settings", back);
  menuAddRow(settingsMenu_, "WiFi", nullptr, openWifi);
  menuAddRow(settingsMenu_, "Battery", nullptr, openBattery);
  menuAddRow(settingsMenu_, "Scale", nullptr, openScale);
  // DIAGNOSE LIVES HERE NOW, not at the top level. It carries the per-cell
  // breakdown that used to sit on the dashboard -- each cell's share, its zero,
  // its tare, its raw conversions and its rate. All of it is real information
  // and none of it belongs on a screen read at a glance by somebody carrying a
  // bowl; it is read deliberately, by somebody who came looking for it.
  menuAddRow(settingsMenu_, "Diagnose", nullptr, openDevice);
  // PRECISION SITS HERE, BESIDE THE PAGE THAT IGNORES IT. Diagnose always shows
  // three decimals -- it is the page you open when you distrust the number, and
  // rounding the evidence to match the dashboard would be the wrong favour. So
  // this row changes the DASHBOARD's last digit only, and its neighbour is the
  // place to go when you want the unrounded figure back.
  //
  // A cycling row for the same reason Average is one: three ordered values, and
  // the way you choose between them is to flip with a mass on the platform and
  // watch which one holds still. The hint shows the format itself rather than
  // the digit count, because "0.00 kg" answers the question the row asks and
  // "2" needs translating first.
  menuAddRow(settingsMenu_, "Precision", nullptr, doCyclePrecision);

  // The Scale page is three rows rather than a screen of its own, which is the
  // whole reason ui_menu exists: adding a setting is adding a row. Tare is
  // first because it is the one done every service; calibration is done once
  // per assembly and clearing it almost never.
  detailScale_ = makeDetail(scr);
  scaleMenu_ = menuCreate(detailScale_, "Scale", back);
  menuAddRow(scaleMenu_, "Tare", nullptr, doTare);
  // THE COMMISSIONING ZERO, and it sits directly under the everyday one so the
  // difference between them is impossible to miss. Tare is volatile and is what
  // gets pressed before a measurement; this one is written to NVS and describes
  // the platform bolted to this particular device, which is a thing you do once
  // and then never think about again.
  menuAddRow(scaleMenu_, "Set platform zero", nullptr, doPlatformZero);
  // OPENS A PAGE rather than acting. The mass used to be baked into this label
  // from a build flag, which assumed the person calibrating owns the weight the
  // firmware was compiled against -- when what they actually own is whatever is
  // to hand. The hint carries the last mass used on this unit, so the row still
  // says what it will do.
  menuAddRow(scaleMenu_, "Calibrate", nullptr, openCalib);
  menuAddRow(scaleMenu_, "Clear calibration", nullptr, doClearCal);
  // A ROW THAT CYCLES rather than a sub-page of five radio buttons. There are
  // five values, they are ordered, and the whole point of the setting is to
  // flip between two of them with a mass on the platform and watch which reads
  // better -- which a tap-to-advance row does in one gesture and a sub-page
  // does in four. The current value is the hint on the right.
  menuAddRow(scaleMenu_, "Average", nullptr, doCycleAvg);
  // A TOGGLE, in the same tap-to-change shape as Average and Precision above
  // it: the hint on the right says which way it is set, so the row states the
  // current value rather than only the action.
  //
  // It sits under Scale rather than under a Display heading because there is no
  // Display heading and inventing one for a single row would be a menu built
  // around a taxonomy instead of around what people do.
  menuAddRow(scaleMenu_, "Cells on home", nullptr, doToggleCells);
  // THE WAY BACK. Without it, "Clear calibration" could not be allowed to stick
  // -- clearing had to leave the NVS key absent so a reflash with a corrected
  // default could take effect, which meant a unit cleared on purpose came back
  // from its next power cycle showing kilograms again. With a deliberate route
  // to the built-in figure, clearing can mean cleared and this row is the
  // undo.
  menuAddRow(scaleMenu_, "Restore default", nullptr, doRestore);

  // CONTAINERS ONLY. Each page's contents are built the first time it is
  // opened -- see ensureBuilt() above for the measurements that moved them.
  detailCalib_ = makeDetail(scr);
  detailWifi_ = makeDetail(scr);
  detailBatt_ = makeDetail(scr);
  detailSensor_ = makeDetail(scr);
  detailDevice_ = makeDetail(scr);

  // Start on the weight view -- what someone walking up to the station wants to
  // see. The menu is one swipe away.
  //
  // THE LAYOUT IS FORCED FIRST, AND IT HAS TO BE. lv_tileview_set_tile() is a
  // scroll: it moves the tileview's content to the tile's coordinates. Until a
  // layout pass has run, every tile is still at 0,0 -- so the scroll goes to the
  // menu tile, and the first thing the device shows after a reset is the menu
  // rather than the weight it was switched on to read.
  //
  // This was latent rather than new. It used to work because five full detail
  // pages were constructed after this point and something in that traffic
  // happened to lay the tileview out in time; building those pages lazily
  // removed the accident and the bug surfaced immediately. The accident was
  // never the reason it worked, so the fix is to state the dependency rather
  // than restore the traffic.
  lv_obj_update_layout(scr);
  lv_tileview_set_tile(tv_, tileHome_, LV_ANIM_OFF);
  lastActive_ = nullptr;
  depth_ = 0;
}

void pagesGoHome() {
  closeAll();
  if (tv_ && tileHome_) lv_tileview_set_tile(tv_, tileHome_, LV_ANIM_OFF);
}

void pagesBack() { back(); }

void pagesOnScaleTare(void (*cb)(void)) { onTare_ = cb; }
void pagesOnScaleClearCal(void (*cb)(void)) { onClearCal_ = cb; }
void pagesOnScaleCycleAvg(void (*cb)(void)) { onCycleAvg_ = cb; }
void pagesOnScaleCyclePrecision(void (*cb)(void)) { onCyclePrecision_ = cb; }
void pagesOnScaleRestore(void (*cb)(void)) { onRestore_ = cb; }
void pagesOnScalePlatformZero(void (*cb)(void)) { onPlatformZero_ = cb; }
void pagesOnScaleToggleCells(void (*cb)(void)) { onToggleCells_ = cb; }

void pagesTick(uint32_t nowMs) {
  // --- data: always, for every page, visible or not ------------------------
  // Neither touches an LVGL object nor invalidates anything, so both stay cheap
  // however many pages exist. In the shipping firmware the converters do not
  // stop sampling because someone swiped, and a page that only collected while
  // visible would show a gap on return.
  demoTick(nowMs);
  scopeSample(nowMs);

  const State &s = demoLatest(nowMs);

  // Never hidden, so it always renders -- updateStatus() early-outs on
  // unchanged values, so a steady state costs comparisons rather than a redraw.
  updateStatus(s);

  // Hints, so the menu answers something at a glance instead of being a list of
  // nouns you must open to learn anything from. menuSetHint compares before
  // writing, so a steady state costs two string compares a frame.
  if (menuRoot_) {
    // BUILT FROM THE COUNT rather than written out. The old version read
    // "1 of 2" as a literal, which was true only because two cells have exactly
    // one interesting partial state; with three it would have called two
    // working cells one.
    static char cells[12];
    const char *hint = "";
    if (s.scale.online == 0) {
      hint = "no cells";
    } else if (s.scale.online < CELLS) {
      snprintf(cells, sizeof(cells), "%u of %u", s.scale.online, CELLS);
      hint = cells;
    }
    menuSetHint(menuRoot_, 1, hint);
  }
  if (settingsMenu_) {
    // The Scale row carries the one fact that decides what the whole dashboard
    // can say: with no calibration there are no grams anywhere in the product.
    menuSetHint(settingsMenu_, ROW_SET_SCALE, s.scale.calibrated ? "" : "uncal");
    // Built from the setting rather than a lookup table, so a fourth choice
    // could never be added to scale.h and leave this row describing the third.
    static char prec[12];
    const uint8_t d = s.scale.decimals ? s.scale.decimals : 3;
    snprintf(prec, sizeof(prec), "0.%0*d kg", (int)d, 0);
    menuSetHint(settingsMenu_, ROW_SET_PRECISION, prec);
  }
  if (scaleMenu_) {
    // The Scale menu's own rows, updated whenever it exists rather than only
    // while it is on screen -- menuSetHint compares before writing, so a hidden
    // page costs one string compare a frame and is correct the instant it is
    // opened rather than one frame later.
    menuSetHint(scaleMenu_, ROW_SCALE_CELLS, s.scale.showCells ? "shown" : "hidden");
  }
  if (scaleMenu_) {
    // The Average row shows the value it will change, which is what makes a
    // tap-to-cycle row usable at all -- otherwise you are guessing where in the
    // sequence you are.
    // Row 1 is Calibrate: its hint is the mass it will open pre-filled with, so
    // the row still says what it does now that the label no longer can.
    lastCalMassG_ = s.scale.calMassG;
    static char mass[12];
    snprintf(mass, sizeof(mass), "%ld g", (long)(s.scale.calMassG + 0.5f));
    menuSetHint(scaleMenu_, ROW_SCALE_CALIBRATE, s.scale.calMassG > 0.0f ? mass : "");
    static char avg[8];
    snprintf(avg, sizeof(avg), "%u", s.scale.window);
    menuSetHint(scaleMenu_, ROW_SCALE_AVERAGE, avg);
  }

  wifiTick();
  // Return value CAPTURED, not discarded. perfTick() is true only when a fresh
  // one-second window has closed, and it is what gates the on-screen readout.
  // Losing that guard in the menu restructure meant scopeShowPerf() rewrote its
  // label on every tick -- measured at 31 fps and 64% of a core on a page whose
  // data arrives at 10 Hz, which is the same "write unconditionally" mistake
  // this whole optimisation pass was about.
  const bool perfFresh = perfTick(nowMs);

#if UI_AUTO_CYCLE_MS
  {
    static uint32_t nextSwap = 0;
    static bool onScope = false;
    if ((int32_t)(nowMs - nextSwap) >= 0) {
      nextSwap = nowMs + UI_AUTO_CYCLE_MS;
      onScope = !onScope;
      closeAll();
      if (onScope) openSensor();
      else if (tv_) lv_tileview_set_tile(tv_, tileHome_, LV_ANIM_OFF);
    }
  }
#else
  // lv_display_get_inactive_time() is LVGL's own count since any input device
  // reported activity, so this needs no touch plumbing and cannot disagree with
  // what the indev saw. Guarded on not-already-home, because setting the
  // current tile still invalidates it, and firing that every frame once idle
  // would be a permanent redraw for no change.
  if (lv_display_get_inactive_time(NULL) > IDLE_HOME_MS) {
    const bool home =
        (depth_ == 0) && (!tv_ || lv_tileview_get_tile_active(tv_) == tileHome_);
    if (!home) pagesGoHome();
  }
#endif

  // --- rendering: only what is actually on screen --------------------------
  // THIS IS WHAT KEEPS THE FRAME RATE FLAT AS PAGES ARE ADDED. Cost scales with
  // what is visible, not with how many pages exist -- which is the whole reason
  // a menu tree can grow without the frame rate paying for it.
  if (depth_ > 0) {
    lv_obj_t *top = stack_[depth_ - 1];
    if (top == detailBatt_) {
      updateBatteryPage(s);
    } else if (top == detailScale_) {
      // Nothing to render: the rows are static and their live values are the
      // hints, which are written below regardless of which page is up.
    } else if (top == detailDevice_) {
      updateDevicePage(s);
    } else if (top == detailCalib_) {
      updateCalibPage(s);
    } else if (top == detailSensor_) {
      scopeRender();
      if (perfFresh) scopeShowPerf();
    }
    return;
  }

  lv_obj_t *active = lv_tileview_get_tile_active(tv_);
  if (active != lastActive_) lastActive_ = active;
  if (active == tileHome_) updateWeight(s);
  else weightPageHidden();
}

}  // namespace ui
