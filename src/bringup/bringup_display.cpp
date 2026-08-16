// ---------------------------------------------------------------------------
// Waveshare ESP32-S3-Touch-LCD-2 bring-up harness.
//
// THIS IS NOT BOWLSTACK. It links no sensor, no WiFi and no telemetry, and it
// is not part of any shipping image -- build_src_filter keeps it out of every
// other environment. Its whole job is to answer, in order, the questions that
// make integration debuggable:
//
//   1. is the touch/IMU I2C bus alive?          (expect CST816D + QMI8658)
//   2. is the camera SCCB bus usable as our     (expect a silent but
//      second I2C bus?                           acknowledging bus)
//   3. does the panel light up, in the right    (raw LovyanGFX, no LVGL)
//      orientation, with the right polarity?
//   4. does LVGL flush to it correctly?
//   5. does touch reach an LVGL widget?
//   6. does the on-board battery divider read
//      a plausible cell voltage on GPIO5?
//
// Each is separated deliberately. Bringing a display and a GUI toolkit up in
// one step means a blank screen has a dozen possible causes; bringing the
// panel up with raw LovyanGFX FIRST means that by the time LVGL is involved,
// "the screen works" is already established fact.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#include "board_waveshare_s3.h"
#include "lgfx_waveshare_s3.h"

