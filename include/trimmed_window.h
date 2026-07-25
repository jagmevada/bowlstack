// Fixed-capacity moving window with a trimmed mean. Pure computation -- no
// hardware, no driver, no Serial -- so it can be reasoned about and unit
// tested independently of the sensors.

#pragma once

#include <Arduino.h>

#include "config.h"

struct WindowStats {
  float mean;
  float stdev;   // spread of the kept samples: the direct read on noise
  uint8_t held;  // samples currently in the window
  uint8_t used;  // samples that survived trimming and were averaged
};

class TrimmedWindow {
 public:
  void push(uint16_t value);
  void clear();

  bool empty() const { return count_ == 0; }
  uint8_t size() const { return count_; }

  // Discards the config::TRIM highest and lowest samples before averaging, so
  // a single glint off worn steel cannot drag the result. Returns a zeroed
  // struct when the window is empty.
  WindowStats stats() const;

 private:
  uint16_t ring_[config::AVG_WINDOW];
  uint8_t count_ = 0;
  uint8_t next_ = 0;
};
