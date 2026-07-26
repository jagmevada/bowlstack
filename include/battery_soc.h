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

static const float LEVEL_LOW_ABOVE = 10.0f;
static const float LEVEL_MEDIUM_ABOVE = 35.0f;
static const float LEVEL_GOOD_ABOVE = 70.0f;

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

inline Level levelFromSoc(int8_t soc) {
  if (soc < 0) return Level::Unknown;
  if (soc > LEVEL_GOOD_ABOVE) return Level::Good;
  if (soc > LEVEL_MEDIUM_ABOVE) return Level::Medium;
  if (soc > LEVEL_LOW_ABOVE) return Level::Low;
  return Level::Critical;
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

}  // namespace battery
