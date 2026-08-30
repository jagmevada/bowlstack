// ---------------------------------------------------------------------------
// Bowlstack :: load-cell station -- Waveshare ESP32-S3-Touch-LCD-2,
//              3x NAU7802 behind a TCA9548A
//
// Same board and the same UI as the touch-ui branch; a different measurement
// underneath it. Where that branch put four VL53L0X on a pipe and counted
// bowls, this one puts three 20 kg cells under one platform and weighs what is
// on it. Three rather than two because two leave the platform free to rock --
// see scale.h.
//
// WHAT THIS IMAGE IS FOR, IN ORDER:
//
//   1. does the mux answer at 0x70, and a converter at 0x2A on each channel?
//   2. do all three converters power up, self-calibrate and produce conversions?
//   3. at what rate, actually delivered rather than configured?
//   4. does the sum behave -- does load on one corner move one trace?
//
// Questions 1-3 are answered on the console within a second of boot and on the
// dashboard permanently. Question 4 is what the scope page is for.
//
// IT REPORTS COUNTS UNTIL SOMEBODY CALIBRATES IT, and that is deliberate rather
// than unfinished. Without a known mass there is no counts-to-gram factor, so
// there are no grams -- and a plausible-looking weight derived from an assumed
// factor is the one kind of wrong this codebase refuses everywhere else. Menu >
// Settings > Scale > Tare, put the known mass on, then Calibrate.
//
// The WiFi and NTP halves live in src/bringup/bringup_wifi.cpp and
// bringup_time.cpp -- linked by name rather than copied, so there is one
// implementation of each. NTP is still the harness's. WiFi no longer is: it
// gained the half that makes a station a product rather than a demonstration --
// credentials that survive a power cycle, a ranked auto-join at boot, and a
// bounded reconnect. See the note at the top of that file for why WiFiManager
// and net.cpp are not part of it.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <lvgl.h>
#include <math.h>

#include <esp_mac.h>

#include "bringup_time.h"
#include "bringup_wifi.h"

#include "battery_soc.h"
#include "board_waveshare_s3.h"
// Explicit, though battery_soc.h already pulls it in. This file reads
// config::BATTERY_DIVIDER and config::BATTERY_SAMPLE_INTERVAL_MS directly, and a
// header you depend on by name should be one you include by name -- otherwise
// the day battery_soc.h stops needing it, this file breaks for a reason that has
// nothing to do with anything in it.
#include "config.h"
#include "lgfx_waveshare_s3.h"
#include "logo128.h"
#include "scale.h"
#include <Preferences.h>

#include "inputs.h"
#include "scale_telemetry.h"
#include "ui_calib.h"
#include "ui_demo.h"
#include "ui_pages.h"
#include "ui_perf.h"
#include "ui_scope.h"
#include "ui_screens.h"
#include "ui_weight.h"
#include "ui_wifi.h"
#include "version.h"

