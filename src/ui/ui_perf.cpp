#include "ui_perf.h"

#include <lvgl.h>
#include <stdio.h>

namespace ui {
namespace {

volatile uint32_t frames_ = 0;
volatile uint32_t flushPx_ = 0;
volatile uint32_t flushes_ = 0;
uint32_t pxPerFrame_ = 0;
uint16_t flushPerFrame_ = 0;
uint32_t windowStart_ = 0;
uint32_t busyAccum_ = 0;
uint32_t uiAccum_ = 0;
uint32_t uiEnter_ = 0;
uint8_t uiPct_ = 0;
uint32_t frameEnter_ = 0;
uint16_t worstMs_ = 0;

uint16_t fps_ = 0;
uint8_t busyPct_ = 0;
uint16_t worstReported_ = 0;
uint32_t memUsed_ = 0, memTotal_ = 0;
uint8_t memFrag_ = 0;

bool armed_ = false;

void onRefrReady(lv_event_t *) { frames_++; }

}  // namespace

void perfBegin() {
  lv_display_t *d = lv_display_get_default();
  if (d) lv_display_add_event_cb(d, onRefrReady, LV_EVENT_REFR_READY, nullptr);
}

void perfFlush(uint32_t px) {
  flushPx_ += px;
  flushes_++;
}

void perfUiStart(uint32_t nowMs) { uiEnter_ = nowMs; }
void perfUiEnd(uint32_t nowMs) { uiAccum_ += nowMs - uiEnter_; }

void perfFrameStart(uint32_t nowMs) { frameEnter_ = nowMs; }

void perfFrameEnd(uint32_t nowMs) {
  const uint32_t dt = nowMs - frameEnter_;
  busyAccum_ += dt;
  if (dt > worstMs_) worstMs_ = (uint16_t)dt;
}

bool perfTick(uint32_t nowMs) {
  if (!armed_) {
    armed_ = true;
    windowStart_ = nowMs;
    return false;
  }
  const uint32_t elapsed = nowMs - windowStart_;
  if (elapsed < 1000) return false;

  fps_ = (uint16_t)((frames_ * 1000UL) / elapsed);
  busyPct_ = (uint8_t)((busyAccum_ * 100UL) / elapsed);
  uiPct_ = (uint8_t)((uiAccum_ * 100UL) / elapsed);
  if (uiPct_ > 100) uiPct_ = 100;
  if (busyPct_ > 100) busyPct_ = 100;
  worstReported_ = worstMs_;

  pxPerFrame_ = frames_ ? (flushPx_ / frames_) : flushPx_;
  flushPerFrame_ = (uint16_t)(frames_ ? (flushes_ / frames_) : flushes_);
  flushPx_ = 0;
  flushes_ = 0;

  frames_ = 0;
  busyAccum_ = 0;
  uiAccum_ = 0;
  worstMs_ = 0;
  windowStart_ = nowMs;

  lv_mem_monitor_t m;
  lv_mem_monitor(&m);
  memTotal_ = m.total_size;
  memUsed_ = m.total_size - m.free_size;
  memFrag_ = m.frag_pct;
  return true;
}

uint16_t perfFps() { return fps_; }
uint8_t perfBusyPct() { return busyPct_; }
uint8_t perfUiPct() { return uiPct_; }
uint16_t perfWorstMs() { return worstReported_; }
uint32_t perfPxPerFrame() { return pxPerFrame_; }
uint16_t perfFlushesPerFrame() { return flushPerFrame_; }

void perfFormatShort(char *buf, uint32_t len) {
  snprintf(buf, len, "fps %u  ui %u%%  worst %ums", fps_, uiPct_, worstReported_);
}

void perfFormat(char *buf, uint32_t len) {
  // SPI time is derivable from the pixel count: 2 bytes per pixel, 8 bits per
  // byte, 40 MHz. Printing it beside the frame time says immediately whether a
  // slow frame is the bus or the renderer.
  const uint32_t spiUs = (pxPerFrame_ * 2UL * 8UL) / 40UL;
  snprintf(buf, len,
           "fps %u ui %u%% loop %u%% worst %ums | %lupx/f %uflush spi~%lums | lvgl %luk/%luk frag %u%%",
           fps_, uiPct_, busyPct_, worstReported_, (unsigned long)pxPerFrame_, flushPerFrame_,
           (unsigned long)(spiUs / 1000), (unsigned long)(memUsed_ / 1024),
           (unsigned long)(memTotal_ / 1024), memFrag_);
}

}  // namespace ui
