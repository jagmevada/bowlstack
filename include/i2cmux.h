// TCA9548A 1-to-8 I2C switch -- what lets three fixed-address converters share
// one bus.
//
// THE NAU7802 ANSWERS AT 0x2A AND NOTHING MOVES IT: no address pins, no strap,
// no OTP field. Two cells were therefore two buses, one of them bit-banged; a
// third would have been a third, off the same loop that draws the screen. The
// mux replaces that whole argument with one hardware port and one channel per
// cell, and the converters cannot hear each other at all.
//
// IT IS A SWITCH, NOT A BUFFER, and every consequence below follows from that:
//
//   * The channels are pass-gates carrying NO pull-ups of their own. Each
//     enabled stub becomes electrically part of the upstream bus, so every stub
//     needs its own 4.7k pair -- fitted on the converter breakout or fitted by
//     hand. The upstream resistors do not reach through the gate properly, and
//     the symptom of relying on them is intermittent NAKs on one channel that
//     read as a flaky converter.
//
//   * A stub that latches SDA low still stops THAT channel dead. What it can no
//     longer do is stop the others -- which is the containment two separate
//     buses were bought for, now bought per cell instead of per pair.
//
//   * Exactly one channel is enabled at a time here. The part can enable
//     several, and doing so would put two 0x2A converters on the bus together,
//     both acknowledging, both clocking out data over each other. Never widen
//     the mask.
//
// ONE MUX, MODULE-LEVEL STATE, no object. There is precisely one of these in
// the product and both callers -- the converter driver and the boot scan -- sit
// in different translation units; handing the same instance to both would be
// ceremony around a singleton that is already a singleton.

#pragma once

#include <stdint.h>

namespace i2cmux {

// Channels this part has. The cells use 0, 1 and 2; see board_waveshare_s3.h.
static const uint8_t CHANNELS = 8;

// `i2cPort` is an lgfx port -- see nau7802.h for why an int and not a TwoWire.
// Does not touch the bus; begin() does that.
void configure(int i2cPort, uint8_t addr, uint32_t freqHz);

// Probes the part and leaves EVERY CHANNEL DISABLED, which is the state the
// boot scan wants: an upstream scan that finds 0x70 and nothing else proves the
// switch is closed, so a 0x2A seen later is definitely a cell behind a channel
// and not something wired straight to the trunk.
//
// Returns false when nothing acknowledged at the mux address.
bool begin();

// False until begin() has seen the part acknowledge, and again after any failed
// transaction -- so "did the mux answer" and "is the mux still answering" are
// the same question.
bool present();

// Enables exactly one channel, or disables all when `channel` is negative.
//
// CACHED. A repeated select of the channel already enabled costs nothing, which
// matters because the converter driver calls this ahead of every register
// access -- two per poll per cell -- and most of those are the second access of
// a pair on a channel that is already live.
bool select(int8_t channel);

// Forget which channel is believed to be enabled, so the next select() writes
// the control register whatever it holds.
//
// CALLED AFTER ANY FAILED DOWNSTREAM TRANSACTION, and that is the whole point
// of exposing it. A converter that NAKs has probably just NAKed -- but a mux
// that browned out, glitched or was reset has silently dropped every channel,
// and from upstream the two look identical. Re-selecting costs one write and is
// the only thing that recovers the second case.
void invalidate();

int port();
uint8_t addr();

// The control byte the part is believed to hold, or 0xFF when that is unknown.
// Boot diagnostics only.
uint8_t control();

}  // namespace i2cmux
