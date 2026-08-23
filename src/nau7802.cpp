#include "nau7802.h"

#include <Arduino.h>

// The bus layer. See the header for why this is lgfx rather than Wire: one call
// set reaches both a hardware peripheral (port >= 0) and a bit-banged pair
// (port < 0), and on port 0 it is the same code path the touch controller
// itself uses, so there is never a second driver on that peripheral.
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "board_waveshare_s3.h"

namespace {

// --- register map ----------------------------------------------------------
// From the NAU7802 datasheet (Rev 1.0, 2019). Named rather than written as
// literals at the call sites, because a load-cell front end is a part where a
// wrong register writes a plausible number instead of failing.
const uint8_t REG_PU_CTRL = 0x00;
const uint8_t REG_CTRL1 = 0x01;
const uint8_t REG_CTRL2 = 0x02;
const uint8_t REG_I2C_CTRL = 0x11;
const uint8_t REG_ADCO_B2 = 0x12;  // 0x12/0x13/0x14, MSB first
const uint8_t REG_ADC = 0x15;
const uint8_t REG_PGA = 0x1B;
const uint8_t REG_POWER = 0x1C;
const uint8_t REG_REVISION = 0x1F;

// PU_CTRL
const uint8_t PU_RR = 0;    // register reset
const uint8_t PU_PUD = 1;   // power up digital
const uint8_t PU_PUA = 2;   // power up analog
const uint8_t PU_PUR = 3;   // power up ready (read-only)
const uint8_t PU_CS = 4;    // cycle start
const uint8_t PU_CR = 5;    // cycle ready (read-only) -- the data-ready flag
const uint8_t PU_AVDDS = 7; // AVDD source: 1 = internal LDO

// CTRL2
const uint8_t C2_CALS = 2;      // start calibration; self-clears when done
const uint8_t C2_CAL_ERR = 3;   // calibration failed
const uint8_t C2_CHS = 7;       // 0 = input pair 1, 1 = input pair 2

// I2C_CTRL
const uint8_t IC_SI = 3;        // short the PGA inputs together, internally

// PGA (0x1B)
const uint8_t PGA_LDOMODE = 6;

// POWER (0x1C)
const uint8_t PWR_PGA_CAP_EN = 7;

// --- configured operating point --------------------------------------------
//
// GAIN 128. A 20 kg cell at 2 mV/V excited from a 3 V rail puts 6 mV across the
// bridge at full load, against the converter's ~+/-VREF full scale. Anything
// less than the maximum gain throws away most of the 24 bits before the signal
// ever reaches them.
const uint8_t GAIN_128 = 0b111;

// LDO 3.0 V, NOT 3.3 V, and this is the one setting most likely to be copied
// wrong from an example.
//
// The part's own LDO supplies AVDD, which is both the bridge excitation and the
// converter's reference -- so the measurement is RATIOMETRIC and a sagging rail
// is not an error that cancels. This board runs the NAU7802 from 3.3 V. Asking
// a 3.3 V input for a 3.3 V output leaves the regulator no dropout headroom at
// all, so it stops regulating and follows the input instead, and every drop on
// the USB or battery rail lands directly on the gain. 3.0 V leaves 300 mV,
// which the part is specified to work with.
//
// The cost is 10% of span, i.e. one part in ten of a 24-bit number. It is not
// close.
const uint8_t LDO_3V0 = 0b101;

// 80 SPS, and it is deliberately not 320.
//
// The screen refreshes at 10-15 fps. 80 SPS is already five to eight samples
// per displayed frame -- enough to average and still show a value from the
// current frame -- while 320 SPS would deliver samples nobody can see and cost
// four times the noise per sample, since converter noise falls with integration
// time. "Maximum refresh rate" is a property of the panel here; the converter
// stopped being the limit two settings ago.
//
// Raise it if a future feature actually consumes samples faster than the panel:
// 0b111 is 320 SPS.
const uint8_t RATE_80SPS = 0b011;
const uint16_t RATE_80SPS_HZ = 80;

// A cell that acknowledged at boot but has completed no conversion in this long
// has stopped converting. Same failure the ToF array needs SENSOR_STALE_MS for:
// the registers still answer, so the read path cannot see it.
const uint32_t CELL_STALE_MS = 2000;

const uint8_t IO_FAILURES_TO_OFFLINE = 5;

}  // namespace

