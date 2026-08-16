// ---------------------------------------------------------------------------
// Waveshare ESP32-S3-Touch-LCD-2 bring-up harness.
//
// THIS IS NOT BOWLSTACK. It links no sensor, no WiFi and no telemetry, and it
// is not part of any shipping image -- build_src_filter keeps it out of every
// other environment. Its job is to answer, in order, the questions that make
// integration debuggable:
//
//   1. is the touch/IMU I2C bus alive?          (expect CST816D + QMI8658)
//   2. is the camera SCCB bus usable as our     (expect a silent but
//      second I2C bus?                           acknowledging bus)
//   3. does the panel light up, in the right    (raw LovyanGFX, no LVGL)
//      orientation and polarity?
//   4. does LVGL flush to it correctly?
//   5. does touch reach an LVGL widget?
//   6. does the on-board battery divider read
//      a plausible cell voltage on GPIO5?
//
// Each is separated deliberately. Bringing a display and a GUI toolkit up in
// one step means a blank screen has a dozen possible causes; bringing the panel
// up with raw LovyanGFX FIRST means that by the time LVGL is involved, "the
// screen works" is already established fact.
//
// From stage 4 on it shows src/ui/ -- the same gallery and the same stock view,
// driven by the same ui_demo scenarios, as `pio run -e sim`. Not a lookalike:
// the same source files. That is what makes the desktop preview evidence about
// this device rather than a picture that resembles it.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#include <esp_mac.h>

#include "battery_soc.h"
#include "board_waveshare_s3.h"
#include "lgfx_waveshare_s3.h"
#include "logo128.h"
#include "version.h"
#include "ui_demo.h"
#include "ui_pages.h"
#include "ui_perf.h"
#include "ui_scope.h"
#include "ui_screens.h"
#include "ui_wifi.h"

namespace {

LGFX_WaveshareS3Touch2 gfx;

// Bytes per pixel is stated explicitly and NOT taken from sizeof(lv_color_t).
//
// In LVGL 9 `lv_color_t` is a 3-byte {blue, green, red} struct REGARDLESS of
// LV_COLOR_DEPTH -- it is the API's colour type, not the draw buffer's pixel
// format. At LV_COLOR_DEPTH 16 the buffer holds 2-byte pixels, which is also
// why the flush callback casts to lgfx::rgb565_t.
//
// 48 lines, a little under a sixth of the panel. Briefly raised to 80 to chase
// a flicker that turned out to be capacitive coupling from a hand on the panel
// edge -- a handling matter, not a rendering one -- so the extra 30 KB bought
// nothing and is given back. It will be wanted later: WiFi and TLS have yet to
// move into this image, and telemetry alone took ~5 KB of stack the first time
// it ran on the discrete board.
const uint32_t LV_BUF_BPP = 2;  // LV_COLOR_DEPTH 16
const uint32_t LV_BUF_LINES = 48;
const uint32_t LV_BUF_PX = board::LCD_W * LV_BUF_LINES;
const uint32_t LV_BUF_BYTES = LV_BUF_PX * LV_BUF_BPP;

uint8_t *buf1 = nullptr;
uint8_t *buf2 = nullptr;

// --- battery ---------------------------------------------------------------
// Same 16-sample mean the discrete build uses. A single ESP32 conversion
// carries tens of millivolts of noise, and the steep end of a Li-ion discharge
// curve turns that into several percent of apparent charge -- enough to flap a
// band on sampling noise alone.
uint16_t readBatteryPinMv() {
  uint32_t acc = 0;
  for (uint8_t i = 0; i < 16; i++) acc += analogReadMilliVolts(board::PIN_BATTERY_ADC);
  return (uint16_t)(acc / 16);
}

// --- I2C probing -----------------------------------------------------------
// Reports what acknowledges, and just as importantly separates "nothing is
// there" from "the bus is stuck". A pair of lines held low by a wiring fault
// looks identical to an empty bus through a scan alone, so the idle levels are
// read directly first.
void scanBus(TwoWire &bus, const char *name, int sda, int scl, uint32_t hz) {
  Serial.printf("\n  %s  (SDA=%d SCL=%d)\n", name, sda, scl);

  pinMode(sda, INPUT);
  pinMode(scl, INPUT);
  delayMicroseconds(50);
  const int idleSda = digitalRead(sda);
  const int idleScl = digitalRead(scl);
  Serial.printf("    idle levels: SDA=%d SCL=%d  %s\n", idleSda, idleScl,
                (idleSda && idleScl)
                    ? "(pulled up - good)"
                    : "(LOW - no pull-ups, or a device is holding the bus)");

  bus.begin(sda, scl, hz);
  uint8_t found = 0;
  for (uint8_t addr = 0x08; addr < 0x78; addr++) {
    bus.beginTransmission(addr);
    if (bus.endTransmission() == 0) {
      const char *known = "";
      if (addr == 0x15) known = "  <- CST816D touch";
      else if (addr == 0x6A || addr == 0x6B) known = "  <- QMI8658 IMU";
      else if (addr == 0x29) known = "  <- VL53L0X (power-up default)";
      else if (addr >= 0x30 && addr <= 0x33) known = "  <- VL53L0X (Bowlstack-assigned)";
      Serial.printf("    0x%02X ack%s\n", addr, known);
      found++;
    }
  }
  if (found == 0) Serial.println("    (nothing acknowledged)");
}

// --- LVGL bindings ---------------------------------------------------------

void lvglLog(lv_log_level_t, const char *msg) { Serial.printf("lvgl: %s\n", msg); }

void flushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  const int32_t w = area->x2 - area->x1 + 1;
  const int32_t h = area->y2 - area->y1 + 1;

