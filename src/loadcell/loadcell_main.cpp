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
// The WiFi and NTP halves are the bring-up harness's, unmodified: this image
// links src/bringup/bringup_wifi.cpp and bringup_time.cpp rather than copies of
// them. Only the measurement is new.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <lvgl.h>
#include <math.h>

#include <esp_mac.h>

#include "bringup_time.h"
#include "bringup_wifi.h"

#include "battery_soc.h"
#include "board_waveshare_s3.h"
#include "lgfx_waveshare_s3.h"
#include "logo128.h"
#include "scale.h"
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
          "   -- the cells moved behind the mux on GPIO21/16 -- so unlike the two-cell\n"
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

void onTareBoth() {
  Serial.println("ui: tare (dashboard button)");
  scale::tare();
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

void onClearCal() {
  Serial.println("ui: calibration cleared");
  scale::clearCalibration();
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
  // Both lines should read 1. Bus A has 4.7k pull-ups fitted on the board
  // (R29/R30); bus B has none, so a 1 there is the ESP32's internal pull-up if
  // you have not fitted the resistors, which is weak but still reads high.
  // A 0 on SDA is a slave holding the bus, and it takes the touch controller
  // down with it.
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
      Serial.println("     moved behind the mux on GPIO21/16 -- so suspect the panel cable.");
    }
    if (!digitalRead(pins[2]) || !digitalRead(pins[3])) {
      // ONE STUB CAN DO THIS THROUGH A CLOSED CHANNEL ONLY IF THE MUX IS ALSO
      // WEDGED. Normally a stuck stub is invisible from the trunk, which is the
      // whole point of the switch -- so a trunk held low is the mux itself, or
      // something wired past it.
      Serial.println("  !! THE CELL TRUNK IS HELD LOW. GPIO21/16 carry the mux and nothing");
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
  bootMark("wifi + time tasks started");

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

  ui::pagesOnScaleTare(onTare);
  ui::weightOnTare(onTareBoth);
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

  ui::buildPages();
  bootMark("scope canvas + buildPages -- SPLASH ENDS HERE");
  Serial.println(
      "  console: t = tare all, 1/2/3 = tare one, c = calibrate, x = clear,\n"
      "           s = self-test, w = average, d = decimals, ? = help");
  Serial.println("  home = weight; gear button (or swipe LEFT) for the menu,");
  Serial.println("  house button at the same spot to come back:");
  Serial.println("    Settings -> WiFi, Battery, Scale (tare / calibrate)");
  Serial.println("    Sensors  -> live cell scope, three traces + frame rate");

  // --- battery ------------------------------------------------------------
  analogSetPinAttenuation(board::PIN_BATTERY_ADC, ADC_11db);
  Serial.println("\n--- battery ---");
  Serial.printf("  GPIO%d, on-board 200k/100k divider (ratio %.2f)\n",
                board::PIN_BATTERY_ADC, board::BATTERY_DIVIDER_NOMINAL);
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

  // 20 Hz, matching what the scale task publishes at. Faster would copy the
  // same snapshot repeatedly under a mutex the measuring task wants back.
  static uint32_t nextScale = 0;
  if ((int32_t)(millis() - nextScale) >= 0) {
    nextScale = millis() + 50;
    publishScale(millis());
  }

  ui::pagesTick(millis());
  ui::perfFrameEnd(millis());

  // Once per second, not per frame. The 16-sample mean costs 16 ADC
  // conversions, a cell moves over hours, and the battery page is the only
  // thing that reads it.
  static uint32_t nextBatt = 0;
  if ((int32_t)(millis() - nextBatt) >= 0) {
    nextBatt = millis() + 1000;
    const uint16_t pinMv = readBatteryPinMv();
    const uint16_t cellMv = (uint16_t)(pinMv * board::BATTERY_DIVIDER_NOMINAL);

    // Bounded at BOTH ends. Below the floor the input is floating rather than
    // measuring; above the ceiling no lithium cell can produce it, so the
    // measurement is broken -- a real failure once read 6365 mV as "100%".
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
    lastPinMv_ = pinMv;
    lastCellMv_ = cellMv;
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
    Serial.printf("%s | esp-heap %u | batt %u mV\n", perf, ESP.getFreeHeap(), lastCellMv_);
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
      Serial.printf("  total  %ld counts   UNCALIBRATED -- Menu > Settings > Scale\n",
                    (long)(sn.cell[0].counts - sn.cell[0].platformZero - sn.cell[0].tare +
                           sn.cell[1].counts - sn.cell[1].platformZero - sn.cell[1].tare));
    }
    Serial.printf("  stacks: scale %lu B free\n", (unsigned long)scale::stackFreeBytes());
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
