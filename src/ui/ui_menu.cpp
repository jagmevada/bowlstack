#include "ui_menu.h"

#include <string.h>

namespace ui {
namespace {

const uint32_t C_BG = 0x000000;
const uint32_t C_PANEL = 0x161B22;
const uint32_t C_BORDER = 0x30363D;
const uint32_t C_TEXT = 0xE6EDF3;
const uint32_t C_MUTED = 0x8B949E;
const uint32_t C_KEY = 0x21262D;

// The row list lives in a child container so menuAddRow and menuSetHint can
// index rows without counting past the header. Stored on the menu object rather
// than in a static, so several menus can exist at once -- which they do: the
// top-level page and the Settings sub-menu are both live.
lv_obj_t *rowsOf(lv_obj_t *menu) {
  return lv_obj_get_child(menu, lv_obj_get_child_count(menu) - 1);
}

void onRowClicked(lv_event_t *e) {
  void (*cb)(void) = (void (*)(void))lv_event_get_user_data(e);
  if (cb) cb();
}

}  // namespace

lv_obj_t *menuCreate(lv_obj_t *parent, const char *title, void (*onBack)(void)) {
  lv_obj_set_style_bg_color(parent, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(parent, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_row(parent, 6, LV_PART_MAIN);
  lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *hdr = lv_obj_create(parent);
  lv_obj_set_size(hdr, LV_PCT(100), 30);
  lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(hdr, 0, LV_PART_MAIN);
  lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lv_obj_t *t = lv_label_create(hdr);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_text(t, title);

  if (onBack) {
    lv_obj_t *b = lv_button_create(hdr);
    lv_obj_set_size(b, 42, 28);
    lv_obj_set_style_radius(b, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_KEY), LV_PART_MAIN);
    lv_obj_add_event_cb(b, onRowClicked, LV_EVENT_CLICKED, (void *)onBack);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, LV_SYMBOL_LEFT);
    lv_obj_center(l);
  } else {
    // The top-level menu is a swipeable PAGE, so it says which way home is
    // rather than offering a button that duplicates the gesture.
    lv_obj_t *l = lv_label_create(hdr);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(C_MUTED), LV_PART_MAIN);
    lv_label_set_text(l, "swipe " LV_SYMBOL_RIGHT);
  }

  lv_obj_t *rows = lv_obj_create(parent);
  lv_obj_set_width(rows, LV_PCT(100));
  lv_obj_set_flex_grow(rows, 1);
  lv_obj_set_style_bg_opa(rows, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(rows, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(rows, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(rows, 6, LV_PART_MAIN);
  lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(rows, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(rows, LV_SCROLLBAR_MODE_AUTO);

  return parent;
}

void menuAddRow(lv_obj_t *menu, const char *label, const char *hint,
                void (*cb)(void)) {
  lv_obj_t *rows = rowsOf(menu);
  lv_obj_t *row = lv_obj_create(rows);
  lv_obj_set_width(row, LV_PCT(100));
  // 44 px, the same target the keyboard keys settled on. A menu row that needs
  // aiming at is worse than no menu.
  lv_obj_set_height(row, 44);
  lv_obj_set_style_bg_color(row, lv_color_hex(C_PANEL), LV_PART_MAIN);
  lv_obj_set_style_border_color(row, lv_color_hex(C_BORDER), LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_left(row, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_right(row, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(row, 0, LV_PART_MAIN);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(row, onRowClicked, LV_EVENT_CLICKED, (void *)cb);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lv_obj_t *l = lv_label_create(row);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_text(l, label);

  lv_obj_t *right = lv_obj_create(row);
  lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(right, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(right, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(right, 6, LV_PART_MAIN);
  lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  // The hint always exists even when empty, so menuSetHint can fill it later
  // without the row's layout shifting the first time a value arrives.
  lv_obj_t *h = lv_label_create(right);
  lv_obj_set_style_text_font(h, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(h, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(h, hint ? hint : "");

  lv_obj_t *chev = lv_label_create(right);
  lv_obj_set_style_text_color(chev, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(chev, LV_SYMBOL_RIGHT);
}

void menuSetHint(lv_obj_t *menu, uint8_t index, const char *hint) {
  if (!menu) return;
  lv_obj_t *rows = rowsOf(menu);
  if (index >= lv_obj_get_child_count(rows)) return;
  lv_obj_t *row = lv_obj_get_child(rows, index);
  lv_obj_t *right = lv_obj_get_child(row, 1);
  lv_obj_t *h = lv_obj_get_child(right, 0);

  // Compared before writing, for the same reason every other update path in
  // this UI compares: lv_label_set_text reallocates and invalidates whether or
  // not the string changed, and this is called once a second.
  if (strcmp(lv_label_get_text(h), hint ? hint : "") == 0) return;
  lv_label_set_text(h, hint ? hint : "");
}

}  // namespace ui