void Nau7802::configure(int i2cPort, uint32_t freqHz, const char *name) {
  port_ = i2cPort;
  freq_ = freqHz;
  name_ = name;
}

bool Nau7802::read(uint8_t reg, uint8_t *buf, uint8_t len) {
  const bool ok =
      lgfx::i2c::transactionWriteRead(port_, board::NAU7802_ADDR, &reg, 1, buf, len, freq_)
          .has_value();
  if (ok) {
    ioFailures_ = 0;
  } else if (ioFailures_ < 0xFF) {
    ioFailures_++;
  }
  return ok;
}

bool Nau7802::write(uint8_t reg, uint8_t val) {
  // mask 0 means "replace", not "or into". lgfx's writeRegister8 computes
  // (current & mask) | data, so a non-zero mask is a read-modify-write and 0 is
  // a plain store -- which is what every call below wants.
  return lgfx::i2c::writeRegister8(port_, board::NAU7802_ADDR, reg, val, 0, freq_).has_value();
}

bool Nau7802::setBit(uint8_t reg, uint8_t bit, bool on) {
  uint8_t v = 0;
  if (!read(reg, &v, 1)) return false;
  const uint8_t mask = (uint8_t)(1u << bit);
  const uint8_t next = on ? (uint8_t)(v | mask) : (uint8_t)(v & ~mask);
  if (next == v) return true;
  return write(reg, next);
}

bool Nau7802::waitBit(uint8_t reg, uint8_t bit, bool want, uint32_t timeoutMs) {
  const uint32_t deadline = millis() + timeoutMs;
  for (;;) {
    uint8_t v = 0;
    if (read(reg, &v, 1)) {
      if ((bool)((v >> bit) & 1) == want) return true;
    }
    if ((int32_t)(millis() - deadline) >= 0) return false;
    delay(1);
  }
}

