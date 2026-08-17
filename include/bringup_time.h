// NTP wall clock on its own FreeRTOS task. See src/bringup/bringup_time.cpp.
#pragma once

#include <stdint.h>

namespace bringup_time {
void begin();

// Copies the latest time into the UI state. Called from the UI thread, so the
// task never touches an LVGL object -- the same one-task-owns-a-subsystem rule
// the rest of this codebase follows.
void publish();

uint32_t stackFreeBytes();
}  // namespace bringup_time