namespace {

// THE ONE PLACE BOTH CELL COUNTS ARE VISIBLE AT ONCE. scale::CELLS and
// ui::CELLS have to agree and cannot be shared: ui_state.h deliberately
// includes no driver header, which is what lets the desktop preview compile the
// same screens. This file includes both, so this is where the duplication gets
// checked -- at compile time, rather than as a cell that renders as a missing
// row on a panel nobody is looking at.
static_assert(scale::CELLS == ui::CELLS, "scale::CELLS and ui::CELLS must match");

// How long to wait for a USB host to open the CDC port before giving up and
// booting anyway. Every millisecond of it is splash time -- nothing is on the
// screen yet -- so it is a knob rather than a constant.
//
// 2000 was the original, and on a bench it is nearly always paid in full: the
// monitor attaches a second or so after the reset, and until it does `Serial`
// is false. On a unit running on battery in a kitchen there is no host at all
// and it is ALWAYS paid in full, to keep boot lines nobody will read.
//
// 400 keeps the useful case -- a host already attached when the board resets,
// which is what `pio run -t upload -t monitor` produces -- and stops paying for
// the case where there is no host. Raise it if early boot lines go missing.
#ifndef BOWLSTACK_CONSOLE_WAIT_MS
#define BOWLSTACK_CONSOLE_WAIT_MS 400
#endif
const uint32_t CONSOLE_WAIT_MS = BOWLSTACK_CONSOLE_WAIT_MS;

// --- boot phase timing -----------------------------------------------------
// THE BOOT LOG NAMED EVERY PHASE AND TIMED NONE OF THEM. "It sits on the logo
// for ten seconds" was therefore a question the device could not answer about
// itself: the phases print in order, so you can see WHERE it is, and nothing
// tells you how long any of them took. Answering it needed a host-side
// timestamp filter and arithmetic on wall-clock stamps, which is detective work
// to recover something the firmware knew and threw away.
//
// Both figures matter and neither substitutes for the other: the delta is what
// you optimise, the total is what the person watching the splash experiences.
uint32_t bootPhaseMs_ = 0;
uint32_t bootStartMs_ = 0;

void bootMark(const char *what) {
  const uint32_t now = millis();
  Serial.printf("  [+%4lu ms, %5lu total] %s\n", (unsigned long)(now - bootPhaseMs_),
                (unsigned long)(now - bootStartMs_), what);
  bootPhaseMs_ = millis();
}

LGFX_WaveshareS3Touch2 gfx;

// Bytes per pixel is stated explicitly and NOT taken from sizeof(lv_color_t).
// In LVGL 9 `lv_color_t` is a 3-byte {b,g,r} struct REGARDLESS of
// LV_COLOR_DEPTH -- it is the API's colour type, not the draw buffer's pixel
// format. At depth 16 the buffer holds 2-byte pixels, which is also why the
// flush callback casts to lgfx::rgb565_t.
const uint32_t LV_BUF_BPP = 2;
const uint32_t LV_BUF_LINES = 48;
const uint32_t LV_BUF_PX = board::LCD_W * LV_BUF_LINES;
const uint32_t LV_BUF_BYTES = LV_BUF_PX * LV_BUF_BPP;

uint8_t *buf1 = nullptr;
uint8_t *buf2 = nullptr;

// Set only once the display, buffers and input device all exist. Without it an
// early return above leaves loop() calling pagesTick() against widgets that
// were never built -- a panic reboot, so the FATAL line the operator is meant
// to read scrolls past in a boot loop.
bool uiReady_ = false;

uint16_t lastPinMv_ = 0;
uint16_t lastCellMv_ = 0;

// --- battery ---------------------------------------------------------------
// The same 16-sample mean the other images use. A single ESP32 conversion
// carries tens of millivolts of noise, and the steep end of a Li-ion discharge
// curve turns that into several percent of apparent charge.
uint16_t readBatteryPinMv() {
  uint32_t acc = 0;
  for (uint8_t i = 0; i < 16; i++) acc += analogReadMilliVolts(board::PIN_BATTERY_ADC);
  return (uint16_t)(acc / 16);
}

// THE BAND IS NOT COMPUTED HERE ANY MORE, and that is the whole point of this
// block. It used to be three bare comparisons in loop() -- soc > 70 / 35 / 10 --
// which are exactly config.h's FALLING edges with their rising partners
// (75/40/15) never consulted. A classifier with one threshold per boundary
// oscillates whenever its input rests on one, and a battery rests on one for
// hours: battery_soc.h records the measured trace, a stationary cell alternating
// low -> medium -> low -> medium on +/-23 mV of ADC noise. Every alternation
// will be a Supabase write once the uplink lands, which is why this is worth
// fixing BEFORE that rather than after.
//
// battery::Monitor is the classifier the discrete product already ships, and it
// is header-only -- no new translation unit, no filter change, no link risk. It
// carries four things the inline version had none of: an EMA over time, six
// hysteretic band thresholds, a 500 ms presence dwell that survives the contact
// bounce of a cell being plugged in, and the order-matters rule that presence is
// decided on the RAW sample so the filter never learns from the absent regime.
//
// WHAT IT STILL CANNOT SEE, stated because the fix looks complete and is not:
// with no cell fitted and USB power applied, the ETA6098 holds the BAT node at
// its charge voltage, so the divider faithfully reports ~4.17 V and this
// classifies a healthy `good` battery on a unit that has none. Nothing in
// software distinguishes those; it needs the charger-sense mod in todo.md, where
// "charging, pinned at 4.2 V, no droop under load" is what identifies it.
battery::Monitor batteryMonitor_;

// Rate limiter, the same shape as device_status::servicePower(). The interval is
// not cosmetic: BATTERY_EMA_ALPHA is expressed in SAMPLES, so the 0.20 that
// config.h describes as a ~500 ms time constant only is one at this 100 ms
// cadence. The old 1000 ms loop made the identical alpha a five-second filter,
// which is a different instrument wearing the same constant.
uint32_t nextBatterySampleMs_ = 0;
bool batteryEverSampled_ = false;

ui::Battery toUiBattery(battery::Level l) {
  switch (l) {
    case battery::Level::Good: return ui::Battery::Good;
    case battery::Level::Medium: return ui::Battery::Medium;
    case battery::Level::Low: return ui::Battery::Low;
    case battery::Level::Critical: return ui::Battery::Critical;
    default: return ui::Battery::Unknown;
  }
}

// --- the uplink task -------------------------------------------------------
// CORE 0, AND NOT loop(). docs/firmware.md puts every piece of network work on
// core 0, below anything that measures, and the discrete product's tasks.cpp
// does exactly that for its telemetry. Here there is a second reason that is
// specific to this board: loop() is the RENDER loop. One request can take the
// full 8 s HTTP timeout, and inside loop() that is eight seconds of frozen
// panel, frozen touch and a stalled 20 Hz publish -- on a device whose whole
// point is a live readout.
//
// The scale task keeps owning the cells; this task only ever takes a snapshot,
// which is the same immutable copy every other consumer gets.
TaskHandle_t uplinkTask_ = nullptr;

// Power, packed into ONE 32-bit word so the uplink task can read it without a
// mutex -- the same trick, for the same reason, as bringup_time.cpp's clock.
// Aligned 32-bit loads and stores are atomic on this core, so a reader either
// sees the whole previous value or the whole new one, never a millivolt figure
// from one sample beside a band from the next. A mutex here would let a task
// doing TLS block the loop that writes it.
//
//   bits 0-15   cell millivolts, EMA-filtered
//   bits 16-23  battery::Level as decided by the hysteresis
volatile uint32_t powerPacked_ = 0;

void publishPower(uint16_t cellMv, battery::Level level) {
  powerPacked_ = ((uint32_t)level << 16) | (uint32_t)cellMv;
}

void uplinkTaskFn(void *) {
  for (;;) {
    // Nothing to report until the converters have published once. A
    // zero-initialised snapshot would go out as `no_cells`, which is a claim
    // about the hardware rather than about the fact that it has not spoken yet.
    if (scale::ready()) {
      const uint32_t p = powerPacked_;
      scale_telemetry::loop(scale::snapshot(), millis() / 1000,
                            (uint16_t)(p & 0xFFFF),
                            (battery::Level)((p >> 16) & 0xFF));
    }
    // Four times a second. scale_telemetry::loop() does its own rate limiting,
    // so this only bounds how promptly a change is NOTICED -- and the 5 s post
    // floor means anything faster would just be waking to decide not to send.
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

void startUplinkTask() {
  // 12 KB, the figure tasks.cpp arrived at for the same work on the discrete
  // board: "TLS alone consumed ~5 KB the first time it ran, and a full
  // handshake against a longer certificate chain can spike further". Priority 1
  // on core 0, beside the time task and below everything that measures.
  xTaskCreatePinnedToCore(uplinkTaskFn, "uplink", 12288, nullptr, 1, &uplinkTask_, 0);
  Serial.println("uplink: task started on core 0");
}

uint32_t uplinkStackFreeBytes() {
  // BYTES on ESP-IDF, not words -- see bringup_time::stackFreeBytes().
  return uplinkTask_ ? uxTaskGetStackHighWaterMark(uplinkTask_) : 0;
}

// --- LVGL bindings ---------------------------------------------------------

void lvglLog(lv_log_level_t, const char *msg) { Serial.printf("lvgl: %s\n", msg); }

void flushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  const int32_t w = area->x2 - area->x1 + 1;
  const int32_t h = area->y2 - area->y1 + 1;
  const uint32_t t0 = micros();

  gfx.startWrite();
  gfx.setAddrWindow(area->x1, area->y1, w, h);
  // Casting to lgfx::rgb565_t rather than a bare uint16_t* is what tells
  // LovyanGFX the source pixel format, so it handles byte order itself. Pushed
  // as raw uint16_t the image renders in convincing but wrong colours.
  gfx.writePixels(reinterpret_cast<lgfx::rgb565_t *>(px_map), w * h);
  gfx.endWrite();

  ui::perfFlush((uint32_t)(w * h), micros() - t0);
  lv_display_flush_ready(disp);
}

// --- touch, and what a dead touch bus costs -------------------------------
// A FAILING TOUCH CHIP MUST NOT DESTROY THE FRAME RATE, and left alone it does.
//
// LVGL polls the input device from inside lv_timer_handler(), roughly every
// 30 ms. LovyanGFX's CST816S driver answers by re-running its init probe and
// then retrying the data read up to three times -- and on a bus where nothing
// acknowledges, each of those transactions runs to the I2C peripheral's ~13 ms
// timeout. Five failing transactions is ~65 ms spent inside lv_timer_handler
// for a read that returns "no finger", every 30 ms. The renderer is then
// drawing a few thousand pixels per frame and still managing six of them a
// second, which is exactly the shape of the numbers this board reported:
//
//     fps 6  ui 84%  worst 172ms | 7795px/f  spi~3ms
//
// Tiny pixel counts, trivial SPI time, and a loop that is nevertheless pegged.
// That combination cannot be a slow renderer; it is a blocked one.
//
// So the read is TIMED, and if it starts taking milliseconds the driver is
// clearly not talking to anything -- back off to one attempt every half second
// and say so once. The screen stops being responsive to touch, which it already
// was not, and stops taking the rest of the UI down with it.
const uint32_t TOUCH_SLOW_US = 3000;    // a healthy read is tens of microseconds
const uint32_t TOUCH_BACKOFF_MS = 500;  // retry cadence once it looks dead
uint8_t touchSlowRun_ = 0;
uint32_t touchNextTryMs_ = 0;
bool touchWarned_ = false;

// THE ONLY PLACE THE TOUCH CONTROLLER IS READ. Nothing else may call
// gfx.getTouch().
//
// The CST816D hands over a touch event and clears its data register in the same
// transaction, so a second reader does not observe the same event -- it steals
// it, leaving LVGL to infer press/release from a stream with holes in it.
//
// THIS NO LONGER SHARES A PORT WITH ANY CELL, and that is worth stating because
// the previous build's version of this comment argued the opposite case. The
// touch read ran on the UI task while the scale task drove cell A on the same
// port 0, safe because lgfx takes a per-port FreeRTOS mutex across each
// transaction. With the cells behind the mux on port 1 that overlap is gone --
// but the mutex argument still matters, one port over: the mux channel select
// and the register read that follows it are TWO transactions, and only the fact
// that a single task owns port 1 keeps another from switching the channel
// between them. See i2cmux.h. Both must still go through lgfx rather than one
// of them opening Wire -- see nau7802.h.
void touchCb(lv_indev_t *, lv_indev_data_t *data) {
  data->state = LV_INDEV_STATE_RELEASED;

  const uint32_t now = millis();
  if (touchSlowRun_ >= 3 && (int32_t)(now - touchNextTryMs_) < 0) return;

  const uint32_t t0 = micros();
  uint16_t x, y;
  const bool touched = gfx.getTouch(&x, &y);
  const uint32_t took = micros() - t0;
  ui::perfTouch(took);

  if (took > TOUCH_SLOW_US) {
    if (touchSlowRun_ < 255) touchSlowRun_++;
    touchNextTryMs_ = now + TOUCH_BACKOFF_MS;
    if (touchSlowRun_ == 3 && !touchWarned_) {
      touchWarned_ = true;
      Serial.printf(
          "\n!! TOUCH IS NOT ANSWERING: a read took %lu us, which is an I2C timeout and\n"
          "   not a touch controller. GPIO47/48 now carry ONLY the touch chip and the IMU\n"
          "   -- the cells moved behind the mux on GPIO12/11 -- so unlike the two-cell\n"
          "   build this can no longer be a load cell dragging the bus down. Backing touch\n"
          "   off to one read every %lu ms so the rest of the UI keeps its frame rate.\n\n",
          (unsigned long)took, (unsigned long)TOUCH_BACKOFF_MS);
    }
    return;
  }

  // A read that completed promptly means the bus is healthy again -- a wedged
  // slave can recover on its own, and a device that stays permanently degraded
  // after one bad second would be worse than the problem.
  touchSlowRun_ = 0;

  if (touched) {
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
  }
}

void bootSplash() {
  // Drawn with raw LovyanGFX rather than as an LVGL screen -- deliberately, so
  // it appears BEFORE LVGL initialises and the boot has no dark gap while the
  // widget tree is built.
  //
  // drawPng decodes straight from flash, so no framebuffer-sized RAM is needed
  // for a 128x128 image.
  //
  // IT IS ALSO NOT SLOW, which is worth recording because it looks like it
  // should be. Chasing a 1249 ms splash phase, decoding 16 k pixels on an
  // Xtensa core was the obvious suspect and it was wrong: the whole of this
  // function -- clear, decode, three lines of text -- measures 44 ms. The
  // 1200 ms was a deliberate hold at the end, which is now gone. Do not trade
  // 23 KB of flash for a raw RGB565 array on the strength of how this reads.
  gfx.fillScreen(TFT_BLACK);
  gfx.drawPng(LOGO128_PNG, LOGO128_PNG_LEN, (gfx.width() - LOGO128_W) / 2,
              (gfx.height() - LOGO128_H) / 2 - 12);
  gfx.setTextColor(TFT_WHITE, TFT_BLACK);
  // A REAL TYPEFACE, NOT THE 5x7 GLCD FONT SCALED UP, which is what made this
  // read as dot matrix: LovyanGFX's default is a 5-by-7 bitmap, and every pixel
  // of it becomes a visible square the moment it is enlarged. DejaVu is drawn
  // at its native size from properly shaped glyphs, so the strokes are strokes
  // rather than stacks of dots.
  //
  // It is still ONE BIT PER PIXEL, and that is a limit of the splash rather
  // than of this font: LovyanGFX only anti-aliases VLW and TTF faces, both of
  // which mean shipping a font asset. Everything AFTER this screen is drawn by
  // LVGL in Montserrat at 4 bpp and is genuinely smooth -- the splash is the
  // one surface that exists before LVGL does, which is the whole reason it is
  // drawn this way (no dark gap while the widget tree is built).
  //
  // If the shapes alone are not enough, the fix is to hand the splash to LVGL
  // as well and accept a beat of black at power-on.
  // THREE LINES: who made it, which unit this is, and what it is running. That
  // is the whole job of a splash on a device that will be one of many -- the
  // last two are the first things anybody asks about a unit that is
  // misbehaving, and this is the only screen that shows them without navigating.
  //
  // The id and version come from the build flags rather than being typed here,
  // so the splash cannot disagree with what the console prints or with what the
  // Diagnose page reports.
  //
  // FONT2 RATHER THAN A DEJAVU, because 16 px is what is wanted and DejaVu is
  // shipped at 9/12/18/24/40/56/72 -- 12 is a long way down from 18 and 18 is
  // not 16. Font2 is lgfx/Fonts/Font16.h: chr_hgt 16, baseline 13, proportional
  // widths from its own table. It is a different family from DejaVu but it is a
  // real 16 px face, which is the point -- NOT Font0 scaled up, which is the
  // 5x7 GLCD bitmap that made this screen read as dot matrix in the first place.
  //
  // THE BLOCK IS ANCHORED TO THE BOTTOM EDGE, not hung off the logo. Growing it
  // downward from the logo pushed the text toward the panel edge as lines were
  // added and crowded the artwork as they were removed; anchoring it to the
  // bottom instead means the logo keeps its air whatever the block does, and a
  // fourth line would eat into the gap rather than run off the screen. With
  // three lines the text starts at y=262 and the logo ends at y=212, so there
  // are 50 px of black between them.
  gfx.setFont(&fonts::Font2);
  gfx.setTextDatum(middle_center);
  {
    const char *const lines[] = {"Unodari", BOWLSTACK_DEVICE_ID,
                                 BOWLSTACK_FW_VERSION};
    const int32_t nLines = (int32_t)(sizeof(lines) / sizeof(lines[0]));
    const int32_t LINE_H = 20;   // 16 px of glyph plus 4 of leading
    const int32_t MARGIN = 8;    // black below the last line
    const int32_t cx = gfx.width() / 2;
    // middle_center, so this is the CENTRE of the first line: the last line's
    // centre sits MARGIN + half a line above the bottom, and the rest stack up
    // from there.
    int32_t y = gfx.height() - MARGIN - LINE_H / 2 - LINE_H * (nLines - 1);
    for (int32_t i = 0; i < nLines; i++, y += LINE_H)
      gfx.drawString(lines[i], cx, y);
  }
  gfx.setTextDatum(top_left);
  gfx.setFont(&fonts::Font0);
  bootMark("splash draw");

  // THE 1200 ms HOLD IS GONE, and it was the single largest thing in the boot
  // after the cells. It existed so the splash stayed up long enough to read --
  // which was a real need when it was written and is not one now: the splash
  // remains on screen for the whole of cell bring-up and page construction,
  // measured at just under two seconds, because nothing else draws until
  // buildPages() finishes.
  //
  // So the hold was not making the splash visible, it was ADDING to a period
  // the splash already filled. Anything that later makes boot faster than a
  // person can read three lines should reinstate a hold rather than restore
  // this one blindly -- and should measure what the rest of boot costs first.
}

// --- the adapter -----------------------------------------------------------
// Where the firmware's vocabulary meets the screen's. Deliberately a
// TRANSLATION rather than a shared type: ui_state.h may not include a driver
// header, and scale.h reaches for Arduino, FreeRTOS and NVS.
ui::Cell toUiCell(CellState s) {
  switch (s) {
    case CellState::Online: return ui::Cell::Online;
    case CellState::Warming: return ui::Cell::Warming;
    default: return ui::Cell::Offline;
  }
}

void publishScale(uint32_t nowMs) {
  const scale::Snapshot sn = scale::snapshot();

  ui::State s = ui::demoLatest(nowMs);
  s.scale.calibrated = sn.calibrated;
  s.scale.tared = sn.tared;
  s.scale.online = sn.online;
  s.scale.totalGrams = sn.totalGrams;
  s.scale.overRange = sn.overRange;
  s.scale.countsPerGram = sn.countsPerGram;
  s.scale.window = sn.window;
  s.scale.decimals = sn.decimals;
  s.scale.showCells = sn.showCells;
  s.scale.zeroed = sn.zeroed;
  s.scale.calMassG = sn.calMassG;

  int32_t totalCounts = 0;
  for (uint8_t i = 0; i < ui::CELLS; i++) {
    ui::CellView &c = s.scale.cell[i];
    c.state = toUiCell(sn.cell[i].state);
    // Tare subtracted here rather than in scale.cpp, so the snapshot keeps the
    // absolute converter reading -- which is what the console line and any
    // future diagnostics want -- while the screen shows the cell's own
    // contribution.
    c.counts = sn.cell[i].counts - sn.cell[i].platformZero - sn.cell[i].tare;
    c.grams = sn.cell[i].grams;
    c.sps = sn.cell[i].sps;
    c.overRange = sn.cell[i].overRange;
    // The unprocessed figures, for the Device page. Deliberately NOT tared and
    // NOT filtered: that page exists to show what the dashboard's number was
    // made from, and a pre-chewed copy of it would show nothing.
    c.rawCounts = sn.cell[i].rawCounts;
    c.pp = sn.cell[i].pp;
    c.platformZero = sn.cell[i].platformZero;
    c.tare = sn.cell[i].tare;
    if (c.state == ui::Cell::Online) totalCounts += c.counts;
  }
  s.scale.totalCounts = totalCounts;

  s.deviceId = BOWLSTACK_DEVICE_ID;
  s.firmware = BOWLSTACK_FW_VERSION;
  // NO RTC ON THIS BOARD. bringup_time overrides this when a sync lands; until
  // then the bar shows "--:--" rather than a number it has not earned.
  s.timeKnown = false;

  // THERE ARE NO ToF SENSORS ON THIS PRODUCT, so say so rather than carrying the
  // fixture's answer. `s` starts life as a copy of demoLatest(), which on the
  // first call is scenario 0 -- four VL53L0X reported online, an Ok stack and
  // four Absent levels. demoOverrideState() then freezes the cycle, so those
  // four claims about hardware this board does not have ride in the published
  // snapshot for the rest of the power cycle.
  //
  // Nothing renders them today: the bowl page is not built in this image. That
  // is exactly why it is worth setting them now -- an inherited value that is
  // invisible is one that will be believed the first time something reads it,
  // and the charge-state field beside them was already correct only by an
  // accident of scenario ordering.
  for (uint8_t i = 0; i < ui::LEVELS; i++) {
    s.levels[i] = ui::Level::Unknown;
    s.sensorOnline[i] = false;
  }
  s.sensorsOnline = 0;
  s.stackCount = 0;
  // Degraded, not Ok. "No trustworthy count" is the truth about a device with no
  // level sensors; Ok would assert a count of zero bowls, which is a measurement
  // nothing here made.
  s.stack = ui::Stack::Degraded;

  ui::demoOverrideState(s);

  // The scope gets the same figures the dashboard does, in whichever unit the
  // dashboard is using. Feeding it counts while the total says grams would make
  // the two pages disagree about the same instant.
  int32_t v[ui::CELLS];
  bool ok[ui::CELLS];
  for (uint8_t i = 0; i < ui::CELLS; i++) {
    ok[i] = s.scale.cell[i].state == ui::Cell::Online;
    v[i] = sn.calibrated ? (int32_t)lroundf(s.scale.cell[i].grams) : s.scale.cell[i].counts;
  }
  ui::scopeFeed(v, ok, sn.calibrated ? "g" : "cts");
}

// --- menu handlers ---------------------------------------------------------
// Called on the UI task from a row tap. scale::tare() and clearCalibration()
// only raise a flag for the scale task, so they return at once; calibrate()
// waits, bounded, because its answer decides what the screen may show next.
//
// The calibration mass is no longer a constant here -- it lives in NVS and is
// typed on the Calibrate page. scale::calMass() is the one place that knows it.

void onTare() {
  Serial.println("ui: tare requested");
  scale::tare();
}

// Called from the keypad's OK, with whatever mass was typed.
//
// THE REFUSAL REASON IS WORKED OUT HERE rather than returned by scale::, and
// deliberately: the causes are all visible in the snapshot, and a page that
// said only "refused" would send somebody to a serial console to find out why
// -- which rather defeats the point of putting calibration on the device.
void onCalibApply(float grams) {
  Serial.printf("ui: calibrate against %.0f g requested\n", grams);

  // THE VERDICT AND THE FACTOR BOTH COME BACK FROM THE MEASURING TASK. This
  // used to call a bool-returning calibrate() and then reconstruct both from a
  // snapshot, which was wrong twice over: the snapshot is up to a publish period
  // stale so the "succeeded" line quoted the factor it had just replaced, and
  // the reason was guessed from a ladder that had no rung for a below-minimum
  // mass -- so a typo was reported as "no load" with the real deflection printed
  // beside it, which sends somebody to re-tare a loaded platform.
  float factor = 0.0f;
  const scale::CalResult r = scale::calibrate(grams, &factor);
  const bool ok = (r == scale::CalResult::Ok);

  char msg[48];
  if (ok) snprintf(msg, sizeof(msg), "ok -- %.3f counts/g", factor);
  else snprintf(msg, sizeof(msg), "%s", scale::calResultText(r));

  Serial.printf("ui: calibration %s\n", msg);
  ui::calibSetResult(msg, ok);

  // The keypad now opens with the mass that WORKED rather than the one this
  // board booted with. Without this the page would re-prefill from the
  // boot-time value on its next visit and quietly discard what was just used.
  if (ok) ui::calibSetMass(grams);
}

void onPlatformZero() {
  Serial.println("ui: store platform zero requested");
  scale::setPlatformZero();
}

void onRestoreDefault() {
  Serial.println("ui: restore built-in calibration requested");
  scale::restoreDefault();
  ui::calibSetMass(0.0f);  // re-prefilled from the snapshot on the next entry
}


void onCycleAvg() {
  Serial.printf("ui: averaging -> %u samples\n", scale::cycleWindow());
}

void onCyclePrecision() {
  Serial.printf("ui: reading -> %u decimals\n", scale::cycleDecimals());
}

void onToggleCells() {
  Serial.printf("ui: cells on home -> %s\n", scale::toggleShowCells() ? "shown" : "hidden");
}

// TRIAL HARNESS: which page the device settles on. Persisted here rather than
// in src/ui/, which has no NVS and no business having one -- the UI asks, the
// platform stores, and the value comes back through State::defaultPage.
uint8_t defaultPage_ = 0;

void onCycleDefaultPage() {
  defaultPage_ = (uint8_t)(defaultPage_ ? 0 : 1);
  Preferences p;
  if (p.begin("bowlinput", false)) {
    p.putUChar("defpage", defaultPage_);
    p.end();
  }
  Serial.printf("\n> default page -> %s\n", defaultPage_ ? "Knob" : "Weight");
}

void onClearCal() {
  Serial.println("ui: calibration cleared");
  scale::clearCalibration();
}

// --- backlight ---------------------------------------------------------------
// DIM AFTER 30 s IDLE, ON BATTERY ONLY, AND NOTHING ELSE CHANGES.
//
// The backlight is the only lever here worth pulling. Everything else on this
// board is already either irreducible or paced: the scale task owns core 1 and
// must keep converting, the uplink task owns core 0 and must keep posting, and
// the render loop already sleeps on lv_timer_handler()'s own next-due time --
// 2 ms floor so it always yields, 20 ms ceiling so touch stays under a frame.
// That pacing is what took it from 10% of a core to the 3% ui / 9% loop the
// console reports now, so there is no idle CPU left to reclaim without slowing
// something that is doing work.
//
// SPECIFICALLY NOT DONE, and deliberately: raising that 20 ms ceiling while
// idle. It looks free and is not. The encoder's TURN is decoded in an ISR and
// would not care, but the SWITCH is polled in this loop against a 25 ms
// debounce dwell -- at a 50 ms period a 30 ms tap can fall entirely between two
// samples and be lost. The knob is the wake control and, once deep sleep
// arrives, the only wake source this board has. Trading its reliability for a
// few milliamps of an already-idle core is the wrong side of that bargain.
//
// WHAT THIS DOES NOT TOUCH, because the brief was explicit: WiFi, the Supabase
// posts, and load sensing all run on their own tasks and are not consulted
// here. Dimming the panel cannot reach them.
const uint8_t BL_FULL = 200;  // the boot ramp's target -- one definition of "on"
const uint8_t BL_DIM = 100;   // 50% duty, and PWM duty is very nearly 50% current
const uint32_t BL_DIM_AFTER_MS = 30000;

uint8_t blLevel_ = BL_FULL;

void serviceBacklight() {
  // ONE NOTION OF ACTIVITY FOR THE WHOLE UI. lv_display_get_inactive_time() is
  // already what ui_pages uses to send the screen home after 60 s, and the
  // encoder now feeds the same counter (see serviceInputs), so the knob keeps
  // the screen awake exactly as a fingertip does. A second idle timer here
  // would eventually disagree with that one, and the failure would be a screen
  // that dims while somebody is using it.
  const bool idle = lv_display_get_inactive_time(NULL) > BL_DIM_AFTER_MS;

  // ON BATTERY ONLY. The whole point is cell life, and a mains-fed station has
  // nothing to save -- so it stays readable across the counter all service.
  //
  // A board with no VBUS wire reads false here and therefore dims, which is the
  // right way for this to fail: the unit that cannot tell whether it is on
  // mains is the unit that should assume it is not.
  const bool onBattery = !inputs::externalPower();

  const uint8_t want = (idle && onBattery) ? BL_DIM : BL_FULL;
  if (want == blLevel_) return;  // setBrightness re-programs the LEDC duty

  blLevel_ = want;
  gfx.setBrightness(want);
  Serial.printf("backlight: %s (%u/255) -- %s, %s\n",
                want == BL_FULL ? "full" : "dim",
                (unsigned)want, idle ? "idle" : "active",
                onBattery ? "on battery" : "on mains");
}

// --- panel controls, and what bring-up needs from them ----------------------
// EVERY EVENT PRINTS. Not because the product wants a chatty console, but
// because this is the only way to confirm a hand-soldered joint: turn the knob
// and either a line appears or it does not, and if it does not, the heartbeat
// below says whether the pin is stuck high, stuck low, or moving but decoding
// wrong. Those are three different soldering mistakes and they are
// indistinguishable from the UI.
//
// Silence when nothing is happening. The heartbeat only prints while something
// is UNCONFIRMED, so a board whose controls all work stops talking about them.
bool encoderSeen_ = false;
bool switchSeen_ = false;
// Counts presses for the dashboard's dot. Monotonic and never reset, because
// the UI detects a press by noticing this MOVED -- see State::encoderPressCount
// for why an edge cannot be carried as a bool across a 20 Hz snapshot.
uint32_t pressCount_ = 0;
bool chargeSeen_ = false;
bool vbusSeen_ = false;
uint32_t nextTraceMs_ = 0;

void serviceInputs(uint32_t nowMs) {
  const inputs::Events e = inputs::loop(nowMs);

  // THE KNOB COUNTS AS ACTIVITY, and it has to be said explicitly because LVGL
  // does not know the encoder exists -- it is not registered as an indev, so
  // nothing about turning it would otherwise reset the inactivity counter. A
  // screen that dims and goes home while somebody is turning the knob is the
  // exact complaint, and it would have been the default.
  //
  // Called from this task, which is the one that makes every other lv_* call.
  if (e.turned || e.pressed) lv_display_trigger_activity(NULL);

  if (e.turned) {
    encoderSeen_ = true;
    // The count is printed even when it is 1, because "CW x1" and "CW" read
    // differently when you have just spun the knob a quarter turn and want to
    // know whether the firmware saw four clicks or one.
    Serial.printf("input: encoder %s x%d  -> position %ld\n",
                  e.turned > 0 ? "CW " : "CCW", abs((int)e.turned),
                  (long)inputs::position());
  }
  if (e.pressed) {
    switchSeen_ = true;
    pressCount_++;
    Serial.println("input: switch DOWN");
  }
  if (e.longPress) Serial.println("input: switch LONG press");
  if (e.released) Serial.println("input: switch up");
  // The LED is NOT bound to the switch. It has a real job -- steady on battery,
  // a slow fade on mains -- and borrowing it for bring-up feedback would mean
  // indicator on the unit lies about the power state while somebody is
  // deliberately testing the power state. Plugging USB in confirms both the LED
  // and the VBUS divider in one gesture anyway, which is the better test.

  if (e.chargeChanged) {
    chargeSeen_ = true;
    Serial.printf("input: charge STAT -> %s\n",
                  inputs::charging() ? "CHARGING" : "not charging");
  }
  if (e.externalChanged) {
    vbusSeen_ = true;
    Serial.printf("input: VBUS -> %s\n",
                  inputs::externalPower() ? "5 V PRESENT" : "on battery");
  }

  // TWO SIGNALS, AND THEY ARE ALLOWED TO DISAGREE. Reported together on any
  // change of either, because the interesting state is the combination and
  // reading it off two separate lines printed seconds apart is how the
  // termination case gets misread as a fault.
  if (e.chargeChanged || e.externalChanged) {
    const bool ext = inputs::externalPower(), chg = inputs::charging();
    Serial.printf("       => %s\n",
                  !ext ? "on battery"
                       : chg ? "on mains, charging"
                             : "on mains, charge complete (STAT released)");
  }

  // Every pass, not only on change: the fixture is a snapshot and a dropped
  // update would leave the row showing a stale position after a page rebuild.
  // It costs two stores.
  ui::demoOverrideEncoder(inputs::position(), pressCount_);

  // TRIAL: the manual fill estimate. Same cadence and the same reason -- it is
  // a snapshot, and a value pushed only on change goes stale after a rebuild.
  ui::demoOverrideFill(inputs::fillKnown(), inputs::fillPercent(),
                       inputs::fillReminderDue(), inputs::fillAgeKnown(),
                       inputs::fillAgeKnown() ? inputs::fillAgeMs() / 1000 : 0);
  ui::demoOverrideDefaultPage(defaultPage_);

  // EVERY PASS, and it was not. This lived only in setup(), so `external_` was
  // frozen at whatever VBUS read during boot and no plug event ever reached the
  // screen -- the status bar's bolt and the battery page both sat on a value
  // that could not change. The pin was moving the whole time; nothing was
  // carrying it across.
  //
  // It is cheap: two bools and a flag, compared against nothing. The seeding
  // call in setup() stays, because the boot console line reports what it found.
  ui::demoOverrideCharging(board::CHARGER_STATUS_READABLE, inputs::charging(),
                           inputs::externalPower());

  // The heartbeat, and only while something is still unproven.
  const bool allSeen = encoderSeen_ && switchSeen_ && chargeSeen_ && vbusSeen_;
  if (!allSeen && (int32_t)(nowMs - nextTraceMs_) >= 0) {
    nextTraceMs_ = nowMs + 3000;
    char line[160];
    inputs::traceLine(line, sizeof(line));
    Serial.printf("input: %s   [waiting on:%s%s%s%s]\n", line,
                  encoderSeen_ ? "" : " turn", switchSeen_ ? "" : " press",
                  chargeSeen_ ? "" : " charge", vbusSeen_ ? "" : " vbus");
  }
}

// --- the same three actions, from the console -------------------------------
// A SECOND ROUTE TO THE SAME FUNCTIONS, not a second implementation: every one
// of these calls the identical scale:: entry point the menu row does.
//
// It earns its place because calibration is a two-handed job. You put a known
// mass on the platform and then have to tell the device it is there -- and
// tapping a 2" screen with the hand that is not holding the weight steady is
// how a calibration gets taken mid-wobble. A keystroke on a terminal that is
// already open for the console output is the better hand.
//
// It is also what makes provisioning a fleet scriptable later: 32 units to tare
// and calibrate is 32 keystrokes over USB rather than 32 trips to a menu.
void serviceConsole() {
  while (Serial.available()) {
    const int c = Serial.read();
    switch (c) {
      case 't':
      case 'T':
        Serial.println("\n> tare both");
        scale::tare();
        break;
      // DIGITS RATHER THAN LETTERS, and that is the third cell's doing. Per-cell
      // tare was 'a' and 'b'; the obvious extension is 'c', which is already
      // calibrate -- and quietly rebinding calibrate to something else on a
      // console people have muscle memory for is worse than moving the cells.
      // 1/2/3 also extends past the third cell without another collision.
      case '1':
      case '2':
      case '3': {
        const uint8_t idx = (uint8_t)(c - '1');
        if (idx >= scale::CELLS) break;
        Serial.printf("\n> tare cell %c\n", (char)('A' + idx));
        scale::tareCell(idx);
        break;
      }
      case 'c':
      case 'C':
        // The STORED mass -- the same one the keypad opens with -- so the
        // console shortcut and the on-screen page calibrate against the same
        // thing. To use a different weight, type it on the page.
        Serial.printf("\n> calibrate against the stored %.0f g\n", scale::calMass());
        onCalibApply(scale::calMass());
        break;
      case 's':
      case 'S':
        // Queued, not run here -- see scale::requestSelfTest(). The reply comes
        // back on this console a few seconds later, from the scale task.
        Serial.println("\n> self-test queued (measurement pauses for ~15 s)");
        scale::requestSelfTest();
        break;
      case 'x':
      case 'X':
        Serial.println("\n> clear calibration");
        scale::clearCalibration();
        break;
      case 'i':
      case 'I':
        inputs::dumpState();
        break;
      case 'l':
      case 'L':
        // Cycles rather than toggles, and always lands back on Auto, so a
        // bring-up session cannot leave the indicator permanently lying.
        inputs::setLedMode(inputs::ledMode() == inputs::LedMode::Auto
                               ? inputs::LedMode::ForceOn
                           : inputs::ledMode() == inputs::LedMode::ForceOn
                               ? inputs::LedMode::ForceOff
                               : inputs::LedMode::Auto);
        Serial.printf("\n> status LED -> %s\n", inputs::ledModeName());
        break;
      case 'w':
      case 'W':
        Serial.printf("\n> averaging -> %u samples\n", scale::cycleWindow());
        break;
      case 'd':
      case 'D':
        Serial.printf("\n> reading -> %u decimals\n", scale::cycleDecimals());
        break;
      case '?':
        Serial.println(
            "\n  t      tare EVERY cell at whatever is on the platform NOW\n"
            "  1 2 3  tare cell A / B / C only\n"
            "  c      calibrate against the STORED mass -- Settings > Scale >\n"
            "         Calibrate on the panel to type a different one\n"
            "  s      full converter self-test: bridge vs shorted vs channel 2,\n"
            "         per cell. This is what tells a dead bridge apart from a\n"
            "         dead converter -- both look like a cell that reads wrong.\n"
            "         Pauses measurement for ~15 s at 10 SPS.\n"
            "  x      clear the calibration and go back to counts\n"
            "  i      dump the panel controls: raw pin levels, encoder\n"
            "         position, decoder health, charge and VBUS state\n"
            "  l      status LED: auto -> forced on -> forced off -> auto.\n"
            "         Auto is steady on battery, a slow fade on mains, and\n"
            "         5 Hz once the fill estimate is stale. Steady is what\n"
            "         says the battery switch is still on when the display\n"
            "         has blanked\n"
            "  w      step the moving average 8 -> 16 -> 32 -> 64 -> 128 -> 8\n"
            "  d      step the reading 0.0 -> 0.00 -> 0.000 kg -> 0.0 (display\n"
            "         only; Diagnose keeps all three places whatever this says)\n"
            "\n  Order matters: tare on an EMPTY platform, then put the mass on,\n"
            "  wait for the reading to settle, then calibrate.\n"
            "\n  1/2/3 are the SETUP tools: zeroing one corner against the others\n"
            "  is how you tell an uneven mounting from an uneven set of cells,\n"
            "  which the total -- being a sum -- cannot show you. On three cells\n"
            "  that is also how you find the corner the platform is not sitting\n"
            "  on, which is the fault three cells were fitted to remove.\n");
        break;
      default:
        break;  // newlines and stray bytes from a terminal are not errors
    }
  }
}

}  // namespace

void setup() {
  bootStartMs_ = bootPhaseMs_ = millis();
  Serial.begin(115200);
  // USB-CDC enumerates only when a host opens the port, so the first lines are
  // lost without this. Bounded, not a spin: a unit on battery has no host and
  // must still boot.
  const uint32_t deadline = millis() + CONSOLE_WAIT_MS;
  while (!Serial && (int32_t)(millis() - deadline) < 0) delay(10);
  bootMark("console wait");

  Serial.println("\n=== Bowlstack :: load-cell station ===");
  Serial.printf("  device      %s, fw %s\n", BOWLSTACK_DEVICE_ID, BOWLSTACK_FW_VERSION);
  Serial.printf("  chip        %s rev %d, %d MHz, %d core(s)\n", ESP.getChipModel(),
                ESP.getChipRevision(), getCpuFrequencyMhz(), ESP.getChipCores());
  Serial.printf("  PSRAM       %u bytes free of %u\n", ESP.getFreePsram(), ESP.getPsramSize());
  Serial.printf("  heap        %u bytes free\n", ESP.getFreeHeap());
  if (ESP.getPsramSize() == 0) {
    Serial.println("  !! PSRAM NOT DETECTED -- check board_build.arduino.memory_type = qio_opi");
  }

  // --- I2C idle levels, BEFORE anything claims the pins --------------------
  // Read as plain GPIO, and read FIRST, because this is the one measurement
  // that separates "nothing is on that bus" from "something is holding it
  // down". A scan cannot tell those apart -- both look like silence -- and the
  // difference is the difference between a missing module and a wedged one.
  //
  // All four lines should read 1, and on this wiring every one of them is held
  // up by a REAL resistor rather than by the ESP32's internal pull-up: the touch
  // pair by R29/R30 and the cell trunk by R4/R5, the camera connector's, both
  // 4.7k and both already fitted. A 0 is therefore a slave holding the line
  // down, not a missing resistor.
  //
  // This paragraph described "bus A" and a pull-up-less "bus B" on GPIO11/12
  // until the cells moved behind the mux. That bus no longer exists, and its
  // caveat -- a 1 that is only the internal pull-up, so weak that 400 kHz NAKed
  // -- no longer applies to anything printed below.
  {
    const int8_t pins[4] = {board::TP_SDA, board::TP_SCL, board::CELL_SDA, board::CELL_SCL};
    for (uint8_t i = 0; i < 4; i++) pinMode(pins[i], INPUT_PULLUP);
    delayMicroseconds(200);
    Serial.printf(
        "  i2c idle    touch  SDA(%d)=%d SCL(%d)=%d   cells  SDA(%d)=%d SCL(%d)=%d\n", pins[0],
        digitalRead(pins[0]), pins[1], digitalRead(pins[1]), pins[2], digitalRead(pins[2]),
        pins[3], digitalRead(pins[3]));
    if (!digitalRead(pins[0]) || !digitalRead(pins[1])) {
      Serial.println("  !! THE TOUCH BUS IS HELD LOW, so the screen will not respond either.");
      Serial.println("     Nothing this branch adds lives on GPIO47/48 any more -- the cells");
      Serial.println("     moved behind the mux on GPIO12/11 -- so suspect the panel cable.");
    }
    if (!digitalRead(pins[2]) || !digitalRead(pins[3])) {
      // ONE STUB CAN DO THIS THROUGH A CLOSED CHANNEL ONLY IF THE MUX IS ALSO
      // WEDGED. Normally a stuck stub is invisible from the trunk, which is the
      // whole point of the switch -- so a trunk held low is the mux itself, or
      // something wired past it.
      Serial.println("  !! THE CELL TRUNK IS HELD LOW. GPIO12/11 carry the mux and nothing");
      Serial.println("     else, so this is the mux, its power, or a converter wired");
      Serial.println("     directly to the trunk instead of to a channel.");
    }
  }

  // --- the panel, before LVGL exists --------------------------------------
  // Still brought up before the cells, though no longer because it has to be.
  // Cell A used to ride the touch controller's port, which gfx.init() opens, so
  // reversing these two lines had the converter talking to a bus nobody had
  // configured. The cells own port 1 now and scale::begin() opens it itself.
  // The order stays because the splash should be on the screen while the
  // converters spend their second each self-testing.
  Serial.println("\n--- display ---");
  bootMark("banner + i2c idle read");
  gfx.init();
  gfx.setRotation(0);    // 0 = portrait 240x320, connector at the bottom
  gfx.setBrightness(0);  // ramp up rather than flashing white at boot
  gfx.fillScreen(TFT_BLACK);
  bootMark("gfx.init + clear");
  for (uint8_t b = 0; b <= 200; b += 5) {
    gfx.setBrightness(b);
    delay(4);
  }
  bootMark("backlight ramp");
  Serial.printf("  panel %dx%d, touch %s\n", gfx.width(), gfx.height(),
                gfx.touch() ? "registered" : "NOT REGISTERED");
  bootSplash();
  // FROM HERE UNTIL buildPages() THE SPLASH IS WHAT THE OPERATOR IS LOOKING AT.
  // Every phase below is logo time, which is the only reason to time them.
  bootMark("display + backlight ramp + splash");

  // --- the cells ----------------------------------------------------------
  // Before the UI task exists, so the first transaction on each bus -- which is
  // what creates lgfx's per-port mutex -- happens while this is still the only
  // thread. After this point the scale task and the touch reads share port 0
  // under that mutex.
  scale::begin();
  bootMark("load cells");

  // --- lvgl ---------------------------------------------------------------
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
    return;  // loop() checks uiReady_ and will not touch LVGL
  }
  Serial.printf("  draw buffers 2 x %u bytes = %u lines (DMA-capable)\n",
                (unsigned)LV_BUF_BYTES, (unsigned)LV_BUF_LINES);

