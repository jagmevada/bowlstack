// Wall-clock time from NTP, on its own FreeRTOS task.
//
// A TASK, NOT A loop() CALL, and that is the point rather than a convenience.
// SNTP's first sync blocks on DNS and a UDP round trip; on a slow or absent
// link that is seconds. docs/firmware.md section 2 is entirely about what
// happens when network work shares a loop with measurement -- the discrete
// build measured 138 s of blocked ranging in the captive portal -- so this goes
// where that document says network work goes: core 0, below everything that
// measures.
//
// It is also the first piece of the task fabric docs/waveshare_port.md section
// 4 specifies for this board. The rest follows when the sensors are wired.
//
// THE DEVICE HAS NO RTC. There is no battery-backed clock on this board, so
// time is genuinely unknown from power-on until the first sync lands, and
// ui_state.h's `timeKnown` exists to say so rather than let the bar show 00:00.
// After a sync the ESP32's internal counter keeps running even if WiFi drops --
// so "known" stays true and merely drifts, which is the honest description.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

#include "bringup_time.h"
#include "ui_demo.h"

namespace bringup_time {
namespace {

// Asia/Kolkata, the default in the devices table (docs/supabase.md section 2).
// POSIX TZ strings invert the sign: IST-5:30 means UTC+5:30.
const char *TZ_IST = "IST-5:30";

// Every five minutes, as asked. SNTP's own default is an hour; this is set
// explicitly so the interval is a decision in this file rather than a library
// default someone has to go looking for.
const uint32_t RESYNC_MS = 5 * 60 * 1000;

// Packed into ONE 32-bit word so the UI can read it without a mutex. Aligned
// 32-bit loads and stores are atomic on this core, so a reader either sees the
// whole previous value or the whole new one -- never an hour from one sync
// beside a minute from the next. A mutex here would mean the UI thread could
// block on a task doing DNS.
volatile uint32_t packed_ = 0;  // bit16 = known, bits 8-15 = hh, bits 0-7 = mm

TaskHandle_t task_ = nullptr;
volatile bool everSynced_ = false;

void onSync(struct timeval *) {
  everSynced_ = true;
  Serial.println("time: sntp sync");
}

void timeTask(void *) {
  // Configure once. esp_sntp then owns the retry and the periodic refresh; a
  // hand-rolled poll loop would duplicate logic that is already correct.
  esp_sntp_set_sync_interval(RESYNC_MS);
  sntp_set_time_sync_notification_cb(onSync);
  configTzTime(TZ_IST, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  Serial.printf("time: sntp configured, tz %s, resync %lu s\n", TZ_IST,
                (unsigned long)(RESYNC_MS / 1000));

  for (;;) {
    struct tm tm_now;
    // 0 ms timeout: ask what the clock currently says and return immediately.
    // A non-zero timeout would make this task sleep inside getLocalTime rather
    // than at its own vTaskDelay, which is the same blocking with worse
    // visibility.
    if (everSynced_ && getLocalTime(&tm_now, 0)) {
      packed_ = (1u << 16) | ((uint32_t)tm_now.tm_hour << 8) | (uint32_t)tm_now.tm_min;
    } else {
      packed_ = 0;
    }

    // One second. The bar shows hh:mm, so this is already 60x more often than
    // the display can change -- but it is what makes the minute roll over
    // promptly rather than up to a minute late.
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

}  // namespace

void begin() {
  // Core 0, priority 1. The network core, below anything that measures --
  // exactly where docs/firmware.md puts WiFi work, and the reason that
  // document gives is that a stall here must not be able to preempt ranging.
  xTaskCreatePinnedToCore(timeTask, "time", 4096, nullptr, 1, &task_, 0);
  Serial.println("time: task started on core 0");
}

void publish() {
  const uint32_t p = packed_;
  ui::demoOverrideTime((p >> 16) & 1, (uint8_t)((p >> 8) & 0xFF), (uint8_t)(p & 0xFF));
}

uint32_t stackFreeBytes() {
  // uxTaskGetStackHighWaterMark returns BYTES on ESP-IDF, not words --
  // StackType_t is uint8_t. Scaling by 4 the way vanilla FreeRTOS requires
  // reports four times the real headroom, which is how a task heading for
  // overflow hides behind a comfortable number. docs/firmware.md section 2
  // records this from the discrete build.
  return task_ ? uxTaskGetStackHighWaterMark(task_) : 0;
}

}  // namespace bringup_time
