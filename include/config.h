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
// 100 kHz, not 400. Both buses use this constant, so this covers I2C0 and I2C1.
// The ESP32 is at the TOP of a 6 ft pipe, so f1 (lowest bowl) has the longest
// cable and f2 the next, and both sit on I2C0. Roughly 200 pF of harness against
// two 10k pull-ups in parallel gives a ~1 us rise time: outside the 300 ns that
// fast mode allows, inside the 1000 ns of standard mode. It costs nothing --
// measured 21.0 samples/s at 400 kHz and 20.9 at 100 kHz, because
// TIMING_BUDGET_US dominates. ~2.2k pull-ups on I2C0 would make 400 kHz safe.
static const uint32_t I2C_HZ = 100000;

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
// PRESENT_BELOW_MM was 100 mm, a bench figure taken from 4 sensors on a table
// with targets blocked by hand. Real bowls do not sit that close to their
// sensor, so it is now 200 mm, set against the actual stack at the real
// standoff.
//
// That narrows the dead band from 300 mm to 200 mm. Still wide against a few
// millimetres of measurement noise, but with less margin than before: a level
// settling near 200 mm will hold whichever state it last had rather than
// resolving. The bowl taper means each level may want a slightly different
// present-distance, so re-check per level if any one of them proves indecisive.
static const float PRESENT_BELOW_MM = 200.0f;
static const float ABSENT_ABOVE_MM = 400.0f;

// --- status LEDs -----------------------------------------------------------
// Five indicators. LED_LEVEL[i] mirrors the thresholded presence of level i --
// f1 on LED 1 and so on -- and LED_HEALTH carries device state.
//
// ALL LEDs ARE COMMON ANODE: the cathode is switched, so the GPIO is ACTIVE
// LOW. Driving a pin HIGH turns its LED OFF. They are also driven OFF as the
// very first act of indicators::begin(), because until a pin is configured it
// floats and the anode's pull to VCC lights the LED -- every indicator would
// glow through boot and read as "everything is fine" before anything had been
// measured.
//
// Pin choice is constrained more than it looks on ESP32:
//   6-11        flash, unusable
//   34-39       input only, cannot drive anything
//   12          MTDI strapping -- HIGH at boot misconfigures flash voltage and
//               can brick the module. Never use for an active-low LED.
//   0, 2, 15    boot strapping; an LED pulling them toward VCC breaks download
//               mode or the boot log
//   1, 3        UART0, in use by the serial console
// 4, 13, 14, 18 and 19 are free of all of that.
static const uint8_t LED_LEVEL[SENSOR_COUNT] = {4, 13, 14, 18};
static const uint8_t LED_HEALTH = 19;

// Health blink rates, in Hz. Solid = healthy.
//
// WiFi loss blinks FASTER than a sensor fault and takes precedence when both
// are true. One LED cannot show two things, and the choice is deliberate: a
// sensor fault still leaves the server receiving data with an explicit fault
// flag, whereas a dropped link means nobody downstream can see anything at all.
static const float HEALTH_BLINK_SENSOR_HZ = 1.0f;
static const float HEALTH_BLINK_WIFI_HZ = 2.0f;

// --- battery and charger ---------------------------------------------------
// GPIO35 is input-only, which ruled it out for XSHUT but makes it ideal here.
// It is ADC1: ADC2 is unusable whenever WiFi is active, so an ADC1 pin is
// mandatory given the telemetry phase to come.
static const uint8_t PIN_BATTERY_ADC = 35;

// --- charger sense ---------------------------------------------------------
// Charger 5 V rail through a 10k series resistor to GPIO27, ACTIVE HIGH, with
// the INTERNAL pull-down enabled.
//
//     charger 5V --[10k]-- GPIO27 (INPUT_PULLDOWN)
//
// Charging: the input clamp conducts and holds the pin at ~3.3 V, sinking
//   (5 - 3.3) / 10k = 170 uA into the rail. Comfortably inside what the clamp
//   tolerates, and far above the ~2.48 V logic-high threshold.
// Idle:     the internal pull-down (~45k) takes the pin to a solid 0 V, so
//   "not charging" is a real reading rather than an undriven input.
//
// The internal pull-down is used here rather than an external resistor because
// the clamp -- not the pull-down -- sets the charging-state voltage, so the
// pull-down's loose tolerance never enters the measurement. It only has to win
// against leakage when nothing is connected, which it does easily.
static const uint8_t PIN_CHARGING = 27;

// FALSE: this senses the charger's 5 V rail, so the pin is HIGH while charging.
// True would suit a TP4056-style open-drain STAT output, which pulls low.
static const bool CHARGING_ACTIVE_LOW = false;

// Majority vote over this many reads. Does not make a floating input correct,
// but it stops a single noise sample from flipping the reported charge state.
static const uint8_t CHARGING_SAMPLES = 5;

