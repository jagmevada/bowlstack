#include "ui_pages.h"

#include <lvgl.h>

#include "ui_demo.h"
#include "ui_scope.h"
#include "ui_screens.h"

namespace ui {
namespace {

const uint32_t C_BG = 0x000000;

}  // namespace

void buildPages() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_clean(scr);
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

  lv_obj_t *tv = lv_tileview_create(scr);
  lv_obj_set_size(tv, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(tv, lv_color_hex(C_BG), LV_PART_MAIN);

  lv_obj_t *stock = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
  lv_obj_set_style_pad_all(stock, 0, LV_PART_MAIN);
  build(stock);

  lv_obj_t *scope = lv_tileview_add_tile(tv, 1, 0, LV_DIR_LEFT);
  buildScope(scope);
}

void pagesTick(uint32_t nowMs) {
  // Both pages are driven unconditionally rather than only the visible one.
  //
  // That is the honest choice for a frame-rate measurement: the shipping
  // firmware will have sensors producing samples and telemetry consuming them
  // whether or not the scope happens to be on screen, so stopping the other
  // page's work while measuring would report a number the real device never
  // sees. The cost is real and it belongs in the figure.
  demoTick(nowMs);
  scopeTick(nowMs);
}

}  // namespace ui
