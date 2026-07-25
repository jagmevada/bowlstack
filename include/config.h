// Every board-specific and tuning constant lives here. Nothing in this header
// depends on the rest of the codebase, so it can be included anywhere without
// pulling in hardware or driver headers.
//
// Credentials do NOT belong here -- WiFi and Supabase secrets go in
// include/secrets.h, which .gitignore already excludes.

#pragma once

#include <Arduino.h>
#include <Wire.h>

// Serial-plotter streaming is a TEST HARNESS, not production behaviour. It is
// off unless a build defines BOWLSTACK_DEBUG_PLOT=1 (see the esp32dev-debug
// environment in platformio.ini). When off, the debug_plot calls compile to
// nothing, so no formatting cost and no UART traffic reach a deployed unit.
// Boot enumeration and error logging are NOT gated by this -- those stay
// available in the field for fault diagnosis.
#ifndef BOWLSTACK_DEBUG_PLOT
#define BOWLSTACK_DEBUG_PLOT 0
#endif

namespace config {

// --- stack levels ----------------------------------------------------------
// Numbered bottom-upward: f1 watches the lowest bowl, f4 the highest.
static const uint8_t SENSOR_COUNT = 4;

// --- serial ----------------------------------------------------------------
// The Serial Plotter extension offers 9600/19200/38400/57600/115200/460800/
// 921600 -- select this value in its dropdown. 115200 is deliberate: at ~88
// bytes per line and 10 Hz the payload is ~8% of the link, and measurement put
// UART at 7.6 ms against a ~2 s latency budget. Raising it changes nothing
// perceptible; latency lives in AVG_WINDOW and DROPOUT_MISSES.
static const uint32_t SERIAL_BAUD = 115200;

// --- I2C -------------------------------------------------------------------
// Two independent hardware buses, two sensors each, so a slave that locks one
// bus cannot take down the whole array.
// Verified against the board: on I2C0, SDA is on 17 and SCL on 16, NOT the
// other way round. I2C1 is conventional.
static const uint8_t I2C0_SDA = 17;
static const uint8_t I2C0_SCL = 16;
static const uint8_t I2C1_SDA = 21;
static const uint8_t I2C1_SCL = 22;
static const uint32_t I2C_HZ = 400000;

// --- per-sensor wiring -----------------------------------------------------

struct SensorConfig {
  const char *name;
  TwoWire *bus;
  uint8_t xshutPin;
  uint8_t address;  // assigned during the XSHUT walk; 0x29 is the power-up default
};

// Defined in config.cpp, ordered bottom of the stack upward.
extern const SensorConfig SENSORS[SENSOR_COUNT];

// --- measurement tuning ----------------------------------------------------
// Two presets, because acquisition and threshold-setting want opposite things.
// Watching a live trace needs low latency; characterising presence thresholds
// wants the quietest possible reading. Flip to 0 for the accuracy preset.
//
//                          RESPONSIVE      ACCURACY
//   timing budget             50 ms          200 ms
//   effective integration    200 ms          800 ms   (4 samples after trim)
//   group delay (lag)       ~137 ms         ~550 ms
//   stdev @ 1 m              ~2.5 mm         ~1.3 mm
//   output rate               10 Hz            5 Hz
//
// Noise scales as 1/sqrt(integration), so RESPONSIVE costs ~2x -- irrelevant
// for deciding bowl present/absent, where the margin is hundreds of mm.
#define TUNING_RESPONSIVE 1

// Ranging noise is shot-noise limited, so accuracy is set by total integration
// time (budget x samples). Spending it inside the sensor beats averaging short
// samples, because each extra measurement re-pays a fixed VCSEL/pre-range
// overhead. 200000 is ST's "high accuracy" preset.
#if TUNING_RESPONSIVE
static const uint32_t TIMING_BUDGET_US = 50000;
static const uint32_t OUTPUT_PERIOD_MS = 100;
#else
static const uint32_t TIMING_BUDGET_US = 200000;
static const uint32_t OUTPUT_PERIOD_MS = 200;
#endif

// Minimum return signal to accept, MCPS. 0.25 is the driver default. Raising
// it rejects low-confidence returns, which helps against a cooperative target
// -- but the bowl wall is ~50% specular and reflects most light away at 23.5
// degrees, leaving only the ~25% diffuse component. Too high a threshold
// reports a present bowl as "no target", which is the worse failure.
static const float SIGNAL_RATE_LIMIT = 0.25f;

// Samples held in the moving average. Must be read together with TRIM:
// trimming discards 2*TRIM samples, so only (AVG_WINDOW - 2*TRIM) contribute.
// A window of 4 with TRIM 1 averages just 2 samples -- half the intended
// integration, and a 2-point stdev estimate too noisy to judge anything by.
static const uint8_t AVG_WINDOW = 6;

// Highest AND lowest samples dropped before averaging. Aged steel gives
// localized glints and dead spots; a plain mean smears those into the result,
// while trimming discards them outright.
static const uint8_t TRIM = 1;

// Consecutive out-of-range reads before the window is discarded. Deliberately
// NOT tied to AVG_WINDOW: when it was, a removed bowl took a full window
// (~1.25 s) to register as gone, which dominated perceived latency. Keep it
// above 1 so one bad ping cannot drop a whole window.
static const uint8_t DROPOUT_MISSES = 3;

// Samples a window must hold before its reading is trusted. This debounces
// APPEARANCE, which DROPOUT_MISSES does not: without it, losing a target takes
// three consecutive misses but gaining one takes a single hit, so a forearm or
// a tray crossing a beam for one measurement would register as a bowl. Tied to
// the trim threshold, so a trusted reading always has outlier rejection active
// -- below 2*TRIM+2 the trim is skipped and a lone stray sample would be
// reported with stdev 0.00, looking maximally confident.
static const uint8_t MIN_VALID_SAMPLES = 2 * TRIM + 2;

// --- sensor health ---------------------------------------------------------
// Consecutive I2C read failures before a sensor is declared Offline. 0xFFFF is
// the failure signature rather than a distance: the driver's readReg returns
// 0xFF per byte when nothing acknowledges, whereas a live sensor seeing no
// target reads ~8190 and never 65535.
static const uint8_t IO_FAILURES_TO_OFFLINE = 5;

// A sensor that still acknowledges its address but completes no measurement in
// this long has stopped ranging. The read path cannot see that failure, since
// its registers still answer.
static const uint32_t SENSOR_STALE_MS = 3000;

// Backoff between attempts to bring an Offline sensor back. Bounded so a
// permanently dead module cannot spin the loop retrying.
static const uint32_t RECOVER_RETRY_MS = 15000;

// Per-part systematic offset in mm, applied after averaging, in SENSORS order.
// Averaging kills random noise but cannot touch bias, and offset error is
// individual to each part. To calibrate one: leave it 0, put a target at a
// known distance, read the reported value, enter (actual - reported).
extern const float OFFSET_MM[SENSOR_COUNT];

// --- range interpretation --------------------------------------------------

// With no target in view the sensor reports ~8190 mm. Readings at or above
// this must never reach the average -- one would swamp a window of real data.
static const uint16_t OUT_OF_RANGE_MM = 8000;

// What to report when nothing is in view. It has to sit at the FAR end of the
// scale: "no object" means nothing within range, so reporting 0 would claim a
// target pressed against the sensor -- the exact opposite of the truth.
static const uint16_t NO_TARGET_MM = 2000;

// --- bowl presence thresholds ----------------------------------------------
// Schmitt trigger, not a single threshold. A bowl reads present below
// PRESENT_BELOW_MM and absent above ABSENT_ABOVE_MM; between the two the
// previous state is held. The dead band is the whole point: with one threshold
// a reading resting near it would chatter between present and absent on
// nothing but measurement noise, and every flap would be a spurious state
// change to report upstream.
//
// Provisional bench values -- 4 sensors on a table, targets blocked by hand to
// mimic stacking. Re-tune against real bowls at the real standoff during field
// testing; the taper means each level may settle at a slightly different
// present-distance.
static const float PRESENT_BELOW_MM = 100.0f;
static const float ABSENT_ABOVE_MM = 400.0f;

// --- battery and charger ---------------------------------------------------
// GPIO35 is input-only, which ruled it out for XSHUT but makes it ideal here.
// It is ADC1: ADC2 is unusable whenever WiFi is active, so an ADC1 pin is
// mandatory given the telemetry phase to come.
static const uint8_t PIN_BATTERY_ADC = 35;

// GPIO27 has an internal pull-up. GPIO34-39 do NOT (input-only pins have no
// pull resistors at all), so using one of those would force an external
// pull-up on the charger's open-drain STAT output.
static const uint8_t PIN_CHARGING = 27;

// True when the charger pulls STAT low while charging, as TP4056 and similar
// parts do.
static const bool CHARGING_ACTIVE_LOW = true;

// Ratio of the battery sense divider, e.g. 2.0 for equal-value resistors.
// A single Li-ion cell tops out at 4.2 V, above the ADC's usable range.
static const float BATTERY_DIVIDER = 2.0f;

// Li-ion endpoints for the percentage estimate. The real discharge curve is
// far from linear, so treat the percentage as indicative and the millivolt
// figure as the ground truth.
static const uint16_t BATTERY_MIN_MV = 3300;
static const uint16_t BATTERY_MAX_MV = 4200;

// Below this the input is floating rather than measuring a cell. Reported as
// "unknown" instead of a fabricated 0%, which matters right now because no
// battery is wired yet.
static const uint16_t BATTERY_ABSENT_BELOW_MV = 2500;

}  // namespace config