// How often the power line -- cell voltage, band and charge state -- is printed
// to the console. Present in the PRODUCTION build too: it is the only power
// telemetry visible without a network. A cell discharges over hours and the
// band has four steps, so anything faster than this is noise on the console
// rather than information.
static const uint32_t POWER_REPORT_MS = 10000;

// Millivolts at the CELL per millivolt the ADC REPORTS. Not the resistor ratio:
// it deliberately folds two independent errors into one measured number.
//
//   divider tolerance   two 10k resistors are 2.0 nominal, 2.0049 measured
//                       (4100 mV cell / 2045 mV at the pin by multimeter)
//   ADC gain error      the ADC reported 2078.7 mV where the pin was truly
//                       2045 -- 1.6% high, ordinary for ESP32 even with eFuse
//                       calibration
//
// Correcting only the divider would have left the ADC error untouched and
// reported a 4.10 V cell as 4.17 V, i.e. 100% where the truth is ~95%.
//
//   factor = true cell mV / reported pin mV = 4100 / 2078.7 = 1.9724
//
// This is a SINGLE-POINT calibration of ONE board. Both error sources vary per
// unit -- resistor tolerance and per-chip ADC calibration -- so across 32
// devices expect a few percent of SoC error if this value is shared. That is
// tolerable given the four coarse bands, but near the knee of the discharge
// curve a few percent of voltage is worth far more than a few percent of
// charge, so calibrate per unit if a band boundary ever matters:
//
//   1. read the "pin" figure from the battery console line
//   2. measure the cell with a multimeter
//   3. factor = cell mV / pin mV
//   4. platformio.ini: -DBOWLSTACK_BATTERY_CAL=1.9724f
#ifndef BOWLSTACK_BATTERY_CAL
#define BOWLSTACK_BATTERY_CAL 1.9724f
#endif
//
//     battery + --[10k]--+-- GPIO35
//                        |
//                      [10k]
//                        |
//                       GND
//
// The ratio is chosen by where the ADC is actually accurate, not by what fits.
// At ADC_11db the nominal full scale is 3.3 V, but the datasheet's recommended
// input range is 150-2450 mV; beyond that the converter goes non-linear and
// saturates early. Halving puts a full 4.2 V cell at 2.10 V and an empty
// 2.75 V one at 1.38 V, so the ENTIRE useful battery range sits inside the
// accurate window.
//
// A 5k/10k divider was the alternative and is rejected: it would put a fully
// charged cell at 2.80 V, past the recommended limit -- error precisely where
// the reading is used to judge the battery healthy.
//
// Equal 10k resistors also give a 5k source impedance, within the ~10k the ADC
// needs to sample accurately, while drawing only ~210 uA (about 1.7 mAh/day at
// 8 h service, negligible against a 3.4 Ah cell). Larger resistors would save
// current but push source impedance out of spec.
static const float BATTERY_DIVIDER = BOWLSTACK_BATTERY_CAL;

// Li-ion endpoints, kept for reference. The real discharge curve is far from
// linear -- see include/battery_soc.h -- so these bound the range rather than
// define the mapping.
static const uint16_t BATTERY_MIN_MV = 3300;
static const uint16_t BATTERY_MAX_MV = 4200;

// --- battery: sampling and filtering ---------------------------------------
// How often the ADC is actually read -- 10 Hz. device_status::sample() runs
// from the sensor task every SENSOR_TICK_MS (2 ms), and 16 conversions at
// ~100 us each is ~1.6 ms of work: reading the battery on every call meant
// ~8000 conversions/second, consuming most of the measurement task to track a
// quantity that moves over hours. 10 Hz is 160 conversions/second for the same
// answer.
//
// It also gives the EMA below a meaningful time base. Filtering is about time,
// and back-to-back reads leave none -- averaging samples taken 60 us apart
// mostly averages the same noise excursion.
static const uint32_t BATTERY_SAMPLE_INTERVAL_MS = 100;

// Exponential moving average across those reads, on top of the 16-sample
// oversample within each. At alpha 0.2 and a 100 ms interval the time constant
// is ~500 ms, cutting residual noise about threefold while still settling
// faster than the 10 s console cadence. Measured swing before filtering was
// +/-23 mV, about +/-3% of SoC on the steep part of the curve -- enough on its
// own to straddle a band boundary indefinitely.
static const float BATTERY_EMA_ALPHA = 0.20f;

// --- battery: presence hysteresis ------------------------------------------
// SEPARATE rising and falling thresholds, and the pair that matters most in
// practice. With a single threshold, the contact bounce of inserting a cell
// drags the reading back and forth across it, and each crossing flips the band
// between a real level and "unknown" -- every flip a state change, every change
// a telemetry event. Observed as a burst of report(change) lines during a
// battery swap that settled only once the cell was seated.
static const uint16_t BATTERY_PRESENT_ABOVE_MV = 2700;  // absent -> present
static const uint16_t BATTERY_ABSENT_BELOW_MV = 2500;   // present -> absent