namespace {

LGFX_WaveshareS3Touch2 gfx;

// Partial-render buffers. A tenth of the panel each, double buffered: LVGL
// renders into one while the other is in flight over SPI DMA.
//
// Full-frame buffering was the alternative and is rejected -- 240x320x2 is
// 150 KB, and two of those would be 300 KB of the S3's 512 KB internal SRAM
// spent on a screen that changes a digit every few seconds. Partial mode costs
// only extra flush calls, and this UI redraws small regions.
//
// They stay in INTERNAL RAM. PSRAM would work, but SPI DMA out of PSRAM on the
// S3 goes through the cache and is measurably slower than out of SRAM, which
// is the wrong trade for the one buffer that is touched every single frame.
// Bytes per pixel is stated explicitly and NOT taken from sizeof(lv_color_t).
//
// In LVGL 9 `lv_color_t` is a 3-byte {blue, green, red} struct REGARDLESS of
// LV_COLOR_DEPTH -- it is the API's colour type, not the draw buffer's pixel
// format. At LV_COLOR_DEPTH 16 the buffer holds 2-byte pixels (`lv_color16_t`),
// which is also why the flush callback casts to lgfx::rgb565_t.
//
// Sizing a buffer with sizeof(lv_color_t) therefore over-allocates by 50% and,
// worse, makes the line count a lie: `240 * 32 * sizeof(lv_color_t)` is 23040
// bytes, which LVGL divides by 2 and uses as 48 lines, not 32. It is not
// unsafe -- allocation and declared size agree -- but every number in the
// comment around it is wrong, which is how a later "optimisation" turns a
// harmless discrepancy into a real overflow.
const uint32_t LV_BUF_BPP = 2;  // LV_COLOR_DEPTH 16
const uint32_t LV_BUF_LINES = 48;
const uint32_t LV_BUF_PX = board::LCD_W * LV_BUF_LINES;
const uint32_t LV_BUF_BYTES = LV_BUF_PX * LV_BUF_BPP;

uint8_t *buf1 = nullptr;
uint8_t *buf2 = nullptr;

lv_obj_t *lblBattery = nullptr;
lv_obj_t *lblTouch = nullptr;
lv_obj_t *lblTaps = nullptr;
uint32_t tapCount = 0;

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
// Reports what acknowledges, and just as importantly reports the difference
// between "nothing is there" and "the bus is stuck". A pair of lines held low
// by a wiring fault looks identical to an empty bus through a scan alone, so
// the idle levels are read directly first.
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

void onTap(lv_event_t *) {
  tapCount++;
  lv_label_set_text_fmt(lblTaps, "taps: %u", tapCount);
  Serial.printf("bringup: tap %u\n", tapCount);
}

// --- stage 3: raw panel test, before LVGL exists ---------------------------
// If this stage looks wrong, LVGL cannot fix it and adding LVGL will only
// obscure it. Specifically:
//   - inverted colours (white text on white)  -> cfg.invert in the LGFX header
//   - red and blue swapped                    -> cfg.rgb_order
//   - content off the edge / rotated          -> setRotation, offset_x/y
void panelSelfTest() {
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

  // Corner ticks prove no rows or columns are lost to a wrong offset.
  gfx.drawRect(0, 0, gfx.width(), gfx.height(), TFT_BLACK);
  gfx.setTextColor(TFT_BLACK);
  gfx.setCursor(8, gfx.height() - 24);
  gfx.printf("%dx%d", gfx.width(), gfx.height());
  delay(1500);
}

// --- stage 4/5: the LVGL screen -------------------------------------------
// Laid out roughly the way the real stock view will be, so this doubles as a
// first look at the eventual UI rather than being throwaway scaffolding.
void buildScreen() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "Bowlstack  bring-up");
  lv_obj_set_style_text_color(title, lv_color_hex(0xE6EDF3), LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

  // A mock stack column, bottom-up, f1 at the bottom -- the same orientation
  // the handoff doc specifies for the web UI, and the same one the physical
  // pipe has. Getting this the right way up in the mock is cheap; discovering
  // it upside down after the logic is wired is not.
  const char *levels[] = {"f4", "f3", "f2", "f1"};
  const bool present[] = {false, false, true, true};
  for (uint8_t i = 0; i < 4; i++) {
    lv_obj_t *cell = lv_obj_create(scr);
    lv_obj_set_size(cell, 120, 44);
    lv_obj_align(cell, LV_ALIGN_TOP_LEFT, 14, 48 + i * 50);
    lv_obj_set_style_radius(cell, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(cell, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(cell, lv_color_hex(0x30363D), LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        cell, present[i] ? lv_color_hex(0x1F6F43) : lv_color_hex(0x1C2128), LV_PART_MAIN);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(cell);
    lv_label_set_text_fmt(l, "%s  %s", levels[i], present[i] ? "present" : "absent");
    lv_obj_set_style_text_color(l, lv_color_hex(0xE6EDF3), LV_PART_MAIN);
    lv_obj_center(l);
  }

  lv_obj_t *count = lv_label_create(scr);
  lv_label_set_text(count, "2");
  lv_obj_set_style_text_font(count, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_set_style_text_color(count, lv_color_hex(0xE6EDF3), LV_PART_MAIN);
  lv_obj_align(count, LV_ALIGN_TOP_RIGHT, -40, 90);

  lv_obj_t *countCap = lv_label_create(scr);
  lv_label_set_text(countCap, "bowls");
  lv_obj_set_style_text_color(countCap, lv_color_hex(0x8B949E), LV_PART_MAIN);
  lv_obj_align(countCap, LV_ALIGN_TOP_RIGHT, -38, 150);

  lblBattery = lv_label_create(scr);
  lv_obj_set_style_text_color(lblBattery, lv_color_hex(0x8B949E), LV_PART_MAIN);
  lv_obj_align(lblBattery, LV_ALIGN_BOTTOM_LEFT, 14, -64);
  lv_label_set_text(lblBattery, "battery: ...");

  lblTouch = lv_label_create(scr);
  lv_obj_set_style_text_color(lblTouch, lv_color_hex(0x8B949E), LV_PART_MAIN);
  lv_obj_align(lblTouch, LV_ALIGN_BOTTOM_LEFT, 14, -46);
  lv_label_set_text(lblTouch, "touch: -");

  // The tap counter is the end-to-end input proof. Raw coordinates printing on
  // the console only shows the driver works; a widget reacting shows that
  // LVGL's indev is registered, the coordinates land in the right space, and
  // hit-testing agrees with what is drawn.
  lv_obj_t *btn = lv_button_create(scr);
  lv_obj_set_size(btn, 130, 40);
  lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, -14, -12);
  lv_obj_add_event_cb(btn, onTap, LV_EVENT_CLICKED, nullptr);

  lblTaps = lv_label_create(btn);
  lv_label_set_text(lblTaps, "taps: 0");
  lv_obj_center(lblTaps);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  // USB-CDC enumerates only when a host opens the port, so the first lines are
  // lost without this. It is a bounded wait, not a spin: a unit powered from a
  // battery has no host and must still boot.
  const uint32_t deadline = millis() + 2000;
  while (!Serial && (int32_t)(millis() - deadline) < 0) delay(10);

  Serial.println("\n=== Bowlstack :: Waveshare ESP32-S3-Touch-LCD-2 bring-up ===");
  Serial.printf("  chip        %s rev %d, %d MHz, %d core(s)\n", ESP.getChipModel(),
                ESP.getChipRevision(), getCpuFrequencyMhz(), ESP.getChipCores());
  Serial.printf("  flash       %u MB\n", ESP.getFlashChipSize() / (1024 * 1024));
  Serial.printf("  PSRAM       %u bytes free of %u\n", ESP.getFreePsram(), ESP.getPsramSize());
  Serial.printf("  heap        %u bytes free\n", ESP.getFreeHeap());

  if (ESP.getPsramSize() == 0) {
    // Not a warning worth burying. An ESP32-S3R8 reporting no PSRAM means the
    // build did not declare octal PSRAM, and the failure it eventually causes
    // is a runtime allocation error far from this cause.
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
  gfx.setRotation(0);          // 0 = portrait 240x320, connector at the bottom
  gfx.setBrightness(0);        // ramp up rather than flashing white at boot
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
  // timer and the input-device read period stall at zero elapsed time, which
  // presents as a screen that draws once and then never updates.
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

  buildScreen();

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
  lv_timer_handler();

  static uint32_t nextSlow = 0;
  const uint32_t now = millis();
  if ((int32_t)(now - nextSlow) >= 0) {
    nextSlow = now + 500;

    const uint16_t pinMv = readBatteryPinMv();
    const uint16_t cellMv = (uint16_t)(pinMv * board::BATTERY_DIVIDER_NOMINAL);
    if (lblBattery) lv_label_set_text_fmt(lblBattery, "batt  pin %u mV -> cell %u mV", pinMv, cellMv);

    uint16_t x, y;
    if (gfx.getTouch(&x, &y)) {
      if (lblTouch) lv_label_set_text_fmt(lblTouch, "touch  x=%u y=%u", x, y);
    }

    static uint32_t nextConsole = 0;
    if ((int32_t)(now - nextConsole) >= 0) {
      nextConsole = now + 5000;
      Serial.printf("battery: pin %u mV -> cell %u mV   heap %u  lvgl-taps %u\n", pinMv, cellMv,
                    ESP.getFreeHeap(), tapCount);
    }
  }

  delay(5);
}
