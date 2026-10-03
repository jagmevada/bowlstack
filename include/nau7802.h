// NAU7802 24-bit bridge ADC -- one load cell each.
//
// WRITTEN HERE RATHER THAN PULLED IN, and the reason is the bus rather than the
// part. Every Arduino NAU7802 library takes a `TwoWire&`, and this board's cell
// bus is not one: the cells sit on GPIO12/11, reached through `lgfx::i2c`,
// which is also what drives the touch controller on the other port. Opening
// `Wire` anywhere near a port LovyanGFX owns puts two drivers on one
// peripheral, and config.h already records what that costs -- the touch chip
// silently stops answering while the display keeps working, so it presents as
// "touch broke" with nothing pointing at the cause.
//
// `lgfx::i2c` reaches a hardware port by a non-negative number and a bit-banged
// one by a negative number through one identical set of calls, so the bus
// abstraction this driver needs is an int, and the driver stays a driver
// instead of growing a porting layer.
//
// THE ADDRESS IS FIXED AT 0x2A. There are no address pins, no strap and no OTP
// field to move it. Three cells therefore sit behind a TCA9548A, one per
// channel, and this driver selects its own channel before touching the bus --
// see i2cmux.h and section 8 of board_waveshare_s3.h.

#pragma once

#include <stdint.h>

// Mirrors SensorState in sensor_array.h deliberately: the same three-way
// vocabulary, so the UI adapter reads the same way for a cell as for a ToF
// sensor, and "has not concluded yet" stays distinct from "not working".
enum class CellState : uint8_t {
  Offline,  // nothing answered at 0x2A, or the part stopped answering
  Warming,  // configured and converting, but no conversion has completed yet
  Online,   // producing conversions
};

class Nau7802 {
 public:
  // `i2cPort` is an lgfx port: >= 0 for a hardware peripheral, < 0 for one of
  // the bit-banged slots (-1, -2). `name` is used only for console lines.
  //
  // `muxChannel` is the TCA9548A channel this converter sits behind, or -1 for
  // one wired straight to the bus. Defaulted so the driver still describes a
  // part on a plain bus -- the mux is this board's answer to a fixed address,
  // not a property of the NAU7802.
  void configure(int i2cPort, uint32_t freqHz, const char *name, int8_t muxChannel = -1);

  // Resets, powers up, configures gain and rate, and runs the internal offset
  // calibration. Blocking -- it waits on the part's own ready and calibration
  // flags -- and called once from setup(), never from the loop.
  //
  // Returns false when nothing acknowledged at 0x2A, when the part never
  // reported ready, or when its self-calibration reported an error. In every
  // one of those cases the cell is left Offline rather than half-configured.
  bool begin();

  // Non-blocking. Reads the conversion-ready flag and, only if it is set,
  // fetches the 24-bit result. Costs one register read when no sample is
  // waiting, which is what lets it be called from the UI loop at frame rate.
  //
  // Returns true when a NEW sample was taken.
  bool poll(uint32_t nowMs);

  CellState state() const { return state_; }
  bool online() const { return state_ == CellState::Online; }

  // Signed 24-bit conversion, sign-extended. This is the raw part: no tare, no
  // scale factor, no filtering. Scale owns all three.
  int32_t counts() const { return counts_; }
  uint32_t lastSampleMs() const { return lastSampleMs_; }

  // Completed conversions per second, measured over the last second rather than
  // assumed from the configured rate. The configured rate is what the part was
  // ASKED for; this is what the bus and the loop actually delivered, and on a
  // bit-banged bus behind a rendering loop those are not the same claim.
  uint16_t sps() const { return sps_; }

  // Silicon revision from register 0x1F, or 0xFF if it was never read. Reported
  // at boot because "0x2A acknowledged but the revision reads 0xFF" is the
  // signature of a bus that is wired but not working, which an ack alone hides.
  uint8_t revision() const { return revision_; }

  int port() const { return port_; }
  int8_t muxChannel() const { return muxChannel_; }
  const char *name() const { return name_; }

  // Re-runs the part's internal offset calibration. Distinct from Scale's tare:
  // this zeroes the ADC's own input offset with the PGA inputs shorted, whereas
  // a tare subtracts the weight of whatever is sitting on the platform. Doing
  // one when you meant the other is the classic way to end up with a scale that
  // reads plausibly and wrongly.
  bool calibrateAfe();

