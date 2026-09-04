// What the UI is allowed to know.
//
// This is the UI's input contract, and it is deliberately NOT DeviceStatus.
// DeviceStatus reaches for config.h, which reaches for Arduino.h and Wire.h --
// none of which exist when this same UI is compiled for a PC. Depending on it
// would make the desktop preview impossible, which is the one thing that makes
// iterating on a 2" layout bearable.
//
// So the seam is drawn here, in the same spirit as `Reading reading(level)`:
// the UI consumes a plain description of what to show and never learns where it
// came from. Firmware adapts DeviceStatus into this; the simulator fabricates
// it. Neither the screens nor the widgets can tell the difference, which is
// precisely what makes what you see on the PC the thing that ships.
//
// Nothing in this header may include a board, driver or framework header. That
// restriction is the whole point of the file.

#pragma once

#include <stdint.h>

namespace ui {

static const uint8_t LEVELS = 4;

// Bottom-upward, matching the physical pipe and FRONTEND_HANDOFF.md: index 0 is
// f1, the lowest bowl. Rendering it any other way up would contradict both the
// hardware and the web UI.
enum class Level : uint8_t { Unknown, Absent, Present };

// Vocabulary pinned to the wire format in supabase/schema.sql. `Discontiguous`
// is not a degraded count -- it is the absence of a trustworthy one.
enum class Stack : uint8_t { Ok, Discontiguous, Degraded };

// A BAND, never a percentage, and `Unknown` means no cell detected rather than
// an empty one. The device computes a percentage internally from a measured
// discharge curve and deliberately does not publish it: a resting-voltage
// estimate moves several points with load, temperature, cell age and per-unit
// ADC calibration, so a number on screen would imply precision the measurement
// does not have.
enum class Battery : uint8_t { Unknown, Critical, Low, Medium, Good };

// --- the weighing assembly --------------------------------------------------
// Three cells under one platform -- three points define a plane, two leave it
// free to rock. See scale.h for why the total is a SUM and why there is one
// gain constant rather than three.
//
// MUST EQUAL scale::CELLS. Restated here rather than shared because this file
// may not include a driver header, which is the whole point of it; the two are
// tied together by a static_assert in loadcell_main.cpp.
static const uint8_t CELLS = 3;

// Mirrors CellState in nau7802.h, restated here because ui_state.h may not
// include a driver header -- that restriction is the whole point of this file.
// The three-way split is the same one the ToF array uses and exists for the
// same reason: "configured but has not measured yet" is not "not working", and
// on a scale it is emphatically not zero, because zero is a real weight.
enum class Cell : uint8_t { Offline, Warming, Online };

struct CellView {
  Cell state;
  // Trimmed mean of the raw conversions with the tare already subtracted, so
  // this is the cell's own contribution and not an absolute converter reading.
  int32_t counts;
  // This cell's share of the load. Only meaningful when the assembly is
  // calibrated; the UI is expected to show counts otherwise rather than a
  // number whose unit it cannot name.
  float grams;
  uint16_t sps;  // conversions per second actually delivered

  // THE THREE FIGURES THE DEVICE PAGE EXISTS FOR, and none of them belongs on
  // the dashboard. `raw` is the last single conversion with nothing done to it,
  // `pp` is how far the raw signal moved across the window, and `offset` is the
  // tare being subtracted. Together they answer "is this cell alive and what is
  // being taken off it" -- which the smoothed, tared kilogram figure above is
  // specifically designed not to show.
  int32_t rawCounts;
  int32_t pp;
  // TWO OFFSETS. The platform's own weight, stored once per device and restored
  // every boot; and this session's tare, which is deliberately forgotten at
  // power-off. See scale.h for why collapsing them into one was wrong.
  int32_t platformZero;
  int32_t tare;

  // The converter has hit the end of its 24-bit range, so this cell has stopped
  // measuring and is reporting its own ceiling. Said rather than inferred: a
  // saturated cell returns a large, steady, plausible number and the weight
  // simply stops rising.
  bool overRange;
};

struct ScaleView {
  CellView cell[CELLS];

  int32_t totalCounts;
  float totalGrams;

