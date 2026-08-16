#include "ui_perf.h"

#include <lvgl.h>
#include <stdio.h>

namespace ui {
namespace {

volatile uint32_t frames_ = 0;
uint32_t windowStart_ = 0;
uint32_t busyAccum_ = 0;
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
  if (busyPct_ > 100) busyPct_ = 100;
  worstReported_ = worstMs_;

  frames_ = 0;
  busyAccum_ = 0;
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
uint16_t perfWorstMs() { return worstReported_; }

void perfFormat(char *buf, uint32_t len) {
  snprintf(buf, len, "fps %u  busy %u%%  worst %ums  lvgl %luk/%luk frag %u%%", fps_,
           busyPct_, worstReported_, (unsigned long)(memUsed_ / 1024),
           (unsigned long)(memTotal_ / 1024), memFrag_);
}

}  // namespace ui
