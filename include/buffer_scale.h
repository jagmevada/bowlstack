// The 200 kg buffer-stock scale: ONE load cell, on its own bus.
//
// A different instrument from scale.h, not a fourth corner of it, and the
// differences are the design:
//
//   * ONE cell under a shelf of stock, so there is nothing to sum and no corner
//     to level. scale.h is three cells under one platform and says why at length.
//   * IT MEASURES TOTAL STOCK, INCLUDING WHAT IS ALREADY ON IT. The counter's
//     scale zeroes itself at power-up to whatever tray is sitting there, which is
//     right for a serving station and WRONG here: a buffer that is half full at
//     boot would read empty. So there is no session tare. The zero is the reading
//     of the EMPTY platform, taken once at commissioning and kept in NVS, like
//     the counter's platform zero.
//   * ITS OWN BUS, ITS OWN TASK, ITS OWN NVS NAMESPACE. Nothing here shares a
//     register, a mutex or a flash key with scale.cpp, so it can be wrong,
//     slow or absent without the counter noticing. See board_waveshare_s3.h
//     section 10 for why it is on IO21/IO16 rather than the trunk.
//
// SAME RULES AS EVERYTHING ELSE: one task owns the converter and publishes
// immutable snapshots under a mutex; NVS is written only from that task, with the
// UI/console only raising a flag; and nothing is claimed that was not measured --
// a cell that is not converting reports no counts, an uncalibrated one reports no
// kilograms, an unzeroed one reports no kilograms either.
//
// IT REPORTS COUNTS UNTIL CALIBRATED, exactly as the counter does. Zero it with
// the platform EMPTY, put a known mass on, then calibrate.
//
// NOT IN THIS FILE, DELIBERATELY: any screen, and any upload. Both need a decision
// about how the number is shown and where it goes, and neither is needed to know
// whether the cell works.

#pragma once

#include <stdint.h>

#include "nau7802.h"

namespace bufscale {

struct Snapshot {
  // True once the mux answered at boot. FALSE means no module is fitted -- the
  // normal state of every unit that does not carry a buffer cell -- and every
  // other field is then meaningless. Kept apart from `state` so "nothing there"
  // and "there but not converting" are different sentences.
  bool fitted;

  CellState state;
  uint8_t revision;   // 0xFF = never read

  // ZERO unless state == Online. A stale last-known figure beside a state that
  // says the cell stopped would read as a live one.
  int32_t rawCounts;  // the latest single conversion, unfiltered
  int32_t counts;     // trimmed mean of the window, absolute -- before the zero
  int32_t pp;         // peak-to-peak of the window: the figure that says it is alive
  uint16_t sps;       // conversions per second actually delivered
  uint8_t samples;    // how many are in the window right now

  bool zeroed;        // an empty-platform zero is stored
  int32_t zero;
  bool calibrated;
  float countsPerGram;

  // The mass on the platform. MEANINGFUL ONLY WHEN `kgKnown`. Zero is a real
  // weight -- an empty shelf -- so it cannot double as "unknown".
  bool kgKnown;
  float grams;

  // The conversion has reached the end of the 24-bit range, so the cell is
  // reporting its own ceiling rather than a weight.
  bool overRange;

  uint32_t seq;
};

// Probes the module and, if it is there, opens the bus, restores the zero and
// calibration from NVS and starts the task. Safe to call on a unit with no module:
// it says so once and starts nothing. Called from setup(), after scale::begin().
void begin();

// A copy, taken under the mutex.
Snapshot snapshot();

// --- commands -------------------------------------------------------------
// All three only RAISE A FLAG and return. The work, and the NVS write, happen on
// the buffer task: a Preferences handle written from two tasks corrupts rather
// than races, which is the reason scale.cpp does the same. The outcome of each is
// printed to the console by the task.

// Store the current reading as the EMPTY-platform zero. The platform must be
// empty, and the window must be full.
void setZero();

// Derive counts-per-gram from a known mass that is on the platform now. Needs a
// zero first. Refusals say why on the console rather than failing quietly.
void calibrate(float knownGrams);

// Forget the calibration; the reading goes back to counts. The zero is kept.
void clearCalibration();

// The reference mass the console's calibrate key uses -- a build constant, like
// the counter's, because it is a mass somebody physically owns.
float calMassG();

// --- console --------------------------------------------------------------
// One line for the 5-second status block, in the same shape as the cells'.
void printStatus();
// Help text for the '?' screen.
void printHelp();
// Handles 'z' (zero), 'g' (calibrate), 'k' (clear calibration). Returns true if
// the key was one of them, so the caller's switch can leave everything else alone.
// Returns false for EVERY key on a unit with no module fitted, and prints nothing:
// a key that does nothing must not echo as though it did.
bool consoleKey(int c);

// Stack still unused, in BYTES. 0 if the task never started.
uint32_t stackFreeBytes();

}  // namespace bufscale