  // FALSE MEANS THERE IS NO GRAM FIGURE, not that the figure is poor. A scale
  // that has never seen a known mass cannot convert counts to grams, and the
  // screen says counts rather than inventing a factor.
  bool calibrated;
  bool zeroed;   // every online cell has a stored platform zero
  bool tared;    // every online cell has a tare for this session
  // Every online cell has its commissioned platform zero from NVS. A unit can
  // be platformZeroed and not tared, which is the ordinary state at boot.
  bool platformZeroed;
  // Empty-vessel mass to subtract from what is shown and sent. 0 = off.
  float vesselOffsetG;
  bool overRange;  // at least one cell saturated -- the total is not a weight

  // Carried so the Device page can state them rather than the reader having to
  // remember what the build was flashed with.
  float countsPerGram;
  uint8_t window;  // samples in the moving average

  // Decimal places the KILOGRAM reading is shown to: 1, 2 or 3. Display only --
  // grams above are unrounded and the Diagnose page ignores this entirely, so
  // the setting can never cost anybody a figure they came looking for.
  uint8_t decimals;

  // The reference mass last calibrated against on this unit. Shown as the
  // Calibrate row's hint and used to pre-fill the keypad.
  float calMassG;

  // Show each cell's share underneath the total on the DASHBOARD, rather than
  // only on the Diagnose page. Display only, and off by default: the dashboard
  // is meant to answer one question, and three supporting numbers on it are for
  // somebody levelling a platform rather than somebody weighing a bowl.
  //
  // It earns its place there in spite of that, because levelling means watching
  // the corners WHILE loading the platform, and a breakdown two menus away gets
  // read after the load has moved.
  bool showCells;

  uint8_t online;
};

struct State {
  Level levels[LEVELS];
  uint8_t stackCount;
  Stack stack;

  bool sensorOnline[LEVELS];
  uint8_t sensorsOnline;

  Battery battery;
  uint16_t batteryMv;

  // Percentage from the measured discharge curve, or -1 for unknown.
  //
  // LOCAL ONLY, and that distinction is the whole reason it can be here at all.
  // device_status.h computes this and prints it to the console but never sends
  // it upstream: a resting-voltage estimate moves several points with load,
  // temperature, cell age and per-unit ADC calibration, so publishing a number
  // would invite a dashboard to render precision the measurement lacks.
  //
  // The battery detail page has the same audience as that console line -- a
  // person standing at the device with a multimeter -- so it shows the same
  // figures. The STATUS BAR still shows only the band, because that is the
  // glanceable surface and the argument against a number holds there.
  int8_t batteryPercent;

  // Raw millivolts at the ADC pin, before the divider. This is what you compare
  // against a multimeter to derive BOWLSTACK_BATTERY_CAL, and an implausible
  // value here identifies a wiring fault that the cell figure would disguise.
  uint16_t batteryPinMv;

  // Tri-state on purpose. The Waveshare board cannot read charge state at all
  // -- the ETA6098's STAT output never reaches a GPIO -- and this codebase does
  // not state what it cannot measure. A plain `bool charging` would force a
  // claim; `chargingKnown == false` is the honest answer.
  bool chargingKnown;
  bool charging;

  // ON MAINS. A DIFFERENT FACT FROM `charging`, and the reason both exist.
  //
  // `charging` is the ETA6098 pushing current into the cell, read from its STAT
  // pin -- which on this board reaches no GPIO unless the mod in
  // board_waveshare_s3.h section 5 has been fitted, so it is normally unknown.
  // This is VBUS presence, measured through a 5k/10k divider on IO10, and it is
  // known on every board.
  //
  // They diverge exactly when the battery fills: the charger terminates, STAT
  // releases, and 5 V is still there. Which is why the STATUS BAR's bolt hangs
  // off this one and the `charging` COLUMN in Supabase hangs off the other -- a
  // bolt means "plugged in", the same thing it means on a phone, and that is
  // true at 100%. A database field called `charging` saying true into a full
  // battery would not be.
  bool externalPower;

  bool wifiConnected;

