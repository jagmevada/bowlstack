// NAU7802 24-bit bridge ADC -- one load cell each.
//
// WRITTEN HERE RATHER THAN PULLED IN, and the reason is the bus rather than the
// part. Every Arduino NAU7802 library takes a `TwoWire&`, and neither of this
// board's two cell buses is one:
//
//   cell A  sits on GPIO47/48, which belongs to LovyanGFX. That port is driven
//           by lgfx's own register-level I2C code, not by the ESP-IDF driver
//           behind TwoWire. Opening `Wire` on those pins puts two drivers on one
//           peripheral, and config.h already records what that costs -- the
//           touch controller silently stops answering and the display keeps
//           working, so it presents as "touch broke" with nothing pointing at
//           the cause.
//
//   cell B  sits on GPIO11/12 with no peripheral at all, bit-banged.
//
// What both DO have is `lgfx::i2c`, which reaches a hardware port by a
// non-negative number and a bit-banged one by a negative number, through one
// identical set of calls. So the bus abstraction this driver needs is an int,
// and the driver stays a driver instead of growing a porting layer.
//
// THE ADDRESS IS FIXED AT 0x2A. There are no address pins, no strap and no OTP
// field to move it, which is why two cells means two buses -- see section 8 of
// board_waveshare_s3.h.

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
  void configure(int i2cPort, uint32_t freqHz, const char *name);

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
  const char *name() const { return name_; }

  // Re-runs the part's internal offset calibration. Distinct from Scale's tare:
  // this zeroes the ADC's own input offset with the PGA inputs shorted, whereas
  // a tare subtracts the weight of whatever is sitting on the platform. Doing
  // one when you meant the other is the classic way to end up with a scale that
  // reads plausibly and wrongly.
  bool calibrateAfe();

 private:
  bool read(uint8_t reg, uint8_t *buf, uint8_t len);
  bool write(uint8_t reg, uint8_t val);
  bool setBit(uint8_t reg, uint8_t bit, bool on);
  bool waitBit(uint8_t reg, uint8_t bit, bool want, uint32_t timeoutMs);

  int port_ = 0;
  uint32_t freq_ = 400000;
  const char *name_ = "?";

  CellState state_ = CellState::Offline;
  int32_t counts_ = 0;
  uint32_t lastSampleMs_ = 0;
  uint8_t revision_ = 0xFF;

  uint16_t sps_ = 0;
  uint16_t sampleCount_ = 0;
  uint32_t spsWindowMs_ = 0;

  // Consecutive failed register reads before the cell is declared Offline. Same
  // reasoning as config::IO_FAILURES_TO_OFFLINE: one NAK on a bit-banged bus
  // with weak pull-ups is noise, five in a row is a fault.
  uint8_t ioFailures_ = 0;
};
