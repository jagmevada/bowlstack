// Measured Li-ion discharge curve, and state-of-charge lookup.
//
// SOURCE DATA AND AN IMPORTANT CORRECTION
// ---------------------------------------
// The supplied table was headed "SoC(%)" but its first column is actually
// DEPTH OF DISCHARGE -- the fraction of capacity REMOVED:
//
//     col1    Ah drawn   volts
//      0.0      0.0      4.159      <- full cell, nothing drawn
//    100.0      3.4      2.750      <- empty cell, 3.4 Ah drawn
//
// Voltage falls as the first column rises, which is the signature of a
// discharge curve, not a charge state. Used literally it would report a flat
// battery as 100% and a full one as 0% -- an inversion that looks plausible on
// a bench where the cell is always near full.
//
// The table below is therefore stored as SoC = 100 - (supplied column),
// ordered by descending voltage so lookup is a simple walk.
//
// CUTOFF
// ------
// The stated operating range is 3.0 V empty / 4.2 V full, but the measured
// curve runs to 2.750 V and tops out at 4.159 V. Those are not in conflict:
// 3.0 V lands at roughly 5% on this curve, i.e. the stated cutoff keeps a
// small reserve rather than running the cell to the knee. 4.2 V is the charger
// terminal voltage; 4.159 V is what the cell settles to under a 0.2 A draw.
//
// Curve measured at a 0.2 A rate. Under a heavier load the cell sags and this
// will read pessimistically; the device draws far less, so error is toward
// under-reporting charge, which is the safe direction.

#pragma once

#include <Arduino.h>
#include <math.h>

#include "config.h"

namespace battery {

struct SocPoint {
  uint16_t mv;  // cell terminal voltage
  float soc;    // percent remaining
};

// Descending voltage. First entry is the fullest measured point.
static const SocPoint SOC_CURVE[] = {
    {4159, 100.0f}, {4095, 94.1f}, {4064, 88.2f}, {4018, 82.4f},
    {3956, 76.5f},  {3898, 70.6f}, {3832, 64.7f}, {3774, 58.8f},
    {3714, 52.9f},  {3662, 47.1f}, {3621, 41.2f}, {3583, 35.3f},
    {3522, 29.4f},  {3471, 23.5f}, {3380, 17.6f}, {3250, 11.8f},
    {3050, 5.9f},   {2750, 0.0f},
};

static const uint8_t SOC_CURVE_LEN = sizeof(SOC_CURVE) / sizeof(SOC_CURVE[0]);

// Four coarse bands. A percentage from a resting-voltage curve is far less
// precise than its two decimal places suggest -- load, temperature and cell
// age all shift it -- so the UI and the operator work in bands, and the raw
// millivolts stay available for anyone who needs the underlying number.
enum class Level : uint8_t {
  Unknown,   // no cell detected
  Critical,  // replace or charge now
  Low,
  Medium,
  Good,
};

// There is deliberately NO stateless soc -> Level function here.
//
// There used to be, and every caller of it was a bug waiting to happen: a
// single-threshold classifier oscillates whenever its input rests on a
// threshold, which for a battery band is most of the time. Classification is
// only correct with the history that Monitor keeps, so Monitor is the only way
// to obtain a Level. The nominal band edges -- 10 / 35 / 70 percent, the
// falling thresholds in config.h -- remain the numbers documented to the
// front-end.

// Piecewise-linear interpolation over the measured curve. Returns 0..100.
// Clamps outside the measured range rather than extrapolating: beyond 4.159 V
// the cell is simply full, and below 2.750 V it is past anything that was
// measured, so a straight-line guess there would be fiction.
inline float socFromMillivolts(uint16_t mv) {
  if (mv >= SOC_CURVE[0].mv) return 100.0f;
  if (mv <= SOC_CURVE[SOC_CURVE_LEN - 1].mv) return 0.0f;

  for (uint8_t i = 1; i < SOC_CURVE_LEN; i++) {
    if (mv >= SOC_CURVE[i].mv) {
      const SocPoint &hi = SOC_CURVE[i - 1];
      const SocPoint &lo = SOC_CURVE[i];
      const float span = (float)(hi.mv - lo.mv);
      if (span <= 0.0f) return lo.soc;
      return lo.soc + (hi.soc - lo.soc) * ((float)(mv - lo.mv) / span);
    }
  }
  return 0.0f;
}

inline const char *levelName(Level l) {
  switch (l) {
    case Level::Good:     return "good";
    case Level::Medium:   return "medium";
    case Level::Low:      return "low";
    case Level::Critical: return "critical";
    default:              return "unknown";
  }
}

// ---------------------------------------------------------------------------
// HoldDebounce -- a boolean that must hold a new value for a dwell time before
// it is believed. Hysteresis in the TIME domain rather than the amplitude
// domain, and the right tool for anything mechanical.
//
// Used for two inputs here: the charger sense, and battery presence. Both are
// contacts, and a contact does not change state cleanly -- it bounces for tens
// of milliseconds. An amplitude filter cannot fix that, because during a bounce
// the signal genuinely is at both values.
// ---------------------------------------------------------------------------
class HoldDebounce {
 public:
  bool update(bool raw, uint32_t nowMs, uint32_t dwellMs) {
    if (!seeded_) {
      // Adopt the first observation rather than debouncing away from a default.
      // Otherwise a device that boots on charge spends the first dwell claiming
      // it is not charging, and then reports a "change" that never happened.
      stable_ = candidate_ = raw;
      sinceMs_ = nowMs;
      seeded_ = true;
      return stable_;
    }
    if (raw != candidate_) {
      candidate_ = raw;
      sinceMs_ = nowMs;  // restart the dwell on every bounce
    }
    if (candidate_ != stable_ && (uint32_t)(nowMs - sinceMs_) >= dwellMs) {
      stable_ = candidate_;
    }
    return stable_;
  }

