#include "ui_battery.h"

#include <stdio.h>
#include <string.h>

namespace ui {
namespace {

const uint32_t C_BG = 0x000000;
const uint32_t C_PANEL = 0x161B22;
const uint32_t C_BORDER = 0x30363D;
const uint32_t C_TEXT = 0xE6EDF3;
const uint32_t C_MUTED = 0x8B949E;
const uint32_t C_KEY = 0x21262D;
const uint32_t C_OK = 0x3FB950;
const uint32_t C_WARN = 0xD29922;
const uint32_t C_FAULT = 0xF85149;

lv_obj_t *lblVolts_ = nullptr;
lv_obj_t *lblPct_ = nullptr;
lv_obj_t *chipBand_ = nullptr;
lv_obj_t *lblCharge_ = nullptr;
lv_obj_t *lblPin_ = nullptr;
lv_obj_t *bar_ = nullptr;

void (*onClose_)(void) = nullptr;
void onCloseCb(lv_event_t *) {
  if (onClose_) onClose_();
}

const char *bandName(Battery b) {
  switch (b) {
    case Battery::Good: return "good";
    case Battery::Medium: return "medium";
    case Battery::Low: return "low";
    case Battery::Critical: return "critical";
    default: return "no cell";
  }
}

uint32_t bandColor(Battery b) {
  switch (b) {
    case Battery::Good: return C_OK;
    case Battery::Medium: return C_OK;
    case Battery::Low: return C_WARN;
    case Battery::Critical: return C_FAULT;
    default: return C_BORDER;
  }
}

lv_obj_t *row(lv_obj_t *parent, const char *caption, lv_obj_t **valueOut,
              const lv_font_t *font) {
  lv_obj_t *r = lv_obj_create(parent);
  lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(r, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(r, 0, LV_PART_MAIN);
  lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lv_obj_t *c = lv_label_create(r);
  lv_obj_set_style_text_font(c, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(c, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(c, caption);

  lv_obj_t *v = lv_label_create(r);
  lv_obj_set_style_text_font(v, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(v, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_text(v, "-");
  *valueOut = v;
  return r;
}

}  // namespace

void batteryOnClose(void (*cb)(void)) { onClose_ = cb; }

void buildBatteryPage(lv_obj_t *parent) {
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

  lv_obj_t *title = lv_label_create(hdr);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_text(title, "Battery");

  lv_obj_t *close = lv_button_create(hdr);
  lv_obj_set_size(close, 38, 28);
  lv_obj_set_style_radius(close, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(close, lv_color_hex(C_KEY), LV_PART_MAIN);
  lv_obj_add_event_cb(close, onCloseCb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *cl = lv_label_create(close);
  lv_label_set_text(cl, LV_SYMBOL_CLOSE);
  lv_obj_center(cl);

  // Cell voltage gets the display size, because it is the one number here that
  // is a MEASUREMENT rather than an inference -- everything else on the page is
  // derived from it.
  lblVolts_ = lv_label_create(parent);
  lv_obj_set_style_text_font(lblVolts_, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblVolts_, lv_color_hex(C_TEXT), LV_PART_MAIN);
  lv_label_set_text(lblVolts_, "-.--  V");

  bar_ = lv_bar_create(parent);
  lv_obj_set_size(bar_, LV_PCT(100), 14);
  lv_bar_set_range(bar_, 0, 100);
  lv_bar_set_value(bar_, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar_, lv_color_hex(C_PANEL), LV_PART_MAIN);
  lv_obj_set_style_border_color(bar_, lv_color_hex(C_BORDER), LV_PART_MAIN);
  lv_obj_set_style_border_width(bar_, 1, LV_PART_MAIN);

  lv_obj_t *panel = lv_obj_create(parent);
  lv_obj_set_width(panel, LV_PCT(100));
  lv_obj_set_flex_grow(panel, 1);
  lv_obj_set_style_bg_color(panel, lv_color_hex(C_PANEL), LV_PART_MAIN);
  lv_obj_set_style_border_color(panel, lv_color_hex(C_BORDER), LV_PART_MAIN);
  lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(panel, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_all(panel, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(panel, 8, LV_PART_MAIN);
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);

  row(panel, "charge", &lblPct_, &lv_font_montserrat_20);

  lv_obj_t *bandRow = lv_obj_create(panel);
  lv_obj_set_size(bandRow, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(bandRow, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(bandRow, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(bandRow, 0, LV_PART_MAIN);
  lv_obj_remove_flag(bandRow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(bandRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bandRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_t *bandCap = lv_label_create(bandRow);
  lv_obj_set_style_text_font(bandCap, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(bandCap, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_label_set_text(bandCap, "band");
  chipBand_ = lv_label_create(bandRow);
  lv_obj_set_style_text_font(chipBand_, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(chipBand_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(chipBand_, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_all(chipBand_, 4, LV_PART_MAIN);
  lv_obj_set_style_text_color(chipBand_, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_label_set_text(chipBand_, "-");

  row(panel, "charging", &lblCharge_, &lv_font_montserrat_18);

  // The ADC pin voltage, at the information size. It is the calibration
  // reference -- BOWLSTACK_BATTERY_CAL is cell divided by this -- and it is
  // also the tell for a wiring fault, since an implausible figure here is
  // visible where the derived cell voltage would look merely wrong.
  lblPin_ = lv_label_create(panel);
  lv_obj_set_style_text_font(lblPin_, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblPin_, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_obj_set_width(lblPin_, LV_PCT(100));
  lv_label_set_long_mode(lblPin_, LV_LABEL_LONG_WRAP);
  lv_label_set_text(lblPin_, "pin -- mV");
}

// GUARD ON WHAT IS DRAWN, NOT ON WHAT IS MEASURED. Same discipline as the
// weight page, and for the same reason -- see the change-detection note in
// ui_weight.cpp. LVGL's setters do not compare before acting: every one of them
// reallocates a label buffer or marks an object dirty whether or not anything
// changed.
void setIfChanged(lv_obj_t *o, char *prev, uint32_t len, const char *txt) {
  if (strncmp(prev, txt, len - 1) == 0) return;
  snprintf(prev, len, "%s", txt);
  lv_label_set_text(o, txt);
}

void colorIfChanged(lv_obj_t *o, uint32_t *prev, bool *have, uint32_t rgb,
                    lv_style_selector_t part) {
  if (*have && *prev == rgb) return;
  *have = true;
  *prev = rgb;
  lv_obj_set_style_text_color(o, lv_color_hex(rgb), part);
}

void bgIfChanged(lv_obj_t *o, uint32_t *prev, bool *have, uint32_t rgb,
                 lv_style_selector_t part) {
  if (*have && *prev == rgb) return;
  *have = true;
  *prev = rgb;
  lv_obj_set_style_bg_color(o, lv_color_hex(rgb), part);
}

char prevVolts_[16] = {0};
char prevPct_[12] = {0};
char prevBand_[16] = {0};
char prevCharge_[16] = {0};
char prevPin_[48] = {0};
uint32_t cVolts_ = 0, cBar_ = 0, cChip_ = 0, cCharge_ = 0;
bool hVolts_ = false, hBar_ = false, hChip_ = false, hCharge_ = false;
int32_t prevBarVal_ = -1;

void updateBatteryPage(const State &s) {
  if (!lblVolts_) return;

  // THE WHOLE-STRUCT EARLY-OUT THAT USED TO SIT HERE NEVER FIRED. It compared
  // raw batteryMv and batteryPinMv, and those are ADC readings -- they wander
  // by a millivolt or three on every single sample. The console shows it
  // plainly: pin 1398, 1397, 1399, 1388. So the guard was false almost every
  // frame, and the page below repainted five labels, a bar and four style
  // colours at whatever rate the loop managed.
  //
  // The comment it carried said "a voltage that moves once a second was
  // repainting the panel" -- which was the right diagnosis and the wrong
  // remedy: it guarded the number rather than the picture. 4194 mV and 4195 mV
  // are a different int and the same four characters on screen.
  //
  // Reported from the field as the LED fade stalling badly whenever
  // Settings > Battery is open, which is exactly what a full-page invalidate
  // every frame does to the one task that also drives the fade.
  //
  // So each widget now guards itself on its own FORMATTED text, and the styles
  // guard on their own colour. The pin line is the only thing here that really
  // does change every reading -- it is a raw diagnostic and is meant to -- and
  // it is now one small label being rewritten instead of the whole page.

  char buf[48];
  if (s.battery == Battery::Unknown || s.batteryMv == 0) {
    setIfChanged(lblVolts_, prevVolts_, sizeof(prevVolts_), "no cell");
    colorIfChanged(lblVolts_, &cVolts_, &hVolts_, C_FAULT, LV_PART_MAIN);
  } else {
    snprintf(buf, sizeof(buf), "%u.%02u V", s.batteryMv / 1000,
             (s.batteryMv % 1000) / 10);
    setIfChanged(lblVolts_, prevVolts_, sizeof(prevVolts_), buf);
    colorIfChanged(lblVolts_, &cVolts_, &hVolts_, C_TEXT, LV_PART_MAIN);
  }

  // A percentage only where one was actually derived. -1 means the curve was
  // never evaluated, and "0%" would be a claim that the cell is empty rather
  // than that nothing is known about it.
  const int32_t barVal = s.batteryPercent < 0 ? 0 : s.batteryPercent;
  if (s.batteryPercent < 0) {
    setIfChanged(lblPct_, prevPct_, sizeof(prevPct_), "unknown");
  } else {
    snprintf(buf, sizeof(buf), "%d %%", s.batteryPercent);
    setIfChanged(lblPct_, prevPct_, sizeof(prevPct_), buf);
  }
  // lv_bar_set_value animates and invalidates even when handed the value it
  // already holds, so it needs a guard of its own.
  if (barVal != prevBarVal_) {
    prevBarVal_ = barVal;
    lv_bar_set_value(bar_, barVal, LV_ANIM_OFF);
  }
  bgIfChanged(bar_, &cBar_, &hBar_, bandColor(s.battery), LV_PART_INDICATOR);

  setIfChanged(chipBand_, prevBand_, sizeof(prevBand_), bandName(s.battery));
  bgIfChanged(chipBand_, &cChip_, &hChip_, bandColor(s.battery), LV_PART_MAIN);

  // FOUR STATES, AND THE POINT IS THAT TWO DIFFERENT THINGS ARE KNOWN TO
  // DIFFERENT DEGREES.
  //
  // Whether current is going into the cell comes from the ETA6098's STAT pin,
  // which reaches no GPIO on an unmodified board -- so it is genuinely unknown
  // and "no" would be a claim the hardware cannot support.
  //
  // Whether the unit is on MAINS is a different question and the board can
  // answer it on every build, through the VBUS divider on IO10. This row used
  // to print a flat "unknown" and stop there, which threw away a fact the
  // device had -- and left somebody looking at a plugged-in station being told
  // nothing at all about its power.
  //
  // So: say what is known, and name which question it answers.
  const char *chargeTxt;
  uint32_t chargeCol;
  if (s.chargingKnown) {
    if (s.charging) {
      chargeTxt = "yes";
      chargeCol = C_OK;
    } else {
      // Not charging AND on mains is the terminated case -- the cell is full
      // and the charger has stopped. Worth distinguishing from running down.
      chargeTxt = s.externalPower ? "no - charged" : "no";
      chargeCol = C_TEXT;
    }
  } else if (s.externalPower) {
    // Deliberately not "yes". The unit is plugged in, which is all this board
    // can see without the STAT mod; whether the charger is still pushing
    // current is exactly the thing it cannot tell.
    chargeTxt = "on mains";
    chargeCol = C_OK;
  } else {
    chargeTxt = "on battery";
    chargeCol = C_TEXT;
  }
  setIfChanged(lblCharge_, prevCharge_, sizeof(prevCharge_), chargeTxt);
  colorIfChanged(lblCharge_, &cCharge_, &hCharge_, chargeCol, LV_PART_MAIN);

  snprintf(buf, sizeof(buf), "pin %u mV  x3.0 divider on GPIO5", s.batteryPinMv);
  setIfChanged(lblPin_, prevPin_, sizeof(prevPin_), buf);
}

}  // namespace ui
