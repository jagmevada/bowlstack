// Font specimen and widget gallery, as swipeable pages.
//
// lv_tileview rather than tabs: a tab bar would eat ~40 px of a 320 px panel to
// show navigation that a swipe already implies, and this screen is temporary
// scaffolding for a decision, not a shipping view.

#include "ui_gallery.h"

#include <lvgl.h>

#include "ui_screens.h"

namespace ui {
namespace {

const uint32_t C_BG = 0x000000;
const uint32_t C_PANEL = 0x161B22;
const uint32_t C_BORDER = 0x30363D;
const uint32_t C_TEXT = 0xE6EDF3;
const uint32_t C_MUTED = 0x8B949E;
const uint32_t C_ACCENT = 0x1F6F43;

struct FontSample {
  const lv_font_t *font;
  const char *name;
};

// Sample string chosen for the decision at hand, not for prettiness. It carries
// the digits the bowl count uses, the lowercase the level labels use, and the
// letter pairs that expose bad hinting at small sizes.
const char *SAMPLE = "f1 present  0123";

void pageHeader(lv_obj_t *parent, const char *title, const char *subtitle) {
  lv_obj_t *h = lv_label_create(parent);
  lv_obj_set_style_text_font(h, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(h, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_text(h, title);

  lv_obj_t *s = lv_label_create(parent);
  lv_obj_set_style_text_font(s, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(s, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(s, subtitle);
}

lv_obj_t *makeTile(lv_obj_t *tv, uint8_t col, uint8_t row, lv_dir_t dir) {
  lv_obj_t *t = lv_tileview_add_tile(tv, col, row, dir);
  lv_obj_set_style_bg_color(t, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(t, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(t, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(t, 4, LV_PART_MAIN);
  lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
  return t;
}

void addFontRows(lv_obj_t *tile, const FontSample *rows, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    lv_obj_t *row = lv_obj_create(tile);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 1, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *tag = lv_label_create(row);
    lv_obj_set_style_text_font(tag, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(tag, lv_color_hex(C_ACCENT), LV_PART_MAIN);
    lv_label_set_text(tag, rows[i].name);

    lv_obj_t *smp = lv_label_create(row);
    lv_obj_set_style_text_font(smp, rows[i].font, LV_PART_MAIN);
    lv_obj_set_style_text_color(smp, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_label_set_text(smp, SAMPLE);
  }
}

}  // namespace

void buildGallery() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_clean(scr);
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

  lv_obj_t *tv = lv_tileview_create(scr);
  lv_obj_set_size(tv, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(tv, lv_color_hex(C_BG), LV_PART_MAIN);

  // --- page 1: small sizes -------------------------------------------------
  {
    lv_obj_t *t = makeTile(tv, 0, 0, LV_DIR_RIGHT);
    pageHeader(t, "Type  1/5", "body sizes - swipe left for more");
    static const FontSample rows[] = {
        {&lv_font_montserrat_12, "Montserrat 12"}, {&lv_font_montserrat_14, "Montserrat 14"},
        {&lv_font_montserrat_16, "Montserrat 16"}, {&lv_font_montserrat_18, "Montserrat 18"},
        {&lv_font_montserrat_20, "Montserrat 20"},
    };
    addFontRows(t, rows, sizeof(rows) / sizeof(rows[0]));
  }

  // --- page 2: display sizes ----------------------------------------------
  {
    lv_obj_t *t = makeTile(tv, 1, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
    pageHeader(t, "Type  2/5", "display sizes - the bowl count");
    static const FontSample rows[] = {
        {&lv_font_montserrat_22, "Montserrat 22"}, {&lv_font_montserrat_24, "Montserrat 24"},
        {&lv_font_montserrat_28, "Montserrat 28"}, {&lv_font_montserrat_32, "Montserrat 32"},
    };
    addFontRows(t, rows, sizeof(rows) / sizeof(rows[0]));

    lv_obj_t *big = lv_label_create(t);
    lv_obj_set_style_text_font(big, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(big, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_label_set_text(big, "0 1 2 3 4");
  }

  // --- page 3: the antialiasing control -----------------------------------
  {
    lv_obj_t *t = makeTile(tv, 2, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
    pageHeader(t, "Type  3/5", "unscii is 1bpp - the real 'dots' look");

    // The comparison that answers the question directly. unscii has NO
    // antialiasing whatsoever; Montserrat has 16 coverage levels per pixel. If
    // the two look alike on this panel, the complaint is resolution and the
    // answer is a bigger size. If they look plainly different, Montserrat is
    // already smoothing and the answer is a different typeface.
    static const FontSample rows[] = {
        {&lv_font_unscii_8, "unscii 8  (1bpp, no AA)"},
        {&lv_font_unscii_16, "unscii 16 (1bpp, no AA)"},
        {&lv_font_montserrat_16, "Montserrat 16 (4bpp AA)"},
        {&lv_font_montserrat_24, "Montserrat 24 (4bpp AA)"},
    };
    addFontRows(t, rows, sizeof(rows) / sizeof(rows[0]));

    lv_obj_t *note = lv_label_create(t);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(note, lv_color_hex(C_MUTED), LV_PART_MAIN);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_label_set_text(note,
                      "LVGL ships only Montserrat for Latin. Any other face "
                      "must be converted from a TTF.");
  }

  // --- page 4: widgets -----------------------------------------------------
  {
    lv_obj_t *t = makeTile(tv, 3, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
    pageHeader(t, "Widgets  4/5", "touch targets at this DPI");

    lv_obj_t *btn = lv_button_create(t);
    lv_obj_set_size(btn, LV_PCT(100), 40);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Button  40 px tall");
    lv_obj_center(bl);

    lv_obj_t *sw_row = lv_obj_create(t);
    lv_obj_set_size(sw_row, LV_PCT(100), 40);
    lv_obj_set_style_bg_opa(sw_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(sw_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sw_row, 0, LV_PART_MAIN);
    lv_obj_remove_flag(sw_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(sw_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sw_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t *swl = lv_label_create(sw_row);
    lv_label_set_text(swl, "Switch");
    lv_switch_create(sw_row);

    lv_obj_t *slider = lv_slider_create(t);
    lv_obj_set_width(slider, LV_PCT(100));
    lv_slider_set_value(slider, 65, LV_ANIM_OFF);

    lv_obj_t *bar = lv_bar_create(t);
    lv_obj_set_size(bar, LV_PCT(100), 14);
    lv_bar_set_value(bar, 40, LV_ANIM_OFF);

    lv_obj_t *cb = lv_checkbox_create(t);
    lv_checkbox_set_text(cb, "Checkbox");

    lv_obj_t *dd = lv_dropdown_create(t);
    lv_obj_set_width(dd, LV_PCT(100));
    lv_dropdown_set_options(dd, "Area D\nArea T\nArea M");

    lv_obj_t *chip = lv_label_create(t);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(chip, lv_color_hex(C_PANEL), LV_PART_MAIN);
    lv_obj_set_style_border_width(chip, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(chip, lv_color_hex(C_BORDER), LV_PART_MAIN);
    lv_obj_set_style_radius(chip, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chip, 4, LV_PART_MAIN);
    lv_label_set_text(chip, "chip / badge");
  }

  // --- page 5: the real stock view ----------------------------------------
  // The shipping screen itself, built into a tile by the same build() the
  // device uses. Not a mock-up of it -- the same function, the same widgets,
  // the same fonts. ui_demo then cycles it through the states worth seeing.
  {
    lv_obj_t *t = lv_tileview_add_tile(tv, 4, 0, LV_DIR_LEFT);
    lv_obj_set_style_pad_all(t, 0, LV_PART_MAIN);
    build(t);
  }
}

}  // namespace ui
