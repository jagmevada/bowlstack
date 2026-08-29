#include "i2cmux.h"

// Same bus layer as the converter driver, and for the same reason: one call set
// reaches a hardware peripheral and a bit-banged pair alike, and on a port
// LovyanGFX also uses it is the same code path rather than a second driver on
// one peripheral. See nau7802.h.
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

namespace i2cmux {
namespace {

int port_ = 1;
uint8_t addr_ = 0x70;
uint32_t freq_ = 400000;

bool present_ = false;

// The control byte the part is BELIEVED to hold. 0xFF is impossible as a real
// value here -- this code never enables more than one channel -- so it doubles
// as "unknown" without needing a second flag.
const uint8_t UNKNOWN = 0xFF;
uint8_t control_ = UNKNOWN;

// A single-byte write with no register address. The TCA9548A has exactly one
// register and no pointer, so writeRegister8() -- which sends a register byte
// first -- would set the control byte to the register number and then leave a
// stray data byte on the bus.
bool writeControl(uint8_t v) {
  return lgfx::i2c::transactionWrite(port_, addr_, &v, 1, freq_).has_value();
}

}  // namespace

void configure(int i2cPort, uint8_t addr, uint32_t freqHz) {
  port_ = i2cPort;
  addr_ = addr;
  freq_ = freqHz;
  present_ = false;
  control_ = UNKNOWN;
}

bool begin() {
  present_ = false;
  control_ = UNKNOWN;

  // Written rather than read, and that is deliberate. A read of this part
  // returns the control byte, which is 0x00 on a cold part -- indistinguishable
  // from a read that failed and left the buffer alone. Writing 0x00 and having
  // it acknowledged is a positive answer: something on the bus took a byte.
  if (!writeControl(0x00)) return false;

  present_ = true;
  control_ = 0x00;
  return true;
}

bool present() { return present_; }

bool select(int8_t channel) {
  const uint8_t want = (channel < 0) ? 0x00 : (uint8_t)(1u << (channel & 0x07));

  // The cache. Not an optimisation for its own sake -- see the header: the
  // converter driver calls this before every register access, and without it a
  // 10 SPS poll of three cells would put sixty extra transactions a second on
  // the bus to say nothing.
  if (present_ && control_ == want) return true;

  if (!writeControl(want)) {
    // BOTH FLAGS DROP. Leaving `control_` set would have the next select()
    // believe a channel is live that the failed write never enabled, and the
    // converter behind it would be read through whatever channel the part
    // actually holds -- a reading attributed to the wrong corner, which is the
    // one failure this whole file exists to make impossible.
    present_ = false;
    control_ = UNKNOWN;
    return false;
  }

  present_ = true;
  control_ = want;
  return true;
}

void invalidate() { control_ = UNKNOWN; }

int port() { return port_; }
uint8_t addr() { return addr_; }
uint8_t control() { return control_; }

}  // namespace i2cmux