bool Nau7802::begin() {
  state_ = CellState::Offline;
  counts_ = 0;
  revision_ = 0xFF;
  ioFailures_ = 0;

  // THE REVISION READ IS THE PRESENCE TEST, not a bare address probe. A probe
  // proves something pulled SDA low for one bit; reading 0x1F back proves the
  // part clocked out a byte it chose. On a bus with 45k internal pull-ups those
  // are genuinely different claims, and the difference is exactly the failure
  // mode a missing 4.7k produces.
  uint8_t rev = 0;
  if (!read(REG_REVISION, &rev, 1)) {
    Serial.printf("  %s: no answer at 0x%02X on i2c port %d\n", name_, board::NAU7802_ADDR,
                  port_);
    return false;
  }
  revision_ = rev;

  // Register reset, then release it. Without this a warm reboot -- an upload, a
  // watchdog -- finds the part still configured and still converting from the
  // previous run, and the calibration below would run against a state this code
  // never set.
  if (!write(REG_PU_CTRL, (uint8_t)(1u << PU_RR))) return false;
  delay(1);
  if (!write(REG_PU_CTRL, 0x00)) return false;

  // Digital first, then analog. PUR comes up a few hundred microseconds after
  // PUD; 100 ms is far more than the part needs and is bounded so a dead cell
  // cannot hold up the boot.
  if (!write(REG_PU_CTRL, (uint8_t)((1u << PU_PUD) | (1u << PU_PUA)))) return false;
  if (!waitBit(REG_PU_CTRL, PU_PUR, true, 100)) {
    Serial.printf("  %s: powered up but never reported ready\n", name_);
    return false;
  }

  // Internal LDO as the AVDD source. On a board where the bridge is excited
  // from the part's own regulator this is what makes the reading ratiometric;
  // left at 0 the part expects AVDD to be supplied externally and the reference
  // is whatever the pin happens to sit at.
  if (!setBit(REG_PU_CTRL, PU_AVDDS, true)) return false;

  // CTRL1: VLDO in bits 5:3, gain in bits 2:0. Written as one store rather than
  // two read-modify-writes so the part never spends an instant at 3.3 V.
  if (!write(REG_CTRL1, (uint8_t)((LDO_3V0 << 3) | GAIN_128))) return false;

  // CTRL2: conversion rate in bits 6:4. The low bits are calibration control
  // and must stay clear here -- CALS is set deliberately, further down.
  if (!write(REG_CTRL2, (uint8_t)(RATE_80SPS << 4))) return false;

  // Disable the ADC chopper (bits 5:4 = 11 in register 0x15). The chopper
  // suppresses low-frequency offset drift and injects its own switching
  // artefacts; the datasheet's own recommendation for a bridge front end is to
  // turn it off, and the offset it would have chased is what the calibration
  // below removes anyway.
  if (!write(REG_ADC, 0x30)) return false;

  // PGA LDO mode: the datasheet's stability setting for the internal-LDO
  // configuration selected above. Reduces the PGA's supply current at the cost
  // of a little bandwidth, which a scale has no use for.
  if (!setBit(REG_PGA, PGA_LDOMODE, true)) return false;

  // PGA output bypass capacitor. Required, per the datasheet, whenever the PGA
  // runs at high gain -- and 128 is the highest there is.
  if (!setBit(REG_POWER, PWR_PGA_CAP_EN, true)) return false;

  if (!calibrateAfe()) return false;

  // Start converting. Everything above configures; this is the line that makes
  // the part produce data.
  if (!setBit(REG_PU_CTRL, PU_CS, true)) return false;

  // WARMING, NOT ONLINE. No conversion has completed yet, and this codebase
  // does not let "configured" pass for "measuring" -- an empty reading has to
  // be distinguishable from a measured zero, which on a scale is a real value.
  state_ = CellState::Warming;
  lastSampleMs_ = millis();
  spsWindowMs_ = lastSampleMs_;
  sampleCount_ = 0;
  sps_ = 0;

  Serial.printf("  %s: rev 0x%02X, gain 128, LDO 3.0 V, %u SPS, i2c port %d\n", name_,
                revision_, RATE_80SPS_HZ, port_);
  return true;
}

bool Nau7802::calibrateAfe() {
  // Internal offset calibration: the part shorts its own PGA inputs and stores
  // what it reads. This removes the FRONT END's offset, not the platform's
  // weight -- see the header. It is re-run whenever gain or rate change, which
  // is why it lives after those writes rather than before them.
  if (!setBit(REG_CTRL2, C2_CALS, true)) return false;

  // At 80 SPS a calibration cycle is tens of milliseconds. 1 s is a bound
  // rather than an expectation.
  if (!waitBit(REG_CTRL2, C2_CALS, false, 1000)) {
    Serial.printf("  %s: offset calibration did not finish\n", name_);
    return false;
  }

  uint8_t c2 = 0;
  if (!read(REG_CTRL2, &c2, 1)) return false;
  if ((c2 >> C2_CAL_ERR) & 1) {
    // A CAL_ERR on a bridge front end usually means the input is not where the
    // part expects it: an open bridge wire, a cell wired single-ended, or E+/E-
    // swapped. Saying so beats a bare false.
    Serial.printf("  %s: CAL_ERR set -- check the bridge wiring (E+/E-/A+/A-)\n", name_);
    return false;
  }
  return true;
}