// ...and a DWELL on top of those thresholds. Amplitude hysteresis alone cannot
// survive a cell being inserted, because during the contact bounce the reading
// genuinely is at 0 and at 4150 in alternate samples -- it crosses both
// thresholds legitimately, so no threshold pair can reject it. Only requiring
// the reading to stay present for a continuous interval can. 500 ms is ~5
// samples at the 10 Hz rate, comfortably longer than a contact settles.
static const uint32_t BATTERY_PRESENCE_DEBOUNCE_MS = 500;

// Upper bound, likewise hysteretic. Above this no lithium cell can be present:
// without it the SoC curve clamps anything past its top point to 100%, so a
// floating pin reads as a healthy FULL battery -- measured on the bench at
// 6365 mV reported as "100% (good)".
static const uint16_t BATTERY_IMPLAUSIBLE_ABOVE_MV = 4400;  // valid -> invalid
static const uint16_t BATTERY_PLAUSIBLE_BELOW_MV = 4300;    // invalid -> valid

// Ceiling applied to battery_mv before it is PUBLISHED. Must not exceed the
// `check (battery_mv between 0 and 6000)` bound in supabase/schema.sql.
//
// This is a wire constraint, not a measurement one. The floating-pin fault this
// firmware already hit read 6365 mV, which the CHECK rejects -- and PostgREST
// answers 400, so patchStatus() fails, backs off 15 s, and retries forever
// without ever succeeding. A unit with a disconnected divider would therefore
// never report its BOWL COUNT: the product lost to a battery-wiring fault.
//
// Clamping rather than sending null keeps the diagnostic. 6000 mV is still
// impossible for a single Li-ion cell, so a dashboard seeing it still knows the
// divider is broken -- which is the entire reason battery_mv is published.
static const uint16_t BATTERY_PUBLISH_MAX_MV = 6000;

// --- telemetry sizing -------------------------------------------------------
// Depth of the per-device offline history buffer. A real unit gets 32; the fleet
// simulator overrides this down because it holds one buffer PER VIRTUAL DEVICE,
// and 31 x 32 entries would be several times the RAM the exercise needs. The
// simulator is not testing buffer depth -- it is testing the wire interface and
// the front-end's handling of what arrives.
#ifndef BOWLSTACK_TELEMETRY_QUEUE_LEN
#define BOWLSTACK_TELEMETRY_QUEUE_LEN 32
#endif
static const uint8_t TELEMETRY_QUEUE_LEN = BOWLSTACK_TELEMETRY_QUEUE_LEN;

// --- battery: band hysteresis (percent) ------------------------------------
// One rising and one falling threshold per boundary. A band must be HARDER to
// leave than it was to enter, or a reading sitting on a boundary oscillates
// forever on ADC noise alone -- and each oscillation is a published state
// change, not merely a cosmetic wobble.
//
// The FALLING thresholds are the nominal band edges -- 10 / 35 / 70 -- because
// those are the numbers documented to the front-end, and a battery in service
// is discharging. The rising thresholds sit 5 points above them.
//
// 5 points is sized from measured noise. Unfiltered swing was +/-23 mV, about
// +/-3% of SoC where the curve is steep; the EMA cuts that to roughly +/-1%, so
// 5 points is about 5 sigma. 3 would have been the tempting choice and is not
// enough -- the EMA output is correlated sample to sample, so excursions
// persist rather than averaging away within a band crossing, and a cell parked
// near an edge would still flip occasionally. The cost of the wider band is
// that a charging cell reads one band low for a few extra percent, which is
// invisible in a 4-band indicator.
//
// This is the fault that prompted it: a charging cell sat at 3577-3623 mV,
// reading 35-41%, with the low/medium boundary at exactly 35. It alternated
// low/medium for minutes, and every alternation was a published state change.
static const float BAT_CRITICAL_TO_LOW_UP = 15.0f;
static const float BAT_LOW_TO_CRITICAL_DOWN = 10.0f;
static const float BAT_LOW_TO_MEDIUM_UP = 40.0f;
static const float BAT_MEDIUM_TO_LOW_DOWN = 35.0f;
static const float BAT_MEDIUM_TO_GOOD_UP = 75.0f;
static const float BAT_GOOD_TO_MEDIUM_DOWN = 70.0f;

// --- battery: reported percentage ------------------------------------------
// The console figure only moves once it has moved this far, so a stationary
// cell shows a stationary number. Only the BAND is published, so this is
// presentation rather than protocol -- but a percentage that jitters by a point
// every line makes the console harder to read during calibration.
static const int8_t BATTERY_PCT_DEADBAND = 2;

// --- charger sense: debounce ------------------------------------------------
// A new charging state must hold for this long before it is accepted. The
// majority vote in readCharging() samples microseconds apart, which rejects a
// noise spike but not a genuinely bouncing contact -- plugging a charger in is
// a mechanical event lasting tens of milliseconds, and every bounce would
// otherwise be a published change.
static const uint32_t CHARGING_DEBOUNCE_MS = 2000;

}  // namespace config