  bool state() const { return stable_; }

 private:
  bool seeded_ = false;
  bool stable_ = false;
  bool candidate_ = false;
  uint32_t sinceMs_ = 0;
};

// ---------------------------------------------------------------------------
// Monitor -- filtering and hysteresis around the curve above.
//
// WHY THIS EXISTS
// ---------------
// Every classifier with a single threshold oscillates when its input sits on
// that threshold, and a battery reading sits on one for hours: the whole point
// of a band is that the cell spends a long time near each edge. Observed on the
// bench, charging, with the low/medium boundary at exactly 35%:
//
//     cell 3583 mV : 35% -> low
//     cell 3619 mV : 41% -> medium
//     cell 3579 mV : 35% -> low
//     cell 3597 mV : 37% -> medium
//
// The cell was fine and the ADC was working. +/-23 mV of noise on a 3.6 V cell
// is about +/-3% of SoC where the curve is steep, and that was enough to
// alternate bands indefinitely -- each alternation a published state change and
// a Supabase write.
//
// Two independent mechanisms fix it, and both are needed:
//   FILTER     -- an EMA over time, so the input stops moving +/-3% per read.
//   HYSTERESIS -- separate rising and falling thresholds, so that even when the
//                 input does cross a boundary, it must cross back by a margin
//                 to undo it.
// The filter alone only narrows the noise; a quiet input parked exactly on a
// threshold still flips on the last surviving millivolt. Hysteresis alone works
// but needs a wide band to swallow raw noise, which costs real resolution.
// Together each can stay modest.
//
// Hysteresis is applied AFTER filtering, never before -- feeding a latched
// value back into a filter builds a system whose output depends on its own
// history in a way that is very hard to reason about.
//
// Single-threaded: owned by the sensor task via device_status::sample().
// ---------------------------------------------------------------------------
class Monitor {
 public:
  // Feed a fresh CELL-side millivolt reading, already scaled by the divider and
  // already oversampled. Call at a steady rate -- the EMA time constant is
  // expressed in samples, so an irregular interval changes the filter.
  void update(uint16_t cellMv, uint32_t nowMs) {
    lastRawMv_ = cellMv;

    // --- 1. presence, decided on the RAW reading ---------------------------
    // ORDER MATTERS, and getting it wrong was a real bug. This used to filter
    // first and latch presence off the FILTERED value, which couples the two:
    // presence gates the filter, so deciding presence from the filter's own
    // output means the absent period leaks into the present one.
    //
    // What that produced: with no cell fitted the EMA decays toward 0 mV, and
    // because haveEma_ latched true forever it never re-seeded. Fitting a full
    // 4150 mV cell into a running unit then had the EMA climb from ~0, cross the
    // presence threshold partway up, and be classified on a voltage the cell
    // never had. Simulated against this curve: critical -> low -> medium -> good
    // over 1.4 s, i.e. FOUR published band changes and a `critical` battery
    // alert for a cell at 99%. The mirror case was worse -- recovering from the
    // floating-pin fault, the EMA decayed from 6365 mV and re-entered the
    // plausible window at 4259 mV, publishing a confident FULL battery.
    //
    // Deciding presence on the raw sample makes the two independent: the filter
    // never sees a sample from the wrong regime, so it has nothing to unlearn.
    if (rawPresent_) {
      if (cellMv < config::BATTERY_ABSENT_BELOW_MV ||
          cellMv > config::BATTERY_IMPLAUSIBLE_ABOVE_MV) {
        rawPresent_ = false;
      }
    } else {
      if (cellMv >= config::BATTERY_PRESENT_ABOVE_MV &&
          cellMv <= config::BATTERY_PLAUSIBLE_BELOW_MV) {
        rawPresent_ = true;
      }
    }

    // Amplitude hysteresis alone cannot survive a battery being inserted: the
    // contact bounces, so the raw reading is genuinely at 0 and at 4150 in
    // alternate samples and crosses both thresholds legitimately. A dwell is
    // what rejects that -- the cell must read present CONTINUOUSLY before the
    // band is allowed to change.
    const bool present =
        presence_.update(rawPresent_, nowMs, config::BATTERY_PRESENCE_DEBOUNCE_MS);

    if (!present) {
      // Discard the filter state so the next cell is seeded from its own first
      // sample rather than ramping up from this one's absence.
      haveEma_ = false;
      // Say "unknown", not 0%. A fabricated zero is indistinguishable upstream
      // from a genuinely flat cell, and the two call for opposite actions.
      level_ = Level::Unknown;
      reportedPct_ = -1;
      return;
    }

    // --- 2. filter, over present samples only ------------------------------
    if (!haveEma_) {
      // Seed rather than ramp. Starting from anywhere else walks up through
      // every band, and each step looks like a real transition.
      emaMv_ = (float)cellMv;
      haveEma_ = true;
    } else {
      emaMv_ += config::BATTERY_EMA_ALPHA * ((float)cellMv - emaMv_);
    }
    const uint16_t mv = (uint16_t)lroundf(emaMv_);

    const float soc = socFromMillivolts(mv);

    // --- 3. band, hysteretic ----------------------------------------------
    // Each boundary is evaluated with the threshold appropriate to which side
    // we are currently on: harder to leave a band than it was to enter. Doing
    // all three independently -- rather than only checking the neighbours of
    // the current band -- means a large genuine jump (a cell swap, a charger
    // connecting) still lands in the right band immediately instead of stepping
    // through the intermediate ones.
    //
    // The FIRST classification after the cell appears uses the falling
    // thresholds, i.e. the nominal band edges, by starting every boundary as
    // "above". There is no history to preserve on a first look, so hysteresis
    // has nothing to do, and the nominal edges are the ones documented to the
    // front-end. Starting from "below" instead would classify a cell that boots
    // at 72% as `medium` and leave it there for the rest of the discharge, since
    // it can never rise to the 75% entry threshold.
    const bool fresh = (level_ == Level::Unknown);
    bool overCrit = fresh || (level_ == Level::Low || level_ == Level::Medium ||
                              level_ == Level::Good);
    bool overLow = fresh || (level_ == Level::Medium || level_ == Level::Good);
    bool overMed = fresh || (level_ == Level::Good);

    overCrit = crossed(soc, overCrit, config::BAT_CRITICAL_TO_LOW_UP,
                       config::BAT_LOW_TO_CRITICAL_DOWN);
    overLow = crossed(soc, overLow, config::BAT_LOW_TO_MEDIUM_UP,
                      config::BAT_MEDIUM_TO_LOW_DOWN);
    overMed = crossed(soc, overMed, config::BAT_MEDIUM_TO_GOOD_UP,
                      config::BAT_GOOD_TO_MEDIUM_DOWN);

    if (overMed) {
      level_ = Level::Good;
    } else if (overLow) {
      level_ = Level::Medium;
    } else if (overCrit) {
      level_ = Level::Low;
    } else {
      level_ = Level::Critical;
    }

    // --- 4. percentage deadband -------------------------------------------
    // Only the band is published, so this is presentation: it stops the console
    // figure twitching by a point between lines on a cell that is not moving,
    // which otherwise makes calibration readings hard to trust.
    const int8_t pct = (int8_t)lroundf(soc);
    if (reportedPct_ < 0 ||
        abs((int)pct - (int)reportedPct_) >= config::BATTERY_PCT_DEADBAND) {
      reportedPct_ = pct;
    }
  }