bool Nau7802::sampleStats(uint8_t n, int32_t *mean, int32_t *pp, uint32_t timeoutMs) {
  int64_t acc = 0;
  int32_t lo = 0, hi = 0;
  uint8_t got = 0;
  bool dropFirst = true;
  const uint32_t deadline = millis() + timeoutMs;

  while (got < n && (int32_t)(millis() - deadline) < 0) {
    uint8_t pu = 0;
    if (!read(REG_PU_CTRL, &pu, 1)) return false;
    if (!((pu >> PU_CR) & 1)) {
      delay(2);
      continue;
    }

    uint8_t b[3];
    if (!read(REG_ADCO_B2, b, 3)) return false;
    int32_t raw = ((int32_t)b[0] << 24) | ((int32_t)b[1] << 16) | ((int32_t)b[2] << 8);
    raw >>= 8;

    // The first conversion after a configuration change was integrating ACROSS
    // that change, so it belongs to neither setting. Dropping it is the
    // difference between a clean before/after and a p-p figure dominated by one
    // transitional sample.
    if (dropFirst) {
      dropFirst = false;
      continue;
    }

    if (got == 0) {
      lo = hi = raw;
    } else {
      if (raw < lo) lo = raw;
      if (raw > hi) hi = raw;
    }
    acc += raw;
    got++;
  }

  if (got == 0) return false;
  *mean = (int32_t)(acc / got);
  *pp = hi - lo;
  return true;
}

void Nau7802::selfTest() {
  if (state_ == CellState::Offline) {
    Serial.printf("  %s: offline -- nothing to test\n", name_);
    return;
  }

  // Read the configuration BACK rather than trusting that the writes landed.
  // Two cells configured by the same code on two different buses should read
  // identically here; a difference means a write did not take, which is a
  // completely different fault from a cell that is not wired.
  uint8_t pu = 0, c1 = 0, c2 = 0, pga = 0, pwr = 0;
  read(REG_PU_CTRL, &pu, 1);
  read(REG_CTRL1, &c1, 1);
  read(REG_CTRL2, &c2, 1);
  read(REG_PGA, &pga, 1);
  read(REG_POWER, &pwr, 1);
  Serial.printf("  %s: PU_CTRL %02X  CTRL1 %02X  CTRL2 %02X  PGA %02X  POWER %02X\n", name_,
                pu, c1, c2, pga, pwr);

  int32_t mBridge = 0, ppBridge = 0;
  const bool okBridge = sampleStats(16, &mBridge, &ppBridge, 1500);
  if (okBridge) {
    Serial.printf("  %s: bridge     mean %9ld   p-p %7ld\n", name_, (long)mBridge,
                  (long)ppBridge);
  } else {
    Serial.printf("  %s: bridge     NO CONVERSIONS\n", name_);
  }

  // --- the PGA's own inputs, shorted together -------------------------------
  int32_t mShort = 0, ppShort = 0;
  bool okShort = false;
  if (setBit(REG_I2C_CTRL, IC_SI, true)) {
    delay(60);  // two conversion periods at 80 SPS, so the change has landed
    okShort = sampleStats(16, &mShort, &ppShort, 1500);
    setBit(REG_I2C_CTRL, IC_SI, false);
    delay(60);
  }
  if (okShort) {
    Serial.printf("  %s: shorted    mean %9ld   p-p %7ld   delta %ld\n", name_, (long)mShort,
                  (long)ppShort, (long)(mBridge - mShort));
  }

  // --- the second input pair ------------------------------------------------
  // Switching channels invalidates the offset calibration, which is per
  // channel, so both the move and the move back re-run it. That costs a few
  // hundred milliseconds of boot and is worth it exactly once: a bridge landed
  // on VIN2 reads identically to no bridge at all on VIN1.
  int32_t mCh2 = 0, ppCh2 = 0;
  bool okCh2 = false;
  {
    uint8_t saved = 0;
    if (read(REG_CTRL2, &saved, 1) && write(REG_CTRL2, (uint8_t)(saved | (1u << C2_CHS)))) {
      calibrateAfe();
      okCh2 = sampleStats(16, &mCh2, &ppCh2, 1500);
      write(REG_CTRL2, saved);
      calibrateAfe();
    }
  }
  if (okCh2) {
    Serial.printf("  %s: channel 2  mean %9ld   p-p %7ld\n", name_, (long)mCh2, (long)ppCh2);
  }

  // --- what the numbers mean ------------------------------------------------
  // Stated in the log rather than left as an exercise. The thresholds are
  // deliberately loose: this is meant to point at the right half of the
  // problem, not to grade a cell.
  //
  // A live 350 ohm bridge is a NOISE SOURCE. At gain 128 the converter resolves
  // well under a microvolt, so even a perfectly balanced, perfectly still cell
  // wanders by tens to hundreds of counts between conversions. An input with
  // nothing across it does not: it sits where the offset calibration parked it.
  // So peak-to-peak, not the mean, is what separates the two cases -- and the
  // mean is exactly what a trimmed average on the dashboard is designed to make
  // look calm.
  if (!okBridge) return;
  const bool quiet = ppBridge < 50;
  const bool looksShorted = okShort && (mBridge - mShort < 500) && (mShort - mBridge < 500);
  if (quiet && looksShorted) {
    Serial.printf(
        "  %s: >> READS LIKE AN OPEN OR SHORTED INPUT. p-p %ld counts is too quiet for a\n"
        "     live bridge, and shorting the PGA barely moved it. Check E+/E- for ~3.0 V,\n"
        "     check A+/A- continuity through the cell, and check the cell is not on\n"
        "     channel 2 (see above).\n",
        name_, (long)ppBridge);
  } else if (quiet) {
    Serial.printf("  %s: >> quiet (p-p %ld) but the short moved it -- input is connected.\n",
                  name_, (long)ppBridge);
  }
  if (okCh2 && ppCh2 > ppBridge * 4 && ppCh2 > 200) {
    Serial.printf("  %s: >> CHANNEL 2 IS NOISIER THAN CHANNEL 1. The bridge may be wired to\n"
                  "     VIN2P/VIN2N. This firmware reads channel 1.\n",
                  name_);
  }
}

