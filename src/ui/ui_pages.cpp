#include "ui_pages.h"

#include <lvgl.h>

#include "ui_demo.h"
#include "ui_scope.h"
#include "ui_screens.h"
#include "ui_status.h"
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
lv_obj_t *detail_ = nullptr;
bool detailOpen_ = false;

void closeDetail() {
  if (!detail_) return;
  lv_obj_add_flag(detail_, LV_OBJ_FLAG_HIDDEN);
  detailOpen_ = false;
}

void openWifi() {
  if (!detail_) return;
  lv_obj_remove_flag(detail_, LV_OBJ_FLAG_HIDDEN);
  detailOpen_ = true;
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

  // Created AFTER the tileview so it stacks above it, and sized to the page
  // area rather than the screen -- the status bar is deliberately still
  // reachable while a detail page is open.
  detail_ = lv_obj_create(scr);
  lv_obj_set_width(detail_, LV_PCT(100));
  lv_obj_set_height(detail_, LV_PCT(100));
  lv_obj_set_style_pad_all(detail_, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(detail_, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(detail_, 0, LV_PART_MAIN);
  lv_obj_remove_flag(detail_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(detail_, LV_OBJ_FLAG_HIDDEN);
  buildWifiPage(detail_);
  wifiOnClose(closeDetail);

  statusOnWifiTap(openWifi);

  lastActive_ = nullptr;
}

void pagesTick(uint32_t nowMs) {
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
  if (detailOpen_) return;

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
  }
}

}  // namespace ui
