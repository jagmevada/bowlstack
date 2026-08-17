#include "ui_pages.h"

#include <lvgl.h>

#include "ui_battery.h"
#include "ui_demo.h"
#include "ui_menu.h"
#include "ui_perf.h"
#include "ui_scope.h"
#include "ui_screens.h"
#include "ui_status.h"
#include "ui_wifi.h"

namespace ui {
namespace {

const uint32_t C_BG = 0x000000;
const uint32_t C_MUTED = 0x8B949E;

lv_obj_t *tv_ = nullptr;
lv_obj_t *tileMenu_ = nullptr;
lv_obj_t *tileStock_ = nullptr;
lv_obj_t *lastActive_ = nullptr;
lv_obj_t *menuRoot_ = nullptr;

// --- the navigation tree ---------------------------------------------------
//
//   [menu] <-swipe-> [stock]          two pages; stock is home
//     |
//     +-- Settings --+-- WiFi
//     |              +-- Battery
//     +-- Sensors  (the live scope)
//     +-- Device   (empty, deliberately)
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
lv_obj_t *detailSensor_ = nullptr;
lv_obj_t *detailDevice_ = nullptr;

// Depth 3 covers menu > Settings > WiFi, the deepest the tree goes; 4 leaves a
// slot spare. A stack rather than a single "previous" pointer, so Back stays
// unambiguous once a page can be reached from more than one place -- which
// Battery already could be, the moment anything else links to it.
lv_obj_t *stack_[4] = {nullptr, nullptr, nullptr, nullptr};
uint8_t depth_ = 0;

void showOnly(lv_obj_t *which) {
  lv_obj_t *all[] = {detailSettings_, detailWifi_, detailBatt_, detailSensor_,
                     detailDevice_};
  for (uint8_t i = 0; i < 5; i++) {
    if (!all[i]) continue;
    if (all[i] == which) lv_obj_remove_flag(all[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(all[i], LV_OBJ_FLAG_HIDDEN);
  }
  // The scope repaints its whole trace from the ring on entry, so telling it on
  // TRANSITION rather than polling is what keeps that a once-per-visit cost.
  scopeSetVisible(which == detailSensor_);
}

void closeAll() {
  wifiResetView();
  depth_ = 0;
  showOnly(nullptr);
}

void push(lv_obj_t *page) {
  if (!page) return;
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
  menuAddRow(menuRoot_, "Device", nullptr, openDevice);

  tileStock_ = lv_tileview_add_tile(tv_, 1, 0, LV_DIR_LEFT);
  lv_obj_set_style_pad_all(tileStock_, 0, LV_PART_MAIN);
  build(tileStock_);

  // Overlays, created AFTER the tileview so they stack above it.
  detailSettings_ = makeDetail(scr);
  lv_obj_t *settings = menuCreate(detailSettings_, "Settings", back);
  menuAddRow(settings, "WiFi", nullptr, openWifi);
  menuAddRow(settings, "Battery", nullptr, openBattery);

  detailWifi_ = makeDetail(scr);
  buildWifiPage(detailWifi_);
  wifiOnClose(back);

  detailBatt_ = makeDetail(scr);
  buildBatteryPage(detailBatt_);
  batteryOnClose(back);

  detailSensor_ = makeDetail(scr);
  buildScope(detailSensor_);
  scopeOnClose(back);

  detailDevice_ = makeDetail(scr);
  lv_obj_t *device = menuCreate(detailDevice_, "Device", back);
  lv_obj_t *soon = lv_label_create(device);
  lv_obj_set_style_text_font(soon, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(soon, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_obj_set_width(soon, LV_PCT(100));
  lv_label_set_long_mode(soon, LV_LABEL_LONG_WRAP);
  lv_label_set_text(soon,
                    "Nothing here yet. Device id, firmware, uptime, MAC and the "
                    "installation's area and slot belong on this page.");

  // Start on the stock view -- what someone walking up to the station wants to
  // see. The menu is one swipe away.
  lv_tileview_set_tile(tv_, tileStock_, LV_ANIM_OFF);
  lastActive_ = nullptr;
  depth_ = 0;
}

void pagesGoHome() {
  closeAll();
  if (tv_ && tileStock_) lv_tileview_set_tile(tv_, tileStock_, LV_ANIM_OFF);
}

void pagesBack() { back(); }

void pagesTick(uint32_t nowMs) {
  // --- data: always, for every page, visible or not ------------------------
  // Neither touches an LVGL object nor invalidates anything, so both stay cheap
  // however many pages exist. In the shipping firmware the sensors do not stop
  // ranging because someone swiped, and a page that only collected while
  // visible would show a gap on return.
  demoTick(nowMs);
  scopeSample(nowMs);

  const State &s = demoLatest(nowMs);

  // Never hidden, so it always renders -- updateStatus() early-outs on
  // unchanged values, so a steady state costs comparisons rather than a redraw.
  updateStatus(s);

  // A hint on the Sensors row, so the menu answers something at a glance
  // instead of being a list of nouns you must open to learn anything from.
  if (menuRoot_) menuSetHint(menuRoot_, 1, s.sensorsOnline ? "" : "offline");

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
      else if (tv_) lv_tileview_set_tile(tv_, tileStock_, LV_ANIM_OFF);
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
        (depth_ == 0) && (!tv_ || lv_tileview_get_tile_active(tv_) == tileStock_);
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
    } else if (top == detailSensor_) {
      scopeRender();
      if (perfFresh) scopeShowPerf();
    }
    return;
  }

  lv_obj_t *active = lv_tileview_get_tile_active(tv_);
  if (active != lastActive_) lastActive_ = active;
  if (active == tileStock_) update(s);
}

}  // namespace ui