  // Reads back every register begin() configured and checks it holds what was
  // written: powered up and ready, internal LDO at 3.0 V, gain 128, the configured
  // rate, chopper off, PGA LDO mode, PGA cap, converting. Returns false with a
  // short reason in `why`.
  //
  // WHY IT EXISTS: begin() configures with read-modify-writes. On a bus that has
  // just come out of a mid-transaction reset, a READ can return garbage (the 200 kg
  // buffer cell was seen reading its revision register as 0x00), and a
  // read-modify-write built on a garbage read writes garbage back -- a converter
  // that "came up" with the wrong gain. Checking the result is the only way to
  // know. Additive: the counter's cells do not call it.
  bool verifyConfig(char *why, unsigned whyLen);

  // One register read, for bus-reliability tests. Same path as every other read.
  bool readRegister(uint8_t reg, uint8_t *value);

  // --- boot diagnostics ------------------------------------------------------
  // BLOCKING, and called once from setup() before the scale task exists.
  //
  // It exists to answer one question that no amount of staring at the dashboard
  // can: when a cell converts happily and its number does not move with load,
  // is the CONVERTER not seeing a signal, or is the CELL not producing one?
  // Those have completely different fixes -- one is wiring, the other is a
  // mounting or a dead gauge -- and from the outside they look identical.
  //
  // Three measurements, printed with their peak-to-peak:
  //
  //   bridge     what the part reads normally
  //   shorted    the same with the PGA's own inputs internally shorted
  //              (I2C_CTRL bit 3). If this is close to the bridge reading AND
  //              the bridge reading is quiet, the input already looks like a
  //              short to the converter -- i.e. nothing is wired to it, or the
  //              bridge is open, or the excitation is dead.
  //   channel 2  the part's second input pair. A bridge landed on VIN2 instead
  //              of VIN1 reads exactly like a disconnected cell on channel 1,
  //              and this is the only cheap way to find that out.
  //
  // PEAK-TO-PEAK IS THE FIGURE THAT MATTERS. A mean can sit anywhere; what
  // separates a live 350 ohm bridge from an open input is how much the reading
  // moves between conversions, and whether it moves at all.
  //
  // `samples` per measurement and `includeChannel2` both exist because THE COST
  // IS SET BY THE OUTPUT RATE, and this firmware runs at 10 SPS on purpose. Every
  // sample is 100 ms of wall clock, so the full 16-sample three-part test is
  // ~5.4 s per cell -- 16 s of boot on three cells, which is what it measured
  // before these two arguments existed. Boot asks for a short bridge-vs-shorted
  // pair; the console's 's' asks for the full-depth version when somebody is
  // actually diagnosing.
  void selfTest(uint8_t samples = 16, bool includeChannel2 = true);

 private:
  // Blocking: waits for `n` completed conversions and returns their mean and
  // peak-to-peak. Boot only -- it spins on the ready flag, which is exactly
  // what poll() exists to avoid doing on the render loop.
  bool sampleStats(uint8_t n, int32_t *mean, int32_t *pp, uint32_t timeoutMs);

  // Points the mux at this cell's channel. Called by read() and write() rather
  // than by their callers, so there is no path to the converter that can forget
  // to do it -- which would read one cell's conversion and file it under
  // another's name, the one failure a mux introduces that two buses could not.
  bool selectBus();

  bool read(uint8_t reg, uint8_t *buf, uint8_t len);
  bool write(uint8_t reg, uint8_t val);
  bool setBit(uint8_t reg, uint8_t bit, bool on);
  bool waitBit(uint8_t reg, uint8_t bit, bool want, uint32_t timeoutMs);

  int port_ = 0;
  uint32_t freq_ = 400000;
  int8_t muxChannel_ = -1;
  const char *name_ = "?";

  CellState state_ = CellState::Offline;
  int32_t counts_ = 0;
  uint32_t lastSampleMs_ = 0;
  uint8_t revision_ = 0xFF;

  uint16_t sps_ = 0;
  uint16_t sampleCount_ = 0;
  uint32_t spsWindowMs_ = 0;

  // Consecutive failed register reads before the cell is declared Offline. Same
  // reasoning as config::IO_FAILURES_TO_OFFLINE: one NAK on a stub whose
  // pull-ups are marginal is noise, five in a row is a fault.
  uint8_t ioFailures_ = 0;
};
