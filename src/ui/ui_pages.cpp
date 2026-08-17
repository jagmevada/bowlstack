#include "ui_pages.h"

#include <lvgl.h>

#include "ui_demo.h"
#include "ui_scope.h"
#include "ui_screens.h"
#include "ui_perf.h"
#include "ui_status.h"
#include "ui_battery.h"
#include "ui_wifi.h"

namespace ui {
namespace {

const uint32_t C_BG = 0x000000;

lv_obj_t *tv_ = nullptr;
lv_obj_t *tileStock_ = nullptr;
lv_obj_t *tileScope_ = nullptr;
lv_obj_t *lastActive_ = nullptr;

// The detail layer: a full-height panel over the PAGE AREA only, so the status
// bar stays visible and the thing you tapped to get here remains on screen.
// Hidden rather than destroyed, because rebuilding a QR code and a keyboard on
// every open would be visibly slow for a screen people bounce in and out of.
lv_obj_t *detailWifi_ = nullptr;
lv_obj_t *detailBatt_ = nullptr;
lv_obj_t *detailOpen_ = nullptr;

void closeDetail() {
  // Back to the network list AND clear the field. The passphrase box is
  // deliberately unmasked (see ui_wifi.cpp), so leaving it populated after an
  // idle timeout would show the key to whoever walks up next.
  wifiResetView();
  if (detailWifi_) lv_obj_add_flag(detailWifi_, LV_OBJ_FLAG_HIDDEN);
  if (detailBatt_) lv_obj_add_flag(detailBatt_, LV_OBJ_FLAG_HIDDEN);
  detailOpen_ = nullptr;
}

void openDetail(lv_obj_t *which) {
  closeDetail();
  if (!which) return;
  lv_obj_remove_flag(which, LV_OBJ_FLAG_HIDDEN);
  detailOpen_ = which;
}

void openWifi() { openDetail(detailWifi_); }
void openBattery() { openDetail(detailBatt_); }

// Every detail panel is the same shape: an overlay pinned below the status bar
// and taken out of the screen's flex flow. Built once here rather than repeated
// per page, because the flag is the non-obvious half -- without it the panel is
// a flex ITEM placed after the tileview and hangs off the bottom.
lv_obj_t *makeDetail(lv_obj_t *scr) {
  lv_obj_t *d = lv_obj_create(scr);
  lv_obj_add_flag(d, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_set_pos(d, 0, STATUS_H);
  lv_obj_set_size(d, SCREEN_W, SCREEN_H - STATUS_H);
  lv_obj_set_style_pad_all(d, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(d, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(d, 0, LV_PART_MAIN);
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

  // The screen is a COLUMN: status bar, then pages. The bar is a sibling of the
  // tileview rather than a child of it, which is the whole reason it survives a
  // swipe -- a tileview scrolls its children, so anything inside would travel
  // with the page.
  lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(scr, 0, LV_PART_MAIN);

  lv_obj_t *status = lv_obj_create(scr);
  buildStatus(status);

  tv_ = lv_tileview_create(scr);
  lv_obj_set_width(tv_, LV_PCT(100));
  lv_obj_set_flex_grow(tv_, 1);
  lv_obj_set_style_bg_color(tv_, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_border_width(tv_, 0, LV_PART_MAIN);

  tileStock_ = lv_tileview_add_tile(tv_, 0, 0, LV_DIR_RIGHT);
  lv_obj_set_style_pad_all(tileStock_, 0, LV_PART_MAIN);
  build(tileStock_);

  tileScope_ = lv_tileview_add_tile(tv_, 1, 0, LV_DIR_LEFT);
  buildScope(tileScope_);

  // Detail overlays, created AFTER the tileview so they stack above it.
  detailWifi_ = makeDetail(scr);
  buildWifiPage(detailWifi_);
  wifiOnClose(closeDetail);

  detailBatt_ = makeDetail(scr);
  buildBatteryPage(detailBatt_);
  batteryOnClose(closeDetail);

  statusOnWifiTap(openWifi);
  statusOnBatteryTap(openBattery);

  lastActive_ = nullptr;
}

// Measurement aid, off in any normal build. With -DUI_AUTO_CYCLE_MS=8000 the
// pages advance on their own, so both can be profiled without a finger on the
// glass -- which is the only way to get an honest reading of a page nobody is
// there to swipe to. It also suppresses the idle timeout, which would otherwise
// drag the measurement back to the stock page every minute.
#ifndef UI_AUTO_CYCLE_MS
#define UI_AUTO_CYCLE_MS 0
#endif

void pagesGoHome() {
  closeDetail();
  if (tv_ && tileStock_) lv_tileview_set_tile(tv_, tileStock_, LV_ANIM_OFF);
}

void pagesTick(uint32_t nowMs) {
  // --- idle timeout --------------------------------------------------------
  // lv_display_get_inactive_time() is LVGL's own count of milliseconds since
  // any input device reported activity, so this needs no touch plumbing of its
  // own and cannot disagree with what the indev actually saw.
  //
  // Guarded on already-being-home, because lv_tileview_set_tile on the current
  // tile still invalidates it, and firing that every frame once idle would be a
  // permanent redraw for no change -- the exact waste this work is about.
#if UI_AUTO_CYCLE_MS
  {
    static uint32_t nextSwap = 0;
    static bool onScope = false;
    if ((int32_t)(nowMs - nextSwap) >= 0) {
      nextSwap = nowMs + UI_AUTO_CYCLE_MS;
      onScope = !onScope;
      closeDetail();
      if (tv_) lv_tileview_set_tile(tv_, onScope ? tileScope_ : tileStock_, LV_ANIM_OFF);
    }
  }
#else
  if (lv_display_get_inactive_time(NULL) > IDLE_HOME_MS) {
    const bool home = (detailOpen_ == nullptr) &&
                      (!tv_ || lv_tileview_get_tile_active(tv_) == tileStock_);
    if (!home) pagesGoHome();
  }
#endif

  // --- data: always, for every page, visible or not ------------------------
  // This is the requirement, and it is why sampling and rendering are separate
  // calls. In the shipping firmware the sensors do not stop ranging because
  // someone swiped, and telemetry does not stop posting; a page that only
  // collected while on screen would show a gap on return and would report a
  // history with holes in it. Both of these touch no LVGL object and invalidate
  // nothing, so they stay cheap no matter how many pages exist.
  demoTick(nowMs);
  scopeSample(nowMs);

  const State &s = demoLatest(nowMs);

  // The status bar is never hidden, so it always renders -- but updateStatus()
  // early-outs on unchanged values, so a steady state costs comparisons rather
  // than a redraw.
  updateStatus(s);

  // A detail page covers the tileview entirely, so nothing underneath is worth
  // rendering. Data collection above still runs -- that is the point of the
  // split -- so the scope's history is intact when the panel closes.
  //
  // The battery page is the exception that proves the rule: it IS visible, so
  // it gets updated. It shows live measurements, and a diagnostic screen frozen
  // at whatever the values were when it opened would be worse than none.
  // Before the early return: the measurement window has to keep closing while
  // an overlay is up, or the console reprints stale fps/busy figures for the
  // whole time someone is poking at the device -- which is precisely when the
  // numbers are being read.
  wifiTick();
  perfTick(nowMs);

  if (detailOpen_) {
    if (detailOpen_ == detailBatt_) updateBatteryPage(s);
    return;
  }

  // --- rendering: only the page a person is actually looking at ------------
  // THIS IS WHAT KEEPS THE FRAME RATE FLAT AS PAGES ARE ADDED. Cost scales with
  // what is on screen, which is one page, rather than with how many exist.
  //
  // Without it, adding a third and fourth page would divide the frame budget
  // among four charts nobody can see -- the exact drop that had to be avoided.
  lv_obj_t *active = lv_tileview_get_tile_active(tv_);
  if (active != lastActive_) {
    lastActive_ = active;
    // Told on transition rather than polled, so the scope can repopulate its
    // chart from the ring in one pass and pick up exactly where the data did.
    scopeSetVisible(active == tileScope_);
  }

  if (active == tileStock_) {
    update(s);
  } else if (active == tileScope_) {
    scopeRender();
    // Once a second, not per frame: the readout is a line of text, and
    // rewriting it at frame rate would be its own measurable cost inside the
    // thing it is measuring.
    scopeShowPerf();
  }
}

}  // namespace ui
