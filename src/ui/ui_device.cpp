#include "ui_device.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace ui {
namespace {

const uint32_t C_BG = 0x000000;
const uint32_t C_TEXT = 0xE6EDF3;
const uint32_t C_MUTED = 0x8B949E;
const uint32_t C_KEY = 0x21262D;

lv_obj_t *lblBlock_ = nullptr;
void (*onClose_)(void) = nullptr;

char prevBlock_[512] = {0};
uint32_t nextUpdateMs_ = 0;

// 4 Hz. The figures below move at the publish rate of 20 Hz and nobody reads
// raw converter counts faster than this -- a slower rewrite is a steadier page
// to read as well as a cheaper one to draw.
const uint32_t UPDATE_MS = 250;

void closeClicked(lv_event_t *) {
  if (onClose_) onClose_();
}

}  // namespace

void deviceOnClose(void (*cb)(void)) { onClose_ = cb; }

void buildDevicePage(lv_obj_t *parent) {
  lv_obj_set_style_bg_color(parent, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(parent, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_row(parent, 4, LV_PART_MAIN);
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
  lv_label_set_text(t, "Diagnose");

  lv_obj_t *b = lv_button_create(hdr);
  lv_obj_set_size(b, 42, 28);
  lv_obj_set_style_radius(b, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_KEY), LV_PART_MAIN);
  lv_obj_add_event_cb(b, closeClicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *bl = lv_label_create(b);
  lv_label_set_text(bl, LV_SYMBOL_LEFT);
  lv_obj_center(bl);

  // 14 px, the "read deliberately, up close" tier of the type scale in
  // lv_conf.h. This page is read by someone standing at the device with a
  // reason to be there, not glanced at across a kitchen, so it is the one place
  // the small size is the right size rather than a compromise.
  lblBlock_ = lv_label_create(parent);
  lv_obj_set_style_text_font(lblBlock_, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lblBlock_, lv_color_hex(C_MUTED), LV_PART_MAIN);
  lv_obj_set_width(lblBlock_, LV_PCT(100));
  lv_obj_set_flex_grow(lblBlock_, 1);
  lv_label_set_long_mode(lblBlock_, LV_LABEL_LONG_CLIP);
  lv_label_set_text(lblBlock_, "waiting for a sample");
}

void updateDevicePage(const State &s) {
  if (!lblBlock_) return;

  const uint32_t now = lv_tick_get();
  if ((int32_t)(now - nextUpdateMs_) < 0) return;
  nextUpdateMs_ = now + UPDATE_MS;

  const ScaleView &sc = s.scale;
  char buf[512];
  int n = 0;

  n += snprintf(buf + n, sizeof(buf) - n, "%s   fw %s\n", s.deviceId ? s.deviceId : "BWL-000",
                s.firmware ? s.firmware : "0.0.0");
  n += snprintf(buf + n, sizeof(buf) - n, "up %lu h %lu m\n\n",
                (unsigned long)(s.uptimeSec / 3600UL),
                (unsigned long)((s.uptimeSec / 60UL) % 60UL));

  static const char *NAME[CELLS] = {"A", "B", "C"};
  for (uint8_t i = 0; i < CELLS; i++) {
    const CellView &c = sc.cell[i];
    if (c.state != Cell::Online) {
      // No figures at all for a cell that is not converting. Printing its last
      // known counts here would be the same lie the dashboard refuses -- a
      // stale number that looks exactly like a live one.
      n += snprintf(buf + n, sizeof(buf) - n, "cell %s   %s\n\n", NAME[i],
                    c.state == Cell::Warming ? "warming" : "OFFLINE");
      continue;
    }
    // THE CELL'S MASS FIRST, because it is the figure that moved here off the
    // dashboard and the one most people open this page for. An uneven split is
    // how you see a bowl placed off-centre, a mount fouling, or one cell doing
    // all the work -- none of which the total can show you, being a sum.
    if (sc.calibrated) {
      const long mg = (long)lroundf(c.grams);
      const long a = mg < 0 ? -mg : mg;
      n += snprintf(buf + n, sizeof(buf) - n, "cell %s   %s%ld.%03ld kg%s\n", NAME[i],
                    mg < 0 ? "-" : "", a / 1000L, a % 1000L, c.overRange ? "  OVER" : "");
    } else {
      n += snprintf(buf + n, sizeof(buf) - n, "cell %s   %ld cts%s\n", NAME[i],
                    (long)c.counts, c.overRange ? "  OVER" : "");
    }
    n += snprintf(buf + n, sizeof(buf) - n, "         raw %8ld\n", (long)c.rawCounts);
    // net = filtered minus tare, which is the figure the kilograms are computed
    // from. Printed beside the raw one so the effect of the filter and the
    // effect of the tare can be told apart at a glance.
    n += snprintf(buf + n, sizeof(buf) - n, "         net %8ld   p-p %5ld\n", (long)c.counts,
                  (long)c.pp);
    // BOTH ZEROS, labelled, because they are set by different people at
    // different times and only one survives a power cycle. A platform zero of 0
    // means this device was never commissioned; a tare of 0 means nothing is
    // sitting on top of the platform right now.
    n += snprintf(buf + n, sizeof(buf) - n, "        zero %8ld   %u/s\n",
                  (long)c.platformZero, c.sps);
    n += snprintf(buf + n, sizeof(buf) - n, "        tare %8ld\n\n", (long)c.tare);
  }

  if (sc.calibrated) {
    n += snprintf(buf + n, sizeof(buf) - n, "%.3f counts/g   avg %u\n", sc.countsPerGram,
                  sc.window);
    // The ADC ceiling in the unit a person thinks in, computed from the LIVE
    // factor rather than a constant, so it stays true after a recalibration.
    n += snprintf(buf + n, sizeof(buf) - n, "full scale ~%.1f kg / cell",
                  8000000.0f / sc.countsPerGram / 1000.0f);
  } else {
    n += snprintf(buf + n, sizeof(buf) - n, "UNCALIBRATED   avg %u\n", sc.window);
    n += snprintf(buf + n, sizeof(buf) - n, "Settings > Scale > Calibrate");
  }

  if (strcmp(prevBlock_, buf) == 0) return;
  snprintf(prevBlock_, sizeof(prevBlock_), "%s", buf);
  lv_label_set_text(lblBlock_, buf);
}

}  // namespace ui
