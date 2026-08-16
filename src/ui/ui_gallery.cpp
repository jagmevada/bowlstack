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

// --- pixel-shift test page state -------------------------------------------
lv_obj_t *shiftBox = nullptr;
lv_obj_t *shiftLabel = nullptr;
lv_obj_t *shiftSwitch = nullptr;
uint8_t shiftStep = 0;

// Same ring the shipping shift uses, kept in step deliberately: a test page
// that exercised a different pattern from the real one would be measuring
// something nobody ships.
const int8_t RING_X[] = {0, 1, 2, 2, 2, 1, 0, 0};
const int8_t RING_Y[] = {0, 0, 0, 1, 2, 2, 2, 1};

void shiftTimerCb(lv_timer_t *) {
  if (!shiftBox) return;
  const bool on = shiftSwitch && lv_obj_has_state(shiftSwitch, LV_STATE_CHECKED);

  const int8_t dx = on ? RING_X[shiftStep] : 0;
  const int8_t dy = on ? RING_Y[shiftStep] : 0;

  // translate_x/y, not set_pos: the offset is applied at draw time, so nothing
  // relayouts and the hairlines keep their exact 1 px geometry. That is the
  // property under test -- a relayout could round differently at each offset
  // and would soften edges for reasons unrelated to the shift itself.
  lv_obj_set_style_translate_x(shiftBox, dx, LV_PART_MAIN);
  lv_obj_set_style_translate_y(shiftBox, dy, LV_PART_MAIN);

  if (shiftLabel) lv_label_set_text_fmt(shiftLabel, "%s  %d,%d", on ? "on" : "off", dx, dy);
  if (on) shiftStep = (uint8_t)((shiftStep + 1) % 8);
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
    pageHeader(t, "Type  1/6", "body sizes - swipe left for more");
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
    pageHeader(t, "Type  2/6", "display sizes - the bowl count");
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
    pageHeader(t, "Type  3/6", "unscii is 1bpp - the real 'dots' look");

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
    pageHeader(t, "Widgets  4/6", "touch targets at this DPI");

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
    lv_obj_t *t = lv_tileview_add_tile(tv, 4, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
    lv_obj_set_style_pad_all(t, 0, LV_PART_MAIN);
    build(t);
  }

  // --- page 6: pixel-shift test -------------------------------------------
  // Runs the shift at 1 Hz instead of the shipping 60 s, because the question
  // is whether whole-pixel movement SOFTENS anything, and that cannot be
  // answered by a change that happens once a minute.
  //
  // The content is chosen to be the worst case for both failure modes at once:
  //   - a 48 px glyph, which is what actually retained on this panel
  //   - 1 px hairlines, which are the first thing any resampling destroys
  //   - a hard-edged box, where softening shows as a grey fringe
  // If edges soften, the hairlines go grey or disappear at some offsets. If
  // they stay crisp at every offset, the shift is doing what it should.
  {
    lv_obj_t *t = makeTile(tv, 5, 0, LV_DIR_LEFT);
    pageHeader(t, "Pixel shift  6/6", "1 Hz here, 60 s in the real UI");

    lv_obj_t *ctl = lv_obj_create(t);
    lv_obj_set_size(ctl, LV_PCT(100), 36);
    lv_obj_set_style_bg_opa(ctl, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctl, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ctl, 0, LV_PART_MAIN);
    lv_obj_remove_flag(ctl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ctl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctl, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    shiftLabel = lv_label_create(ctl);
    lv_obj_set_style_text_font(shiftLabel, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(shiftLabel, lv_color_hex(C_MUTED), LV_PART_MAIN);
    lv_label_set_text(shiftLabel, "off  0,0");

    shiftSwitch = lv_switch_create(ctl);
    lv_obj_add_state(shiftSwitch, LV_STATE_CHECKED);

    // The shiftable group. Everything that must move sits inside it; the tile
    // around it stays still, which is what gives the eye a fixed reference to
    // judge the movement against.
    shiftBox = lv_obj_create(t);
    lv_obj_set_width(shiftBox, LV_PCT(100));
    lv_obj_set_flex_grow(shiftBox, 1);
    lv_obj_set_style_bg_opa(shiftBox, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(shiftBox, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(shiftBox, 4, LV_PART_MAIN);
    lv_obj_remove_flag(shiftBox, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *glyph = lv_label_create(shiftBox);
    lv_obj_set_style_text_font(glyph, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(glyph, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_label_set_text(glyph, "2");
    lv_obj_align(glyph, LV_ALIGN_TOP_LEFT, 0, 0);

    // Hard-edged box: softening would show as a grey fringe on its border.
    lv_obj_t *box = lv_obj_create(shiftBox);
    lv_obj_set_size(box, 56, 56);
    lv_obj_align(box, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_radius(box, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(box, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    // 1 px hairlines, horizontal and vertical. These are the sensitive test:
    // any resampling turns a 1 px white line into two grey ones.
    for (uint8_t i = 0; i < 4; i++) {
      lv_obj_t *hl = lv_obj_create(shiftBox);
      lv_obj_set_size(hl, LV_PCT(90), 1);
      lv_obj_align(hl, LV_ALIGN_TOP_LEFT, 0, 70 + i * 6);
      lv_obj_set_style_bg_color(hl, lv_color_hex(C_TEXT), LV_PART_MAIN);
      lv_obj_set_style_border_width(hl, 0, LV_PART_MAIN);
      lv_obj_set_style_radius(hl, 0, LV_PART_MAIN);
      lv_obj_remove_flag(hl, LV_OBJ_FLAG_SCROLLABLE);
    }
    for (uint8_t i = 0; i < 6; i++) {
      lv_obj_t *vl = lv_obj_create(shiftBox);
      lv_obj_set_size(vl, 1, 34);
      lv_obj_align(vl, LV_ALIGN_TOP_LEFT, i * 7, 100);
      lv_obj_set_style_bg_color(vl, lv_color_hex(C_TEXT), LV_PART_MAIN);
      lv_obj_set_style_border_width(vl, 0, LV_PART_MAIN);
      lv_obj_set_style_radius(vl, 0, LV_PART_MAIN);
      lv_obj_remove_flag(vl, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_t *txt = lv_label_create(shiftBox);
    lv_obj_set_style_text_font(txt, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(txt, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_label_set_text(txt, "f1 present");
    lv_obj_align(txt, LV_ALIGN_TOP_LEFT, 0, 142);

    lv_timer_create(shiftTimerCb, 1000, nullptr);
  }
}

}  // namespace ui