  lv_display_t *disp = lv_display_create(board::LCD_W, board::LCD_H);
  lv_display_set_flush_cb(disp, flushCb);
  lv_display_set_buffers(disp, buf1, buf2, LV_BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchCb);
  uiReady_ = true;

  // --- the shared UI ------------------------------------------------------
  // The REAL MAC, read from the eFuse. It needs no WiFi stack, and it is
  // precisely what someone diagnosing a join failure asks for first.
  {
    uint8_t m[6];
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    static char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3],
             m[4], m[5]);
    ui::wifiSetMac(macStr);
    static char apSsid[33];
    snprintf(apSsid, sizeof(apSsid), "Bowlstack-%s", BOWLSTACK_DEVICE_ID);
    ui::wifiSetSetupAp(apSsid, "bowlstack");
    Serial.printf("  mac %s\n", macStr);
  }

  bootMark("lvgl init + draw buffers + display/indev");

  bringup_wifi::begin();
  bringup_time::begin();
  scale_telemetry::begin();
  startUplinkTask();
  bootMark("wifi + time + uplink tasks started");

  ui::perfBegin();

  // The scope's canvas lives in PSRAM. ~88 KB has no business in LVGL's 96 KB
  // pool or in the internal SRAM that WiFi and TLS will want; and because the
  // sweep invalidates one column per sample, LVGL reads only that column back
  // out per frame, so PSRAM's slower access never lands on the hot path.
  {
    void *scopeBuf = heap_caps_malloc(ui::SCOPE_BUF_BYTES, MALLOC_CAP_SPIRAM);
    if (!scopeBuf) Serial.println("  !! scope canvas alloc FAILED - page will say so");
    else Serial.printf("  scope canvas %u bytes in PSRAM\n", (unsigned)ui::SCOPE_BUF_BYTES);
    ui::scopeSetBuffer(scopeBuf);
  }

  // THE ONLY TARE PATH. There were two -- this and ui::weightOnTare(onTareBoth)
  // for the dashboard button -- and the button is gone, so the second install
  // and its handler went with it rather than being left dangling off a widget
  // nothing creates.
  ui::pagesOnScaleTare(onTare);
  ui::pagesOnScaleClearCal(onClearCal);
  ui::pagesOnScaleCycleAvg(onCycleAvg);
  ui::pagesOnScaleCyclePrecision(onCyclePrecision);
  ui::pagesOnScaleToggleCells(onToggleCells);
  ui::pagesOnScaleRestore(onRestoreDefault);
  ui::pagesOnScalePlatformZero(onPlatformZero);
  ui::calibOnApply(onCalibApply);
  // Opens pre-filled with whatever this unit was last calibrated against, which
  // on a fresh board is the build default.
  ui::calibSetMass(scale::calMass());

  // WHICH PAGE TO OPEN ON, READ BEFORE THE PAGES EXIST.
  //
  // This used to sit with the other panel-control setup, thirty lines further
  // down -- which is AFTER buildPages(), and buildPages is what chooses the
  // first tile. So the setting persisted perfectly and the device ignored it:
  // every boot opened on the weight page, and the menu row underneath cheerfully
  // read "Knob".
  //
  // The give-away was that the idle timeout and the home button DID honour it,
  // because both run later and read the value through State. Only the very
  // first tile was wrong, which is the one moment nothing has re-read anything
  // yet.
  {
    // Read-write to open it, for the same reason inputs.cpp does: a namespace
    // that has never been written does not exist, and a read-only open of one
    // fails and logs an error that looks exactly like corruption.
    Preferences p;
    if (p.begin("bowlinput", false)) {
      defaultPage_ = p.getUChar("defpage", 0);
      p.end();
    }
    if (defaultPage_ > 1) defaultPage_ = 0;
  }
  ui::demoOverrideDefaultPage(defaultPage_);

  ui::buildPages();
  bootMark("scope canvas + buildPages -- SPLASH ENDS HERE");
  Serial.println(
      "  console: t = tare all, 1/2/3 = tare one, c = calibrate, x = clear,\n"
      "           s = self-test, w = average, d = decimals, ? = help");
  Serial.println("  home = weight; gear button (or swipe LEFT) for the menu,");
  Serial.println("  house button at the same spot to come back:");
  Serial.println("    Settings -> WiFi, Battery, Scale (tare / calibrate)");
  Serial.println("    Sensors  -> live cell scope, three traces + frame rate");

  // --- panel controls -------------------------------------------------------
  // AFTER the panel and the cells, because those two own pins and this must not
  // be the thing that discovers a collision. Before the battery block, so the
  // charge-sense line is already being read when the battery block prints what
  // it thinks of the charger.
  inputs::begin();
  ui::pagesOnCycleDefaultPage(onCycleDefaultPage);
  Serial.printf("  TRIAL    default page is %s -- Menu > Settings > Default page\n",
                defaultPage_ ? "Knob" : "Weight");
  Serial.printf("  backlight dims to %u%% after %lu s idle, ON BATTERY ONLY\n",
                (unsigned)((BL_DIM * 100) / BL_FULL),
                (unsigned long)(BL_DIM_AFTER_MS / 1000));
  bootMark("inputs");

  // --- battery ------------------------------------------------------------
  analogSetPinAttenuation(board::PIN_BATTERY_ADC, ADC_11db);
  Serial.println("\n--- battery ---");
  // BOTH RATIOS, side by side, because they answer different questions and a
  // gap between them is itself the diagnosis. The nominal is what the resistors
  // are; the calibration is what this unit measures, and it is the one actually
  // applied. A unit whose calibration has drifted far from 3.0 has a divider
  // fault or an ADC well outside its usual few per cent, and printing only the
  // figure in use would hide that.
  Serial.printf("  GPIO%d, on-board 200k/100k divider (nominal %.2f, using %.4f)\n",
                board::PIN_BATTERY_ADC, board::BATTERY_DIVIDER_NOMINAL,
                config::BATTERY_DIVIDER);

  // THE CHARGER IS NOT WIRED TO ANYTHING THIS CHIP CAN READ, and the firmware
  // says so rather than letting a fixture answer for it. The ETA6098's STAT
  // output drives LED1's cathode and that net carries no module pin -- see
  // board_waveshare_s3.h section 5 -- so `unknown` is the measurement, not a
  // placeholder. Fitting the one-resistor mod in todo.md is what makes
  // CHARGER_STATUS_READABLE true, and this line is then the only one that has to
  // learn where the pin went.
  ui::demoOverrideCharging(board::CHARGER_STATUS_READABLE, inputs::charging(),
                           inputs::externalPower());
  Serial.printf("  charge state %s\n",
                board::CHARGER_STATUS_READABLE
                    ? "readable -- STAT mod fitted"
                    : "NOT DECLARED READABLE -- sensed and printed, but reported "
                      "as unknown rather than as 'no'");

  // One reading before the first loop, so the boot log carries a number rather
  // than leaving the first power line 100 ms away and the panel showing the
  // fixture until then.
  {
    const uint16_t pinMv = readBatteryPinMv();
    const uint16_t cellMv = (uint16_t)lroundf((float)pinMv * config::BATTERY_DIVIDER);
    Serial.printf("  pin %u mV -> cell %u mV\n", pinMv, cellMv);
    if (cellMv < config::BATTERY_PRESENT_ABOVE_MV) {
      // Named as the two different faults it can be. An open input and a wrong
      // divider constant both land here, and they send somebody to opposite
      // ends of the board.
      Serial.println("  !! below the presence threshold -- either no cell is fitted,");
      Serial.println("     or BOWLSTACK_BATTERY_CAL is wrong for this hardware.");
    }
  }
  bootMark("battery");
  Serial.println("\nready.\n");
}