  gfx.startWrite();
  gfx.setAddrWindow(area->x1, area->y1, w, h);
  // Casting to lgfx::rgb565_t rather than a bare uint16_t* is what tells
  // LovyanGFX the source pixel format, so it handles byte order itself. Pushed
  // as raw uint16_t the image renders in convincing but wrong colours -- a bug
  // that looks like a panel configuration problem and is not.
  gfx.writePixels(reinterpret_cast<lgfx::rgb565_t *>(px_map), w * h);
  gfx.endWrite();

  lv_display_flush_ready(disp);
}

// THE ONLY PLACE THE TOUCH CONTROLLER IS READ. Nothing else may call
// gfx.getTouch().
//
// The CST816D hands over a touch event and clears its data register in the same
// transaction, so a second reader does not observe the same event -- it steals
// it, leaving LVGL to infer press/release from a stream with holes in it, which
// presents as a spurious drag.
void touchCb(lv_indev_t *, lv_indev_data_t *data) {
  uint16_t x, y;
  if (gfx.getTouch(&x, &y)) {
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// --- stage 3: the panel, before LVGL exists --------------------------------
// The colour bars are OFF by default. They answered a one-time question --
// is invert right, is rgb_order right, are rows lost to a bad offset -- and it
// is answered on this hardware. A full-screen test pattern in the boot path of
// a device someone watches every day is noise, not diagnostics.
//
// Retained rather than deleted because the question returns the moment the
// panel, the FPC or the LGFX config changes:
//
//     pio run -e ws-s3-bringup -t upload   (with -DBRINGUP_COLOR_BARS=1)
//
// | symptom                        | fix, in lgfx_waveshare_s3.h |
// | black-on-white where inverted  | cfg.invert                  |
// | red and blue swapped           | cfg.rgb_order               |
// | content shifted or rotated     | setRotation, cfg.offset_x/y |
#ifndef BRINGUP_COLOR_BARS
#define BRINGUP_COLOR_BARS 0
#endif

void panelSelfTest() {
#if BRINGUP_COLOR_BARS
  const struct { uint16_t c; const char *name; } bars[] = {
      {TFT_RED, "RED"}, {TFT_GREEN, "GREEN"}, {TFT_BLUE, "BLUE"}, {TFT_WHITE, "WHITE"},
  };
  const int32_t bandH = gfx.height() / 4;
  for (uint8_t i = 0; i < 4; i++) {
    gfx.fillRect(0, i * bandH, gfx.width(), bandH, bars[i].c);
    gfx.setTextColor(TFT_BLACK, bars[i].c);
    gfx.setTextSize(2);
    gfx.setCursor(8, i * bandH + 8);
    gfx.print(bars[i].name);
  }
  gfx.drawRect(0, 0, gfx.width(), gfx.height(), TFT_BLACK);
  gfx.setTextColor(TFT_BLACK);
  gfx.setCursor(8, gfx.height() - 24);
  gfx.printf("%dx%d", gfx.width(), gfx.height());
  delay(1500);
#else
  // The Dadabhagwan Foundation logo, drawn with raw LovyanGFX rather than as an
  // LVGL screen -- deliberately, so it appears BEFORE LVGL initialises and the
  // boot has no dark gap while the widget tree is built.
  //
  // drawPng decodes straight from flash; no framebuffer-sized RAM is needed for
  // a 128x128 image, and 9.9 KB of PNG beats 32 KB of raw RGB565 for something
  // shown once per boot.
  gfx.fillScreen(TFT_BLACK);
  gfx.drawPng(LOGO128_PNG, LOGO128_PNG_LEN, (gfx.width() - LOGO128_W) / 2,
              (gfx.height() - LOGO128_H) / 2 - 12);
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  gfx.setTextSize(1);
  gfx.setTextDatum(middle_center);
  gfx.drawString("Bowlstack", gfx.width() / 2, (gfx.height() + LOGO128_H) / 2 + 4);
  gfx.setTextDatum(top_left);
  delay(1200);
#endif
}

}  // namespace

void setup() {
  Serial.begin(115200);
  // USB-CDC enumerates only when a host opens the port, so the first lines are
  // lost without this. Bounded, not a spin: a unit on battery has no host and
  // must still boot.
  const uint32_t deadline = millis() + 2000;
  while (!Serial && (int32_t)(millis() - deadline) < 0) delay(10);

  Serial.println("\n=== Bowlstack :: Waveshare ESP32-S3-Touch-LCD-2 bring-up ===");
  Serial.printf("  chip        %s rev %d, %d MHz, %d core(s)\n", ESP.getChipModel(),
                ESP.getChipRevision(), getCpuFrequencyMhz(), ESP.getChipCores());
  Serial.printf("  flash       %u MB\n", ESP.getFlashChipSize() / (1024 * 1024));
  Serial.printf("  PSRAM       %u bytes free of %u\n", ESP.getFreePsram(), ESP.getPsramSize());
  Serial.printf("  heap        %u bytes free\n", ESP.getFreeHeap());

  if (ESP.getPsramSize() == 0) {
    Serial.println("  !! PSRAM NOT DETECTED -- expected 8 MB on an ESP32-S3R8.");
    Serial.println("  !! Check board_build.arduino.memory_type = qio_opi");
  }

  // --- stage 1 + 2: the buses --------------------------------------------
  Serial.println("\n--- I2C ---");
  scanBus(Wire, "touch + IMU bus (on-board)", board::TP_SDA, board::TP_SCL, 400000);
  Serial.println("    expect 0x15 (CST816D) and 0x6A/0x6B (QMI8658).");
  Serial.println("    Nothing here = the panel FPC is not seated.");
  Wire.end();

  scanBus(Wire1, "camera SCCB bus (candidate sensor bus A)", board::CAM_SCCB_SDA,
          board::CAM_SCCB_SCL, 400000);
  Serial.println("    expect nothing yet, but BOTH lines high -- R4/R5 are the");
  Serial.println("    4.7k pull-ups that make this pair usable for VL53L0X.");
  Wire1.end();

  // --- stage 3: the panel, raw -------------------------------------------
  Serial.println("\n--- display ---");
  gfx.init();
  gfx.setRotation(0);   // 0 = portrait 240x320, connector at the bottom
  gfx.setBrightness(0); // ramp up rather than flashing white at boot
  gfx.fillScreen(TFT_BLACK);
  for (uint8_t b = 0; b <= 200; b += 5) {
    gfx.setBrightness(b);
    delay(4);
  }
  Serial.printf("  panel %dx%d, touch %s\n", gfx.width(), gfx.height(),
                gfx.touch() ? "registered" : "NOT REGISTERED");
  panelSelfTest();

  // --- stage 4: LVGL ------------------------------------------------------
  Serial.println("\n--- lvgl ---");
  lv_init();
  lv_log_register_print_cb(lvglLog);
  // LVGL 9 takes its tick source at runtime. Without this every animation,
  // timer and input read period sees zero elapsed time, which presents as a
  // screen that draws once and never updates.
  lv_tick_set_cb(reinterpret_cast<lv_tick_get_cb_t>(millis));

  buf1 = (uint8_t *)heap_caps_malloc(LV_BUF_BYTES, MALLOC_CAP_DMA);
  buf2 = (uint8_t *)heap_caps_malloc(LV_BUF_BYTES, MALLOC_CAP_DMA);
  if (!buf1 || !buf2) {
    Serial.println("  FATAL: draw buffer allocation failed");
    return;
  }
  Serial.printf("  draw buffers 2 x %u bytes = %u lines (DMA-capable)\n",
                (unsigned)LV_BUF_BYTES, (unsigned)LV_BUF_LINES);

  lv_display_t *disp = lv_display_create(board::LCD_W, board::LCD_H);
  lv_display_set_flush_cb(disp, flushCb);
  lv_display_set_buffers(disp, buf1, buf2, LV_BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchCb);

  // --- stage 5: the shared UI --------------------------------------------
  // The REAL MAC, read from the eFuse. It needs no WiFi stack, which matters
  // here: this harness links no networking at all, and the MAC is precisely
  // what someone diagnosing a join failure asks for first.
  {
    uint8_t m[6];
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    static char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2],
             m[3], m[4], m[5]);
    ui::wifiSetMac(macStr);
    static char apSsid[33];
    snprintf(apSsid, sizeof(apSsid), "Bowlstack-%s", BOWLSTACK_DEVICE_ID);
    ui::wifiSetSetupAp(apSsid, "bowlstack");
    Serial.printf("  mac %s\n", macStr);
  }
  ui::demoInstallWifiMocks();

  ui::perfBegin();
  ui::buildPages();
  Serial.println("  2 pages, swipe horizontally -- identical to `pio run -e sim`:");
  Serial.println("    1  stock view, cycling scenarios every 3 s");
  Serial.println("    2  scope: 4 live traces 0-500 mm, with a frame-rate readout");

  // --- stage 6: battery ---------------------------------------------------
  analogSetPinAttenuation(board::PIN_BATTERY_ADC, ADC_11db);
  Serial.println("\n--- battery ---");
  Serial.printf("  GPIO%d, on-board 200k/100k divider (ratio %.2f)\n",
                board::PIN_BATTERY_ADC, board::BATTERY_DIVIDER_NOMINAL);
  Serial.println("  Compare 'cell' below against a multimeter at the MX1.25");
  Serial.println("  header, then set BOWLSTACK_BATTERY_CAL = cell / pin.");
  Serial.println("\nready.\n");
}