  // Filtered while a cell is present; the last RAW reading otherwise. Not zero
  // when absent, deliberately: battery_mv is published as a diagnostic, and its
  // whole purpose is to distinguish an open input (~0 mV) from a divider fault
  // or floating pin (implausibly high). Reporting 0 for both would collapse the
  // two and send someone hunting for a dead battery over a wiring fault.
  uint16_t millivolts() const {
    return haveEma_ ? (uint16_t)lroundf(emaMv_) : lastRawMv_;
  }
  int8_t percent() const { return reportedPct_; }
  Level level() const { return level_; }
  bool present() const { return presence_.state(); }

 private:
  // One boundary with two thresholds. `over` is which side we are currently on,
  // which is what makes the result depend on history -- the definition of a
  // Schmitt trigger.
  static bool crossed(float soc, bool over, float upThreshold,
                      float downThreshold) {
    return over ? (soc >= downThreshold) : (soc >= upThreshold);
  }

  bool haveEma_ = false;
  float emaMv_ = 0.0f;
  uint16_t lastRawMv_ = 0;
  bool rawPresent_ = false;   // amplitude hysteresis, pre-dwell
  HoldDebounce presence_;     // time-domain dwell on top of it
  Level level_ = Level::Unknown;
  int8_t reportedPct_ = -1;
};

// Charger sense uses the same dwell debounce, with its own interval.
//
// The majority vote in device_status samples microseconds apart, which rejects
// an isolated noise spike but not a bouncing contact -- plugging a charger in is
// a mechanical event lasting tens of milliseconds, and every bounce during it
// would otherwise be a published state change.
using ChargeDebounce = HoldDebounce;

}  // namespace battery