  // Signal strength in dBm, for the status bar's bars. 0 means "connected but
  // strength not yet read", which is distinct from a genuinely weak -90.
  int16_t wifiRssi;

  // THE DEVICE HAS NO CLOCK. There is no RTC on this board -- the schematic
  // carries a QMI8658 IMU and nothing else on that bus -- and docs/supabase.md
  // already builds the whole telemetry design around the device having no
  // reliable time: events are backdated from a millis() age and the SERVER
  // clock is authoritative.
  //
  // So the status bar's time is genuinely unknown until something supplies it,
  // and `timeKnown` exists to say so rather than let the UI display 00:00 and
  // have it read as midnight. Consistent with the rest of this codebase: no
  // cell detected reports null, not 0%; a sensor that has not concluded reports
  // unknown, not "no bowl".
  bool timeKnown;
  uint8_t hh;
  uint8_t mm;

  uint32_t uptimeSec;
  const char *deviceId;
  const char *firmware;

  // Carried in the same snapshot as everything else rather than plumbed
  // separately, so one struct still describes the whole screen and a page can
  // never render half of one instant beside half of another.
  ScaleView scale;

  // --- the panel knob -------------------------------------------------------
  // Signed detents since boot. Not wrapped, not clamped: the dashboard shows it
  // raw because the only question it currently answers is "is the encoder
  // wired", and a figure that had been massaged could not answer it.
  int32_t encoderPos;

  // A MONOTONIC COUNTER, NOT A `bool pressed`, and the difference is the whole
  // reason a press ever shows on screen.
  //
  // This struct is a SNAPSHOT, pushed at 20 Hz. A tap on a tactile switch is
  // 30-80 ms of contact, so a boolean sampled every 50 ms lands inside the
  // press sometimes and outside it others -- the dot would appear for some taps
  // and not for others, which reads as a flaky switch rather than as a sampling
  // artefact. A counter cannot miss an edge: whatever the snapshot rate, the UI
  // sees the number has moved and knows exactly how many presses it missed.
  //
  // The 0.5 s the dot stays lit is decided by the UI, not by the platform. How
  // long a confirmation blink lasts is a presentation question, and pushing a
  // pre-computed `showDot` across this seam would put a timer in the sensing
  // task to serve a widget.
  uint32_t encoderPressCount;

  // --- TRIAL HARNESS: the manual fill estimate ------------------------------
  // TEMPORARY, AND FOR ONE DEVICE. LDC-001 carries a knob so the counter
  // attendant can eyeball the vessel and dial in how full it is, while the load
  // cell underneath keeps measuring independently. The point of the exercise is
  // the GAP between the two -- it is what tells a reviewer whether a knob and a
  // person are an acceptable substitute for a scale, or a false economy.
  //
  // KEPT SEPARATE FROM EVERY MEASURED FIELD, deliberately. Nothing here feeds
  // the weight, the slot totals or the burn rate: an experiment whose subject
  // is allowed to contaminate its own control measures nothing.
  //
  // fillKnown is false on a device with no knob, which is every device but one.
  // It is not "0%" -- that would be a claim that the vessel is empty.
  bool fillKnown;
  uint8_t fillPercent;       // 0..100
  bool fillReminderDue;      // the estimate is older than the reminder interval

  // Is fillAgeSec MEANINGFUL? False after a restore from NVS, and it cannot be
  // otherwise -- no RTC, so nothing here can measure wall time across a power
  // cycle. The screen must say "not confirmed since restart" rather than print
  // an uptime as though somebody had set the knob that recently.
  bool fillAgeKnown;

  // Which page the device settles on: 0 = weight, 1 = knob. A SETTING rather
  // than a measurement, carried here because State is already the one snapshot
  // that describes the screen -- the scale's own display settings ride along
  // the same way. The platform owns the NVS copy; the UI only renders it and
  // asks for it to be cycled.
  uint8_t defaultPage;
  uint32_t fillAgeSec;       // how stale it is, for the screen to say so
};

// A sensible zero value: everything unknown, nothing claimed. Used as the
// state before the first real sample arrives, so the screen never shows a
// confident zero it has not earned.
State unknownState();

}  // namespace ui
