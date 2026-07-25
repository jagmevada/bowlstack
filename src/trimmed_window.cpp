#include "trimmed_window.h"

#include <math.h>
#include <string.h>

void TrimmedWindow::push(uint16_t value) {
  ring_[next_] = value;
  next_ = (next_ + 1) % config::AVG_WINDOW;
  if (count_ < config::AVG_WINDOW) count_++;
}

void TrimmedWindow::clear() {
  count_ = 0;
  next_ = 0;
}

WindowStats TrimmedWindow::stats() const {
  WindowStats s = {0.0f, 0.0f, count_, 0};
  if (count_ == 0) return s;

  uint16_t sorted[config::AVG_WINDOW];
  memcpy(sorted, ring_, count_ * sizeof(uint16_t));

  // Insertion sort. AVG_WINDOW is single digits, so this beats anything
  // cleverer and costs microseconds against a 50-200 ms measurement.
  for (uint8_t i = 1; i < count_; i++) {
    uint16_t v = sorted[i];
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && sorted[j] > v) {
      sorted[j + 1] = sorted[j];
      j--;
    }
    sorted[j + 1] = v;
  }

  // Only trim once enough samples remain for the result to still be an
  // average rather than a single survivor.
  const uint8_t trim =
      (count_ >= (uint8_t)(2 * config::TRIM + 2)) ? config::TRIM : 0;
  const uint8_t lo = trim;
  const uint8_t hi = count_ - trim;
  const uint8_t n = hi - lo;

  uint32_t sum = 0;
  for (uint8_t i = lo; i < hi; i++) sum += sorted[i];
  s.mean = (float)sum / n;

  float var = 0.0f;
  for (uint8_t i = lo; i < hi; i++) {
    const float d = (float)sorted[i] - s.mean;
    var += d * d;
  }
  s.stdev = (n > 1) ? sqrtf(var / (n - 1)) : 0.0f;
  s.used = n;
  return s;
}
