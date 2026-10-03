#include "ui_pages.h"

#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "ui_battery.h"
#include "ui_bufcal.h"
#include "ui_demo.h"
#include "ui_calib.h"
#include "ui_vessel.h"
#include "ui_device.h"
#include "ui_menu.h"
#include "ui_perf.h"
#include "ui_scope.h"
#include "ui_screens.h"
#include "ui_status.h"
#include "ui_knob.h"
#include "ui_weight.h"
#include "ui_wifi.h"

namespace ui {
namespace {

const uint32_t C_BG = 0x000000;
const uint32_t C_MUTED = 0x8B949E;

lv_obj_t *tv_ = nullptr;
lv_obj_t *tileMenu_ = nullptr;
lv_obj_t *tileHome_ = nullptr;
// TRIAL HARNESS. The blinded page -- see include/ui_knob.h for why it exists.
lv_obj_t *tileKnob_ = nullptr;
uint8_t defaultPage_ = 0;
void (*onCycleDefaultPage_)(void) = nullptr;
void doCycleDefaultPage() { if (onCycleDefaultPage_) onCycleDefaultPage_(); }
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
lv_obj_t *detailVessel_ = nullptr;
lv_obj_t *detailSensor_ = nullptr;
lv_obj_t *detailDevice_ = nullptr;
// Settings > Buffers: the list, one page per buffer platform, and the kg keypad.
lv_obj_t *detailBuffers_ = nullptr;
lv_obj_t *detailBuf_[BUFFERS] = {nullptr, nullptr, nullptr};
lv_obj_t *detailBufCal_ = nullptr;

// The deepest route is Settings > Buffers > B1 > Calibrate, FOUR overlays; the
// stack holds six so there is still a slot spare, which is what this comment said
// of the old depth of three. A stack rather than a single "previous" pointer, so
// Back stays unambiguous once a page can be reached from more than one place.
const uint8_t STACK_MAX = 6;
lv_obj_t *stack_[STACK_MAX] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
uint8_t depth_ = 0;

// Installed by the firmware; absent in the desktop preview, where there are no
// cells to tare. A null handler leaves the row inert rather than pretending.
void (*onTare_)(void) = nullptr;
void (*onClearCal_)(void) = nullptr;
void (*onCycleAvg_)(void) = nullptr;
void (*onRestore_)(void) = nullptr;
void (*onPlatformZero_)(void) = nullptr;
// The buffer platforms' commands, by slot. Installed by the firmware; null in the
// preview, where there is nothing to zero, so the rows are inert there.
void (*onBufZero_)(uint8_t) = nullptr;
void (*onBufCal_)(uint8_t, float) = nullptr;
void (*onBufBowls_)(uint8_t, int8_t) = nullptr;
void (*onBufClear_)(uint8_t) = nullptr;

// The last mass the snapshot carried, cached so showOnly() can re-prefill the
// keypad without a State to hand. pagesTick() keeps it current.
float lastCalMassG_ = 0.0f;
// Mirrors lastCalMassG_: showOnly() prefills the keypad from it on every entry.
float lastVesselG_ = 0.0f;

lv_obj_t *settingsMenu_ = nullptr;
lv_obj_t *scaleMenu_ = nullptr;
lv_obj_t *buffersMenu_ = nullptr;
lv_obj_t *bufMenu_[BUFFERS] = {nullptr, nullptr, nullptr};
// The labels the buffer pages were last shown under, for the keypad's title, and the
// platform the keypad is calibrating.
char bufLabel_[BUFFERS][4] = {"B1", "B2", "B3"};
uint8_t calSlot_ = 0;

// ROW POSITIONS, NAMED. menuSetHint() addresses rows by index, and inserting
// "Set platform zero" at position 1 silently moved the calibration mass onto it
// and the averaging figure onto "Clear calibration" -- two hints attached to
// two labels that had nothing to do with them, with nothing to fail. Naming the
// positions next to the rows themselves does not make that impossible, but it
// puts the two things a reader has to keep in step within a screen of each
// other.
//
// TWO ROWS LEFT AND ONE ARRIVED with the per-platform dashboard. Precision and
// "Cells on home" both configured the old dashboard -- its decimal places and its
// A/B/C rows -- and that page now draws neither (one decimal, platform rows), so a
// row that changed nothing visible was removed rather than left lying. Buffers is
// new, between Scale and Diagnose. Every index below them moved, which is exactly
// the change the paragraph above is about.
enum : uint8_t {
  ROW_SET_WIFI = 0,
  ROW_SET_BATTERY,
  ROW_SET_SCALE,
  ROW_SET_BUFFERS,
  ROW_SET_DIAGNOSE,
  ROW_SET_DEFAULT_PAGE,  // TRIAL HARNESS
};
enum : uint8_t {
  ROW_SCALE_TARE = 0,
  ROW_SCALE_PLATFORM_ZERO,
  ROW_SCALE_CALIBRATE,
  ROW_SCALE_CLEAR,
  ROW_SCALE_AVERAGE,
  ROW_SCALE_VESSEL,
  ROW_SCALE_RESTORE,
};
// One buffer platform's page. The first three rows are readouts (no handler, so
// inert); the last is the outcome of whatever was last asked of the platform.
enum : uint8_t {
  ROW_BUF_ID = 0,
  ROW_BUF_READING,
  ROW_BUF_BOWLS,
  ROW_BUF_BOWLS_UP,
  ROW_BUF_BOWLS_DOWN,
  ROW_BUF_ZERO,
  ROW_BUF_CALIBRATE,
  ROW_BUF_CLEAR,
  ROW_BUF_LAST,
};

// TARE, THEN OUT TO THE WEIGHT PAGE. The navigation is the confirmation.
//
// This action used to have a button of its own on the dashboard, and it was
// getting pressed by accident: widest target on the screen, thumb height, on
// the one page used with a bowl in the other hand. A stray tare is not a
// visible mistake -- it silently redefines zero and every reading afterwards
// inherits it -- so it moved three taps deep, here.
//
// That fixed the mis-taps and cost the operator the one thing the dashboard
// button gave them for free: watching the total drop. So this row gives it
// back by navigating. Tare, close the overlays, land on the weight page, where
// the 84 px total says 0.0 kg if it took and something else if it did not.
// No confirmation widget and no toast to time out -- the number that is
// already that page's entire purpose does the job, and it keeps saying it.
//
// THE WEIGHT TILE, NOT defaultTile(). Every other route home deliberately goes
// through defaultTile(), and this one deliberately does not: during the trial
// the default is the blinded knob page, which shows no total and would confirm
// nothing at all. Landing on the weight page is safe because the idle timeout
// still routes through defaultTile(), so the panel re-blinds itself a minute
// after whoever tared walks away.
void closeAll();
void goToHome();
void doTare() {
  if (onTare_) onTare_();
  closeAll();
  goToHome();
}
void doClearCal() { if (onClearCal_) onClearCal_(); }
void doCycleAvg() { if (onCycleAvg_) onCycleAvg_(); }
void doRestore() { if (onRestore_) onRestore_(); }
// Home afterwards, for doTare()'s reason: the weight page is the confirmation. An
// empty platform already reading 0.045 kg shows "0.0kg" before and after, so a
// row that stayed put looked like a tap that did nothing.
void doPlatformZero() {
  if (onPlatformZero_) onPlatformZero_();
  closeAll();
  goToHome();
}

// --- buffer platform actions -----------------------------------------------------
// ZERO AND CLEAR TAKE TWO TAPS. Both are stored, and a stray one is not visible as
// a mistake: a zero taken with stock on the shelf makes every later reading short by
// that stock, and a cleared factor turns the platform's kilograms off. The first tap
// arms the row (its hint says "tap again") and a second within ARM_MS commits; the
// counter's Tare got the same protection by being moved three taps deep.
const uint32_t ARM_MS = 3000;
uint32_t armZeroUntil_[BUFFERS] = {0, 0, 0};
uint32_t armClearUntil_[BUFFERS] = {0, 0, 0};
bool armed(uint32_t until) { return until && (int32_t)(until - lv_tick_get()) > 0; }

void bufZero(uint8_t i) {
  if (armed(armZeroUntil_[i])) {
    armZeroUntil_[i] = 0;
    if (onBufZero_) onBufZero_(i);
  } else {
    armZeroUntil_[i] = lv_tick_get() + ARM_MS;
  }
}
void bufClear(uint8_t i) {
  if (armed(armClearUntil_[i])) {
    armClearUntil_[i] = 0;
    if (onBufClear_) onBufClear_(i);
  } else {
    armClearUntil_[i] = lv_tick_get() + ARM_MS;
  }
}
void bufBowls(uint8_t i, int8_t d) {
  if (onBufBowls_) onBufBowls_(i, d);
}

// Menu rows take a plain function pointer, so each slot gets its own trampolines.
template <uint8_t I> void bufZeroT() { bufZero(I); }
template <uint8_t I> void bufClearT() { bufClear(I); }
template <uint8_t I> void bufUpT() { bufBowls(I, +1); }
template <uint8_t I> void bufDownT() { bufBowls(I, -1); }

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

// WHERE THE DEVICE SETTLES, which is not always the weight page any more.
//
// Both the idle timeout and the first frame after boot route through here, so
// there is one answer to "which page is home" rather than two that can drift.
// During the trial that answer is the knob page, and the attendant never has to
// swipe back to it after walking away.
lv_obj_t *defaultTile() {
  if (defaultPage_ == 1 && tileKnob_) return tileKnob_;
  return tileHome_;
}
// TRIAL HARNESS. The OTHER main page, worked out from where the tileview
// actually is rather than from a remembered flag -- a flag would have to be
// kept in step with the swipe, and the first swipe that forgot to update it
// would make the button a no-op on one page and correct on the other.
void goToOther() {
  if (!tv_ || !tileKnob_ || !tileHome_) return;
  lv_obj_t *now = lv_tileview_get_tile_active(tv_);
  lv_tileview_set_tile(tv_, now == tileKnob_ ? tileHome_ : tileKnob_, LV_ANIM_OFF);
}

void goToDefault() {
  if (tv_ && defaultTile()) lv_tileview_set_tile(tv_, defaultTile(), LV_ANIM_OFF);
}

void showOnly(lv_obj_t *which) {
  lv_obj_t *all[] = {detailSettings_, detailWifi_,   detailBatt_,     detailScale_,
                     detailCalib_,    detailSensor_, detailDevice_,   detailVessel_,
                     detailBuffers_,  detailBuf_[0], detailBuf_[1],   detailBuf_[2],
                     detailBufCal_};
  // sizeof rather than a literal. This count was hand-written and had to be
  // edited in step with the array beside it; a page added without touching it
  // would simply never be hidden again.
  for (uint8_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
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
  // Same reasoning as the line above: prefilled on every entry, so the page
  // always opens showing what the unit currently has.
  if (which == detailVessel_) vesselSetOffset(lastVesselG_);
  // And the buffer keypad is told WHICH platform it calibrates, and emptied, on
  // every entry: a half-typed mass for B1 must never be applied to B2.
  if (which == detailBufCal_) bufCalOpenFor(calSlot_, bufLabel_[calSlot_]);
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
bool built_[9] = {false, false, false, false, false, false, false, false, false};

// Forward declared: every page's close handler is back(), and back() is defined
// below because it is part of the navigation rather than of construction.
void back();

// --- Settings > Buffers, built on first open ----------------------------------------
// A list of the platforms, then one page each. Rows rather than a bespoke screen for
// the reason the Scale page gives: adding a setting is adding a row. Per-slot pages
// rather than one page with a selector, because a selector that is easy to leave on
// the wrong platform is how B1 gets zeroed with B2's stock on it.
template <uint8_t I> void openBufT();
template <uint8_t I> void openBufCalT();

void buildBuffersMenu() {
  buffersMenu_ = menuCreate(detailBuffers_, "Buffers", back);
  menuAddRow(buffersMenu_, "B1", nullptr, openBufT<0>);
  menuAddRow(buffersMenu_, "B2", nullptr, openBufT<1>);
  menuAddRow(buffersMenu_, "B3", nullptr, openBufT<2>);
}

void buildBufMenu(uint8_t i) {
  static void (*const ZERO[BUFFERS])(void) = {bufZeroT<0>, bufZeroT<1>, bufZeroT<2>};
  static void (*const CLEAR[BUFFERS])(void) = {bufClearT<0>, bufClearT<1>, bufClearT<2>};
  static void (*const UP[BUFFERS])(void) = {bufUpT<0>, bufUpT<1>, bufUpT<2>};
  static void (*const DOWN[BUFFERS])(void) = {bufDownT<0>, bufDownT<1>, bufDownT<2>};
  static void (*const CAL[BUFFERS])(void) = {openBufCalT<0>, openBufCalT<1>, openBufCalT<2>};
  static const char *const TITLE[BUFFERS] = {"Buffer B1", "Buffer B2", "Buffer B3"};
  bufMenu_[i] = menuCreate(detailBuf_[i], TITLE[i], back);
  // Order: what it is and what it reads, then the everyday correction, then the
  // commissioning actions, then the outcome. Indices are the ROW_BUF_* above.
  menuAddRow(bufMenu_[i], "Platform", nullptr, nullptr);
  menuAddRow(bufMenu_[i], "Reading", nullptr, nullptr);
  menuAddRow(bufMenu_[i], "Bowls", nullptr, nullptr);
  menuAddRow(bufMenu_[i], "Bowls +1", nullptr, UP[i]);
  menuAddRow(bufMenu_[i], "Bowls -1", nullptr, DOWN[i]);
  menuAddRow(bufMenu_[i], "Zero (empty)", nullptr, ZERO[i]);
  menuAddRow(bufMenu_[i], "Calibrate", nullptr, CAL[i]);
  menuAddRow(bufMenu_[i], "Clear cal", nullptr, CLEAR[i]);
  menuAddRow(bufMenu_[i], "Last", nullptr, nullptr);
}

void ensureBuilt(lv_obj_t *page) {
  // The buffer pages: their menus are the hints' targets, and pagesTick() already
  // skips a menu that is still null, so a page nobody has opened costs nothing.
  if (page == detailBuffers_ && !buffersMenu_) {
    buildBuffersMenu();
    return;
  }
  for (uint8_t i = 0; i < BUFFERS; i++) {
    if (page == detailBuf_[i] && !bufMenu_[i]) {
      buildBufMenu(i);
      return;
    }
  }
  if (page == detailBufCal_ && !built_[8]) {
    built_[8] = true;
    buildBufCalPage(detailBufCal_);
    bufCalOnClose(back);
    bufCalOnApply([](uint8_t slot, float grams) {
      if (onBufCal_) onBufCal_(slot, grams);
    });
  } else if (page == detailVessel_ && !built_[7]) {
    built_[7] = true;
    buildVesselPage(detailVessel_);
    vesselOnClose(back);
  } else if (page == detailCalib_ && !built_[4]) {
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
  if (depth_ < STACK_MAX) stack_[depth_++] = page;
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
void openVessel() { push(detailVessel_); }
void openSensor() { push(detailSensor_); }
void openDevice() { push(detailDevice_); }
void openBuffers() { push(detailBuffers_); }
template <uint8_t I> void openBufT() { push(detailBuf_[I]); }
template <uint8_t I> void openBufCalT() {
  calSlot_ = I;
  push(detailBufCal_);
}

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
  // action row is 56 px tall against the bottom padding, and the gear is the
  // right-hand 56 px of it:
  //
  //     gear   x = 224 (content width) - 56 = 168, w = 56, right-anchored
  //     home   x = 8 (the same page pad, from the left), w = 56
  //     both   y = 278 (content height) - 56 = 222, h = 56
  //
  // THIS USED TO BE DERIVED LEFT TO RIGHT, THROUGH TARE: "8 + 152 (TARE) + 8
  // = 168, w = 64". Both numbers had already rotted -- TARE shrank to 96 when
  // the swap button was added, and the gear has been 56 wide, not 64, since
  // then -- and it only landed on the right answer because two errors of 56
  // and -56 cancelled. It cannot rot again: the row is LV_FLEX_ALIGN_END now,
  // so the gear is anchored to the RIGHT margin and nothing placed to its left
  // can move it. The arithmetic above runs right to left for that reason.
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
    // 56, MATCHING THE GEAR. Both dropped from 64 when the dashboard's action
    // row was shortened to make room for the bigger total; leaving this one at
    // 64 would have made "same band, opposite end" false by 8 px at the bottom.
    lv_obj_set_size(home, 56, 56);
    lv_obj_set_style_radius(home, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(home, lv_color_hex(0x21262D), LV_PART_MAIN);
    // THE DEFAULT PAGE, not the weight page. This button and the idle timeout
    // and the boot route all mean "take me to the page this device lives on",
    // and having one of the three still hardcoded to the weight tile is exactly
    // how a setting acquires a stale corner: change the default to Knob, press
    // home, and land somewhere else.
    lv_obj_add_event_cb(home, [](lv_event_t *) { goToDefault(); },
                        LV_EVENT_CLICKED, nullptr);
    lv_obj_t *hl = lv_label_create(home);
    lv_obj_set_style_text_font(hl, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(hl, lv_color_hex(C_MUTED), LV_PART_MAIN);
    lv_label_set_text(hl, LV_SYMBOL_HOME);
    lv_obj_center(hl);
  }

  // LEFT to the menu, RIGHT to the knob page. Adding the direction is what
  // makes the third tile reachable at all -- a tileview will not scroll toward
  // a neighbour the tile does not declare.
  tileHome_ = lv_tileview_add_tile(tv_, 1, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
  lv_obj_set_style_pad_all(tileHome_, 0, LV_PART_MAIN);
  buildWeight(tileHome_);
  weightOnSwap(goToOther);  // TRIAL HARNESS

  // TRIAL HARNESS: the blinded page, one swipe right of the weight page.
  //
  // ORDERED AFTER the weight page rather than before it, so the trial does not
  // renumber anything: menu stays 0, weight stays 1, and removing this tile
  // leaves the other two exactly where they were.
  tileKnob_ = lv_tileview_add_tile(tv_, 2, 0, LV_DIR_LEFT);
  lv_obj_set_style_pad_all(tileKnob_, 0, LV_PART_MAIN);
  buildKnob(tileKnob_);
  knobOnSettings(goToMenu);
  knobOnSwap(goToOther);
  weightOnSettings(goToMenu);

  // Overlays, created AFTER the tileview so they stack above it.
  detailSettings_ = makeDetail(scr);
  settingsMenu_ = menuCreate(detailSettings_, "Settings", back);
  menuAddRow(settingsMenu_, "WiFi", nullptr, openWifi);
  menuAddRow(settingsMenu_, "Battery", nullptr, openBattery);
  menuAddRow(settingsMenu_, "Scale", nullptr, openScale);
  // THE BUFFER PLATFORMS, beside the counter's Scale page rather than inside it:
  // they are separate instruments with their own zero, factor and bowl count, and a
  // Scale page that mixed the two would have a Tare that taring did not mean.
  menuAddRow(settingsMenu_, "Buffers", nullptr, openBuffers);
  // DIAGNOSE LIVES HERE NOW, not at the top level. It carries the per-cell
  // breakdown that used to sit on the dashboard -- each cell's share, its zero,
  // its tare, its raw conversions and its rate. All of it is real information
  // and none of it belongs on a screen read at a glance by somebody carrying a
  // bowl; it is read deliberately, by somebody who came looking for it.
  menuAddRow(settingsMenu_, "Diagnose", nullptr, openDevice);
  // NO PRECISION ROW ANY MORE. It set the dashboard's decimal places, and the
  // dashboard now shows every platform to one decimal so a column of figures can be
  // compared at a glance (see ui_weight.cpp). Diagnose still shows every place.
  // TRIAL HARNESS. Which page the device returns to when left alone and which
  // one it opens on after a power cycle.
  menuAddRow(settingsMenu_, "Default page", nullptr, doCycleDefaultPage);

  // The Scale page is three rows rather than a screen of its own, which is the
  // whole reason ui_menu exists: adding a setting is adding a row. Tare is
  // first because it is the one done every service -- and since the dashboard
  // button was removed it is the ONLY way to tare from the panel, which is
  // another reason it must not be buried under the once-per-assembly rows.
  // Calibration is done once per assembly and clearing it almost never.
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
  // NO "CELLS ON HOME" ROW ANY MORE: the dashboard no longer draws the counter's
  // A/B/C rows at all (they are on Diagnose), so the toggle had nothing to toggle.
  // THE WAY BACK. Without it, "Clear calibration" could not be allowed to stick
  // -- clearing had to leave the NVS key absent so a reflash with a corrected
  // default could take effect, which meant a unit cleared on purpose came back
  // from its next power cycle showing kilograms again. With a deliberate route
  // to the built-in figure, clearing can mean cleared and this row is the
  // undo.
  // THE EMPTY VESSEL'S OWN MASS, taken off what the panel shows and what the
  // uplink sends -- and off NEITHER the tare nor the per-cell figures, which
  // must stay on the real platform load. Ten vessels averaged about 2.5 kg.
  //
  // A cycling row, like Average and Precision above: the values worth having
  // are few and known, and a keypad for a number chosen from four options is
  // four gestures where one will do. Off is in the cycle, so the row is both
  // the enable and the value.
  menuAddRow(scaleMenu_, "Vessel offset", nullptr, openVessel);
  menuAddRow(scaleMenu_, "Restore default", nullptr, doRestore);

  // --- Settings > Buffers -----------------------------------------------------
  // CONTAINERS ONLY, like every detail page: the menus are built on first open (see
  // buildBuffersMenu() / buildBufMenu()). Building all four here -- thirty rows --
  // took buildPages() from ~2.0 s to ~8.5 s on the panel (V1.12/V1.13 boot lines):
  // the same trade the note above ensureBuilt() makes, re-learned.
  detailBuffers_ = makeDetail(scr);
  for (uint8_t i = 0; i < BUFFERS; i++) detailBuf_[i] = makeDetail(scr);

  // CONTAINERS ONLY. Each page's contents are built the first time it is
  // opened -- see ensureBuilt() above for the measurements that moved them.
  detailCalib_ = makeDetail(scr);
  detailVessel_ = makeDetail(scr);
  detailWifi_ = makeDetail(scr);
  detailBatt_ = makeDetail(scr);
  detailSensor_ = makeDetail(scr);
  detailDevice_ = makeDetail(scr);
  detailBufCal_ = makeDetail(scr);

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
  // SEEDED FROM THE FIXTURE, because pagesTick() has not run yet and the
  // static above is still zero. Without this the first tile is always the
  // weight page however the setting is stored -- and the setting would then
  // start working a second later, the moment the first tick arrived, which is
  // the sort of half-fix that reads as a flaky device.
  defaultPage_ = demoLatest(0).defaultPage;
  lv_tileview_set_tile(tv_, defaultTile(), LV_ANIM_OFF);
  lastActive_ = nullptr;
  depth_ = 0;
}

void pagesGoHome() {
  closeAll();
  // THE DEFAULT TILE, NOT THE WEIGHT TILE. This was the last of the three
  // routes still hardcoded, and it is the one that fires on its own.
  //
  // With the trial's default set to Knob the device booted blinded and the menu
  // home button returned there -- so the setting looked correct -- and then
  // sixty seconds of inactivity dragged the panel to the WEIGHT page and left
  // it there. The attendant walks back to kilograms and the per-cell breakdown:
  // precisely what the blinded page exists to hide, and the trial measures them
  // copying the scale's answer from that point on. (It was three things until
  // the TARE button left that page; the two that remain are the ones that
  // matter, because they are the answer the knob is supposed to be guessing.)
  //
  // It survived because it LOOKED covered: a third reference to defaultTile()
  // sits inside `#if UI_AUTO_CYCLE_MS`, which is defined nowhere in the repo,
  // so that branch is dead and only the boot route was ever live.
  if (tv_ && defaultTile()) lv_tileview_set_tile(tv_, defaultTile(), LV_ANIM_OFF);
}

void pagesBack() { back(); }

void pagesOnCycleDefaultPage(void (*cb)(void)) { onCycleDefaultPage_ = cb; }
void pagesOnScaleTare(void (*cb)(void)) { onTare_ = cb; }
void pagesOnScaleClearCal(void (*cb)(void)) { onClearCal_ = cb; }
void pagesOnScaleCycleAvg(void (*cb)(void)) { onCycleAvg_ = cb; }
void pagesOnVesselApply(void (*cb)(float)) { vesselOnApply(cb); }
void pagesOnScaleRestore(void (*cb)(void)) { onRestore_ = cb; }
void pagesOnScalePlatformZero(void (*cb)(void)) { onPlatformZero_ = cb; }
void pagesOnBufferZero(void (*cb)(uint8_t)) { onBufZero_ = cb; }
void pagesOnBufferCalibrate(void (*cb)(uint8_t, float)) { onBufCal_ = cb; }
void pagesOnBufferBowls(void (*cb)(uint8_t, int8_t)) { onBufBowls_ = cb; }
void pagesOnBufferClear(void (*cb)(uint8_t)) { onBufClear_ = cb; }

void pagesPreviewOpen(const char *name) {
  if (!name) return;
  closeAll();
  if (!strcmp(name, "settings")) push(detailSettings_);
  else if (!strcmp(name, "scale")) { push(detailSettings_); push(detailScale_); }
  else if (!strcmp(name, "buffers")) { push(detailSettings_); push(detailBuffers_); }
  else if (!strncmp(name, "buf", 3) && name[3] >= '1' && name[3] <= '3' && name[4] == 0) {
    push(detailSettings_);
    push(detailBuffers_);
    push(detailBuf_[name[3] - '1']);
  } else if (!strcmp(name, "bufcal")) {
    calSlot_ = 0;
    push(detailSettings_);
    push(detailBuffers_);
    push(detailBuf_[0]);
    push(detailBufCal_);
  } else if (!strcmp(name, "diagnose")) { push(detailSettings_); push(detailDevice_); }
}

void pagesBuildAll() {
  lv_obj_t *const all[] = {detailBuffers_, detailBuf_[0], detailBuf_[1], detailBuf_[2],
                           detailBufCal_,  detailCalib_,  detailVessel_, detailWifi_,
                           detailBatt_,    detailSensor_, detailDevice_};
  for (lv_obj_t *p : all) ensureBuilt(p);
}

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
    // How many buffer platforms answered -- the first thing to know before opening
    // the page, and "none" says so on a unit with no buffer bus at all.
    static char fitted[12];
    uint8_t nf = 0;
    for (uint8_t i = 0; i < BUFFERS; i++) nf += s.buffers[i].fitted ? 1 : 0;
    if (nf == 0) snprintf(fitted, sizeof(fitted), "none");
    else snprintf(fitted, sizeof(fitted), "%u fitted", nf);
    menuSetHint(settingsMenu_, ROW_SET_BUFFERS, fitted);
    menuSetHint(settingsMenu_, ROW_SET_DEFAULT_PAGE, s.defaultPage == 1 ? "Knob" : "Weight");
  }

  // --- the buffer pages' hints -------------------------------------------------
  // Written whenever the pages exist, visible or not -- menuSetHint compares before
  // writing, so a hidden page costs string compares and is right the instant it
  // opens.
  for (uint8_t i = 0; i < BUFFERS; i++) {
    const BufferInfo &b = s.buffers[i];
    if (b.fitted && b.label[0]) snprintf(bufLabel_[i], sizeof(bufLabel_[i]), "%s", b.label);
    char reading[24];
    if (!b.fitted) snprintf(reading, sizeof(reading), "not fitted");
    else if (b.kgKnown)
      snprintf(reading, sizeof(reading), "%.1f kg", b.foodG / 1000.0f);
    else
      snprintf(reading, sizeof(reading), "%s", b.why[0] ? b.why : "--");
    if (buffersMenu_) {
      char h[40];
      if (b.fitted && b.kgKnown)
        snprintf(h, sizeof(h), "%s  %u bw%s", reading, b.bowls, b.bowlsConfirmed ? "" : "?");
      else
        snprintf(h, sizeof(h), "%s", reading);
      menuSetHint(buffersMenu_, i, h);
    }
    lv_obj_t *m = bufMenu_[i];
    if (!m) continue;
    menuSetHint(m, ROW_BUF_ID, b.fitted ? b.uid : "not fitted");
    menuSetHint(m, ROW_BUF_READING, reading);
    char bw[16];
    snprintf(bw, sizeof(bw), "%u%s", b.bowls, b.bowlsConfirmed ? "" : " (unconf.)");
    menuSetHint(m, ROW_BUF_BOWLS, b.fitted ? bw : "");
    menuSetHint(m, ROW_BUF_ZERO, armed(armZeroUntil_[i]) ? "tap again" : (b.zeroed ? "stored" : "not set"));
    char cal[16];
    if (b.calibrated) snprintf(cal, sizeof(cal), "%.2f c/g", b.cpg);
    else snprintf(cal, sizeof(cal), "not set");
    menuSetHint(m, ROW_BUF_CALIBRATE, b.fitted ? cal : "");
    menuSetHint(m, ROW_BUF_CLEAR, armed(armClearUntil_[i]) ? "tap again" : "");
    // The outcome of the last command, cut to what a row can hold. The full sentence
    // is on the console; this is enough to say whether it worked.
    char last[20];
    snprintf(last, sizeof(last), "%.19s", b.lastResult);
    menuSetHint(m, ROW_BUF_LAST, last);
  }
  lastVesselG_ = s.scale.vesselOffsetG;
  if (scaleMenu_) {
    // The Average row shows the value it will change, which is what makes a
    // tap-to-cycle row usable at all -- otherwise you are guessing where in the
    // sequence you are.
    // The Calibrate row's hint is the mass it will open pre-filled with, so the
    // row still says what it does now that the label no longer can. Addressed
    // by the ROW_SCALE_CALIBRATE constant rather than a literal -- this comment
    // used to say "row 1" and Calibrate has been row 2 since Set platform zero
    // went in above it.
    lastCalMassG_ = s.scale.calMassG;
    static char mass[12];
    snprintf(mass, sizeof(mass), "%ld g", (long)(s.scale.calMassG + 0.5f));
    menuSetHint(scaleMenu_, ROW_SCALE_CALIBRATE, s.scale.calMassG > 0.0f ? mass : "");
    static char avg[8];
    snprintf(avg, sizeof(avg), "%u", s.scale.window);
    menuSetHint(scaleMenu_, ROW_SCALE_AVERAGE, avg);
    static char vessel[12];
    if (s.scale.vesselOffsetG <= 0.0f) snprintf(vessel, sizeof(vessel), "off");
    else snprintf(vessel, sizeof(vessel), "%.1f kg", s.scale.vesselOffsetG / 1000.0f);
    // "off" and "0.0 kg" are the same state; the row says the word because a
    // person scanning the menu is looking for whether it is on, not for a zero.
    menuSetHint(scaleMenu_, ROW_SCALE_VESSEL, vessel);
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
      else if (tv_) lv_tileview_set_tile(tv_, defaultTile(), LV_ANIM_OFF);
    }
  }
#else
  // lv_display_get_inactive_time() is LVGL's own count since any input device
  // reported activity, so this needs no touch plumbing and cannot disagree with
  // what the indev saw. Guarded on not-already-home, because setting the
  // current tile still invalidates it, and firing that every frame once idle
  // would be a permanent redraw for no change.
  // WHICH DEADLINE APPLIES DEPENDS ON WHAT IS ON TOP. The WiFi and calibration
  // pages are the only two that ask somebody to type a long string one tap at a
  // time; everything else is read or pressed. See IDLE_ENTRY_MS.
  const bool typing = (depth_ > 0) && (stack_[depth_ - 1] == detailWifi_ ||
                                       stack_[depth_ - 1] == detailCalib_ ||
                                       stack_[depth_ - 1] == detailVessel_ ||
                                       stack_[depth_ - 1] == detailBufCal_);
  if (lv_display_get_inactive_time(NULL) > (typing ? IDLE_ENTRY_MS : IDLE_HOME_MS)) {
    // Compared against the SAME tile pagesGoHome() will move to. Fixing only
    // the action would leave this guard permanently false once parked on the
    // knob page, and lv_tileview_set_tile would then run every frame for the
    // whole idle period -- the exact permanent redraw the comment above says
    // this guard exists to prevent.
    const bool home =
        (depth_ == 0) && (!tv_ || lv_tileview_get_tile_active(tv_) == defaultTile());
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
  defaultPage_ = s.defaultPage;

  if (active == tileHome_) updateWeight(s);
  else weightPageHidden();
  // TRIAL: cheap enough to run unconditionally -- every write inside is guarded
  // against its previous value, so an invisible page costs comparisons. Keeping
  // it current also means the page is right on the frame it becomes visible
  // rather than the one after.
  updateKnob(s);
}

}  // namespace ui
