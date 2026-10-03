// The buffer bank: every buffer-stock platform on this board's buffer bus.
//
// REPLACES buffer_scale.*, the single-cell bring-up written for the field trial.
// Each platform is a UNIT with its own id (BWL-001, BWL-002, ...) even though one
// ESP32 hosts them all -- the database, the web app and the panel see one row per
// platform, not one per board. Which platform sits on which mux channel, under which
// id and label, is CONFIGURATION (compiled defaults, overridable from the console and
// kept in NVS), not code.
//
// THREE LAYERS, as include/load_scale.h describes:
//   source   a NAU7802 behind a channel of this bus's own TCA9548A (IO21/IO16,
//            bit-banged -- see board_waveshare_s3.h section 10). The next hardware
//            replaces this with ATtiny3224 nodes on RS485; nothing above it changes.
//   core     lscale::LoadScale -- filter, step detection, zero/factor, the bowl rule.
//   bank     this file: one task owning the bus, the table, NVS, the snapshot.
//
// SAME RULES AS THE COUNTER: one task owns the bus and the converters; state crosses
// to other tasks only as a snapshot under a mutex; NVS is written only from that
// task, with commands queued to it; nothing is claimed that was not measured.

#pragma once

#include <stdint.h>

#include "load_scale.h"

namespace bufbank {

// Buffer platforms this board can host on its buffer bus. Three is the next phase's
// count; the mux has eight channels if that ever grows.
static const uint8_t SLOTS = 3;

// A plain aggregate -- no member initialisers -- so the compiled defaults can be a
// brace-initialised table under the ESP32 toolchain's C++11. Wherever one is
// embedded it is value-initialised (`{}`), so an unset config is zeros, never junk.
struct SlotConfig {
  bool enabled;
  char uid[12];    // the platform's id upstream, e.g. "BWL-001"
  char label[4];   // what the panel calls it, e.g. "B1"
  int8_t muxCh;    // channel on the buffer bus's TCA9548A; -1 = none
};

struct SlotSnapshot {
  SlotConfig cfg{};
  // A converter answered behind its channel at boot. FALSE for an enabled slot with
  // nothing plugged in -- which is normal, not a fault -- and every other field is
  // then meaningless. Kept apart from the reading's link state so "nothing there"
  // and "there but not converting" are different sentences.
  bool fitted = false;
  lscale::Reading r;
  uint8_t revision = 0xFF;
  uint16_t sps = 0;
  // The outcome of the last command sent to this slot, for the panel to show, and a
  // counter that moves every time one lands so a page can tell old news from new.
  char lastResult[48] = {0};
  uint32_t resultSeq = 0;
};

struct Snapshot {
  bool busOk = false;    // the soft I2C port initialised
  bool muxOk = false;    // the bus's TCA9548A answered at boot
  uint8_t fitted = 0;    // how many slots have a converter
  SlotSnapshot slot[SLOTS];
  uint32_t seq = 0;
};

// Probes the bus and every enabled slot, restores each slot's zero, factor and bowl
// count from NVS, and starts the task. On a board with no buffer module it says so
// in one line and starts nothing. Call from setup(), after scale::begin().
void begin();

// A copy, taken under the mutex.
Snapshot snapshot();

// --- commands: QUEUED to the bank task, which does the work and the NVS write and
// records the outcome in that slot's lastResult. They return at once.
void zero(uint8_t slot);                     // the platform must be EMPTY
void calibrate(uint8_t slot, float knownG);  // known mass on the platform now
void clearCalibration(uint8_t slot);
void setBowls(uint8_t slot, uint8_t n);      // operator correction; confirms the count

// The mass the console's calibrate key uses, from -DBOWLSTACK_BUF_CAL_MASS_G.
float defaultCalMassG();

// --- console ---------------------------------------------------------------------
void printStatus();      // the 5-second status block: one line per fitted slot
void printHelp();        // the '?' screen
// Single keys z / g / k act on B1 (slot 0), as on the field-trial build. Returns true
// if the key was one of them.
bool consoleKey(int c);
// ':' starts a command line (":buf ..."); while one is open every byte belongs to
// it, so the caller must route bytes here first. Returns true if it consumed `c`.
bool consoleFeed(int c);

uint32_t stackFreeBytes();

}  // namespace bufbank