void loop() {
  if (!uiReady_) {
    delay(100);
    return;
  }

  // BRACKETS THE WHOLE ITERATION, not just LVGL, so busy% covers everything the
  // loop does. Quoting a figure that excluded the radio and the adapter as
  // evidence that the board has headroom would be measuring neither.
  ui::perfFrameStart(millis());
  ui::perfUiStart(millis());
  // lv_timer_handler RETURNS how long until it next needs calling, and that
  // return value is the difference between 10% of a core and ~1%. Called
  // blindly every 5 ms it ran ~200 times a second, walking its timer list and
  // scanning for invalid areas each time -- measured at 10% busy while drawing
  // ZERO pixels.
  const uint32_t nextMs = lv_timer_handler();
  ui::perfUiEnd(millis());

  bringup_wifi::loop(millis());
  bringup_time::publish();
  serviceConsole();

  // EVERY PASS, not on a timer. The encoder's decode is already in an ISR, so
  // what happens here is draining an int and reading four pins -- microseconds
  // -- and rate-limiting it would only add latency to the one input a person is
  // physically waiting on.
  serviceInputs(millis());

  // After serviceInputs, so a turn or a press taken on this pass has already
  // reset the inactivity counter and the screen comes back on the same frame
  // rather than the next one.
  serviceBacklight();

  // 20 Hz, matching what the scale task publishes at. Faster would copy the
  // same snapshot repeatedly under a mutex the measuring task wants back.
  static uint32_t nextScale = 0;
  if ((int32_t)(millis() - nextScale) >= 0) {
    nextScale = millis() + 50;
    publishScale(millis());
  }

  ui::pagesTick(millis());
  ui::perfFrameEnd(millis());

  // 10 Hz, not 1 Hz, and the rate is part of the filter rather than a sampling
  // preference. BATTERY_EMA_ALPHA is expressed in samples, so config.h's 0.20
  // is the ~500 ms time constant it claims to be only at this interval; at the
  // 1 Hz this used to run at, the same constant was a five-second filter.
  //
  // It costs ~1.6 ms of ADC conversions per sample -- 16 reads at ~100 us --
  // against a loop measured at ui 62%. That is the price of the presence dwell
  // being able to see a contact bounce at all.
  {
    const uint32_t now = millis();
    if (!batteryEverSampled_ || (int32_t)(now - nextBatterySampleMs_) >= 0) {
      nextBatterySampleMs_ = now + config::BATTERY_SAMPLE_INTERVAL_MS;
      batteryEverSampled_ = true;

      const uint16_t pinMv = readBatteryPinMv();
      // ONE divider constant for the image. config::BATTERY_DIVIDER is what
      // platformio.ini's -DBOWLSTACK_BATTERY_CAL sets, and it is the per-unit
      // calibrated figure rather than board::BATTERY_DIVIDER_NOMINAL -- the
      // nominal is what the resistors say, this is what this board measures.
      // Using both would put two answers to one question in one file.
      const uint16_t cellMv =
          (uint16_t)lroundf((float)pinMv * config::BATTERY_DIVIDER);

      // Presence, plausibility, filtering, hysteresis and the percentage
      // deadband all happen inside here. The bounds this used to test inline
      // (2500 / 4400) are Monitor's own, with the hysteresis partners the
      // inline version dropped -- see config.h's BATTERY_*_MV block.
      batteryMonitor_.update(cellMv, now);

      lastPinMv_ = pinMv;
      lastCellMv_ = batteryMonitor_.millivolts();
      ui::demoOverrideBattery(batteryMonitor_.millivolts(), pinMv,
                              batteryMonitor_.percent(),
                              toUiBattery(batteryMonitor_.level()));
      // The same two figures the screen just got, handed across to the uplink
      // task. Published from here rather than read from lastCellMv_ directly,
      // so the cross-core reader gets one consistent pair.
      publishPower(batteryMonitor_.millivolts(), batteryMonitor_.level());
    }
  }

  static uint32_t nextConsole = 0;
  const uint32_t now = millis();
  if ((int32_t)(now - nextConsole) >= 0) {
    nextConsole = now + 5000;
    // THE LINE THAT ANSWERS "IS THE INTERFACE WORKING". Raw counts per cell,
    // the rate each is actually delivering, and the resulting total -- in
    // grams if the assembly has been calibrated and in counts if it has not,
    // stated either way rather than left to be inferred.
    const scale::Snapshot sn = scale::snapshot();
    char perf[128];
    ui::perfFormat(perf, sizeof(perf));
    Serial.printf("%s | esp-heap %u\n", perf, ESP.getFreeHeap());

    // THE POWER LINE, and it prints exactly what the Battery page renders --
    // pin voltage, cell voltage, percentage, band -- so the two can be compared
    // at a glance instead of one of them having to be trusted. The discrete
    // product has carried this since the beginning (device_status::printPower)
    // and it is the only power telemetry visible without a network; this image
    // was printing a bare millivolt figure and nothing else.
    //
    // IT IS ALSO HOW YOU TELL A WIRED READING FROM A STUCK ONE. On USB the
    // ETA6098 holds the BAT node at its charge voltage, so a board with no cell
    // fitted -- and a board with a full one -- both sit at ~4.16 V and do not
    // move. That is a real measurement of a real node and it looks exactly like
    // a fixture. Unplug USB and the number starts falling; that is the test.
    if (batteryMonitor_.level() == battery::Level::Unknown) {
      // The two ways a reading can be invalid, named apart. Too low is an open
      // or unconnected input; too high means the divider constant is wrong or
      // the pin is floating -- and they send somebody to opposite ends of the
      // board.
      Serial.printf("  battery: pin %u mV -> cell %u mV : %s\n", lastPinMv_, lastCellMv_,
                    lastCellMv_ >= config::BATTERY_PRESENT_ABOVE_MV
                        ? "IMPLAUSIBLE -- check the divider or BOWLSTACK_BATTERY_CAL"
                        : "no cell detected");
    } else {
      Serial.printf("  battery: pin %u mV -> cell %u mV : %d%% -> %s   charging %s\n",
                    lastPinMv_, lastCellMv_, batteryMonitor_.percent(),
                    battery::levelName(batteryMonitor_.level()),
                    board::CHARGER_STATUS_READABLE ? "?" : "unknown (no STAT pin)");
      // END TO END, because "the bolt does not light" has three possible
      // causes and only one of them is the pin. vbus is what the GPIO reads,
      // ext is what inputs:: decided after debouncing, and ui.ext is what the
      // status bar was actually handed -- if those three disagree, the answer
      // is in the gap between whichever two.
      Serial.printf("  power:   vbus pin %d, ext %d, ui.ext %d, chargingKnown %d\n",
                    digitalRead(board::PIN_VBUS_SENSE),
                    inputs::externalPower() ? 1 : 0,
                    ui::demoLatest(millis()).externalPower ? 1 : 0,
                    board::CHARGER_STATUS_READABLE ? 1 : 0);
    }
    for (uint8_t i = 0; i < scale::CELLS; i++) {
      const scale::CellSnapshot &c = sn.cell[i];
      const char *st = c.state == CellState::Online
                           ? "online"
                           : (c.state == CellState::Warming ? "warming" : "OFFLINE");
      // p-p IS PRINTED BESIDE THE MEAN ON PURPOSE. The mean is a trimmed
      // average and is meant to look calm, so a cell with a disconnected
      // bridge and a cell sitting perfectly still print the same steady
      // number. The peak-to-peak is what tells them apart: a live 350 ohm
      // bridge at gain 128 wanders by tens to hundreds of counts between
      // conversions, and an open input does not move at all.
      Serial.printf(
          "  cell %c  %-7s rev 0x%02X  raw %8ld  filt %8ld  p-p %6ld  zero %8ld  tare %8ld  "
          "%u/s (%u)\n",
          'A' + i, st, c.revision, (long)c.rawCounts, (long)c.counts, (long)c.pp,
          (long)c.platformZero, (long)c.tare, c.sps, c.samples);
    }
    if (sn.calibrated) {
      // ALWAYS THREE PLACES HERE, WHATEVER THE PANEL IS SET TO -- and the
      // parenthetical says which setting the panel is on, so the two can be
      // reconciled instead of merely looking inconsistent.
      //
      // This used to claim it formatted the number "the same way the screen
      // does, so the console and the panel cannot disagree about the last
      // digit". That stopped being true the moment precision became a setting,
      // and the console is the wrong place to follow it: it is a diagnostic
      // channel, like the Diagnose page, read by somebody who wants the figure
      // the display is rounding rather than the rounded one. Truncating the
      // evidence to match the dashboard would remove the only view that can
      // show why the dashboard reads as it does.
      //
      // Still rounded to whole grams and split with integers rather than
      // printed with "%.3f": "%.0f" of a value a fraction below zero prints
      // "-0", which reads as a bug rather than as a tared platform sitting a
      // few tenths of a gram low.
      const long mg = (long)lroundf(sn.totalGrams);
      const long amg = mg < 0 ? -mg : mg;
      Serial.printf("  total  %s%ld.%03ld kg   (%.3f counts/g, %u-sample window, panel 0.%0*d)\n",
                    mg < 0 ? "-" : "", amg / 1000L, amg % 1000L, sn.countsPerGram, sn.window,
                    (int)(sn.decimals ? sn.decimals : 3), 0);
    } else {
      // SUMMED OVER scale::CELLS, AND ONLY THE ONLINE ONES -- the same rule
      // publishScale() uses for the panel and scale_telemetry::netCounts() uses
      // for net_counts, so the three cannot disagree about the same instant.
      // This was written as cell[0]+cell[1] literally in the two-cell era and
      // survived the move to three, which made a perfectly good corner C look
      // like a wiring fault: pressing it moved the panel and the per-cell line
      // above and left the console total alone.
      int32_t net = 0;
      for (uint8_t i = 0; i < scale::CELLS; i++) {
        const scale::CellSnapshot &c = sn.cell[i];
        if (c.state != CellState::Online) continue;
        net += c.counts - c.platformZero - c.tare;
      }
      Serial.printf("  total  %ld counts   UNCALIBRATED -- Menu > Settings > Scale\n", (long)net);
    }
    // THE UPLINK LINE. A station that weighs perfectly and publishes nothing
    // looks, from the panel, exactly like one doing both -- and the dashboard
    // is the only place the difference shows, which is precisely where nobody
    // standing at the device is looking.
    //
    // The post count is what separates "never worked" from "worked and
    // stopped", and the age of the last one is what says which. Both are
    // useless as a rate and are not printed as one.
    {
      const uint32_t lp = scale_telemetry::lastPostMs();
      char age[24];
      if (scale_telemetry::posts() == 0) snprintf(age, sizeof(age), "never");
      else snprintf(age, sizeof(age), "%lus ago", (unsigned long)((now - lp) / 1000));
      Serial.printf("  uplink: %s  %lu state, %lu samples, %u queued, last %s%s\n",
                    bringup_wifi::connected() ? "online" : "OFFLINE (buffering history)",
                    (unsigned long)scale_telemetry::posts(),
                    (unsigned long)scale_telemetry::samplesPosted(),
                    scale_telemetry::queued(), age,
                    scale_telemetry::unprovisioned()
                        ? "  !! NOT REGISTERED -- run supabase/register_loadcells.sql"
                        : "");
    }

    Serial.printf("  stacks: scale %lu B free, uplink %lu B free\n",
                  (unsigned long)scale::stackFreeBytes(),
                  (unsigned long)uplinkStackFreeBytes());
  }

  // Sleep until LVGL actually wants attention, bounded at both ends: at least
  // 2 ms so the loop always yields to the WiFi, time and scale tasks, at most
  // 20 ms so touch latency stays under a frame. LV_NO_TIMER_READY comes back
  // when there is nothing pending at all.
  uint32_t sleepMs = (nextMs == LV_NO_TIMER_READY) ? 20 : nextMs;
  if (sleepMs < 2) sleepMs = 2;
  if (sleepMs > 20) sleepMs = 20;
  delay(sleepMs);
}