void loop() {
  // Bracketed so busy% is measured rather than guessed. That figure -- share of
  // wall clock spent inside LVGL -- is the one that decides whether this board
  // has anything left for four VL53L0X, a WiFi link and a Supabase POST.
  ui::perfFrameStart(millis());
  lv_timer_handler();
  ui::perfFrameEnd(millis());

  // Pixel shift is OFF -- see todo.md.
  // ui::pixelShiftTick(millis());

  // Drives both pages, same clock and same cadence as the desktop preview.
  ui::pagesTick(millis());

  // Feed the REAL battery into the UI. This is a legitimate platform-specific
  // source in the sense CLAUDE.md means it: the device has an ADC on a divider
  // and the desktop has neither, so measured-here / fabricated-there is a
  // difference in the world rather than in the fixtures.
  //
  // Once per second, not per frame. The 16-sample mean costs 16 ADC
  // conversions, a cell moves over hours, and the battery page is the only
  // thing that reads it.
  static uint32_t nextBatt = 0;
  if ((int32_t)(millis() - nextBatt) >= 0) {
    nextBatt = millis() + 1000;
    const uint16_t pinMv = readBatteryPinMv();
    const uint16_t cellMv = (uint16_t)(pinMv * board::BATTERY_DIVIDER_NOMINAL);

    // Bounded at BOTH ends, exactly as config.h argues. Below the floor the
    // input is floating rather than measuring; above the ceiling no lithium
    // cell can produce it, so the measurement is broken -- and it was a real
    // failure, a disconnected pin once reading 6365 mV as "100% (good)".
    ui::Battery band = ui::Battery::Unknown;
    int8_t pct = -1;
    if (cellMv >= 2500 && cellMv <= 4400) {
      const float soc = battery::socFromMillivolts(cellMv);
      pct = (int8_t)(soc + 0.5f);
      if (soc > 70.0f) band = ui::Battery::Good;
      else if (soc > 35.0f) band = ui::Battery::Medium;
      else if (soc > 10.0f) band = ui::Battery::Low;
      else band = ui::Battery::Critical;
    }
    ui::demoOverrideBattery(cellMv, pinMv, pct, band);
  }

  static uint32_t nextConsole = 0;
  const uint32_t now = millis();
  if ((int32_t)(now - nextConsole) >= 0) {
    nextConsole = now + 5000;
    const uint16_t pinMv = readBatteryPinMv();
    // The FPS figure is only meaningful WHILE THE SCOPE PAGE IS ON SCREEN.
    // LVGL does not draw objects that are scrolled off the tileview, so with
    // the stock view showing, the chart is invalidated but never rendered and
    // this reads near zero. That is not a stall -- it is the renderer correctly
    // declining to draw what nobody can see.
    char perf[96];
    ui::perfFormat(perf, sizeof(perf));
    Serial.printf("%s | batt %u mV | esp-heap %u\n", perf,
                  (uint16_t)(pinMv * board::BATTERY_DIVIDER_NOMINAL), ESP.getFreeHeap());
  }

  delay(5);
}