bool Nau7802::poll(uint32_t nowMs) {
  if (state_ == CellState::Offline) return false;

  // ONE REGISTER READ WHEN NOTHING IS WAITING. That is what lets this be called
  // from the render loop: at 80 SPS most calls find CR clear and cost a single
  // byte on the bus rather than a four-byte transaction.
  uint8_t pu = 0;
  if (!read(REG_PU_CTRL, &pu, 1)) {
    if (ioFailures_ >= IO_FAILURES_TO_OFFLINE) {
      state_ = CellState::Offline;
      Serial.printf("  %s: OFFLINE (%u consecutive I2C failures)\n", name_, ioFailures_);
    }
    return false;
  }

  if (!((pu >> PU_CR) & 1)) {
    // Registers answer but no conversion completes. Invisible to the read path
    // above, so it needs its own deadline -- the same failure SENSOR_STALE_MS
    // exists for on the ToF array.
    if ((uint32_t)(nowMs - lastSampleMs_) > CELL_STALE_MS && state_ == CellState::Online) {
      state_ = CellState::Offline;
      Serial.printf("  %s: OFFLINE (no conversion completed in %lu ms)\n", name_,
                    (unsigned long)CELL_STALE_MS);
    }
    return false;
  }

  uint8_t b[3];
  if (!read(REG_ADCO_B2, b, 3)) return false;

  // 24-bit two's complement, MSB first, sign-extended into int32. Assembling it
  // unsigned and then subtracting 2^24 would work too; shifting left into the
  // sign bit and back is the version that cannot be got wrong by one.
  int32_t raw = ((int32_t)b[0] << 24) | ((int32_t)b[1] << 16) | ((int32_t)b[2] << 8);
  raw >>= 8;

  counts_ = raw;
  lastSampleMs_ = nowMs;
  state_ = CellState::Online;

  // Measured rate, not the configured one. On a bit-banged bus polled from a
  // rendering loop these differ, and the difference is the interesting number.
  sampleCount_++;
  if ((uint32_t)(nowMs - spsWindowMs_) >= 1000) {
    sps_ = (uint16_t)((sampleCount_ * 1000UL) / (nowMs - spsWindowMs_));
    sampleCount_ = 0;
    spsWindowMs_ = nowMs;
  }
  return true;
}
