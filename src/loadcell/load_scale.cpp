#include "load_scale.h"

#include <math.h>

namespace lscale {

const char *wstateToken(WState s) {
  // THESE STRINGS ARE A CONTRACT with device_status_weight_state_ck and the same
  // CHECK on weight_samples. They are spelled exactly as the counter's firmware
  // spells them (scale_telemetry.cpp) -- a near-miss is a 23514 and a dropped row.
  switch (s) {
    case WState::NoCells: return "no_cells";
    case WState::CellsPartial: return "cells_partial";
    case WState::OverRange: return "over_range";
    case WState::Settling: return "settling";
    case WState::Uncalibrated: return "uncalibrated";
    case WState::Untared: return "untared";
    case WState::Ok: return "ok";
  }
  return "no_cells";
}

const char *bowlEventText(BowlEvent e) {
  switch (e) {
    case BowlEvent::None: return "none";
    case BowlEvent::Loaded: return "loaded";
    case BowlEvent::Unloaded: return "unloaded";
    case BowlEvent::EmptyReset: return "empty -- count reset to 0";
    case BowlEvent::Inconsistent: return "count cannot be right -- unconfirmed";
    case BowlEvent::Clamped: return "count limited -- unconfirmed";
  }
  return "?";
}

const char *commitText(Commit c) {
  switch (c) {
    case Commit::Ok: return "done";
    case Commit::NotConverting: return "the cell is not converting";
    case Commit::Settling: return "still settling -- try again in a few seconds";
    case Commit::Moving: return "the reading is not steady";
    case Commit::OverRange: return "the reading is at the end of the converter's range";
    case Commit::NotZeroed: return "no empty-platform zero yet";
    case Commit::MassTooSmall: return "the mass is below the minimum";
    case Commit::NoDeflection: return "the mass barely moved the reading";
    case Commit::Negative: return "the reading FELL when mass was added -- check A+/A-";
    case Commit::Implausible: return "the factor is implausible for this hardware";
  }
  return "?";
}

// =================================================================================
// Filter
// =================================================================================

void Filter::configure(const FilterConfig &c) {
  cfg_ = c;
  window_ = c.window;
  if (window_ < 4) window_ = 4;
  if (window_ > MAX_WINDOW) window_ = MAX_WINDOW;
  if (cfg_.stepConfirm < 1) cfg_.stepConfirm = 1;
  if (cfg_.stepConfirm > sizeof(stepBuf_) / sizeof(stepBuf_[0]))
    cfg_.stepConfirm = (uint8_t)(sizeof(stepBuf_) / sizeof(stepBuf_[0]));
  clear();
}

void Filter::clear() {
  count_ = 0;
  next_ = 0;
  stepRun_ = 0;
}

void Filter::push(int32_t v) {
  ring_[next_] = v;
  next_ = (uint8_t)((next_ + 1) % window_);
  if (count_ < window_) count_++;
}

bool Filter::stats(int32_t *mean, int32_t *pp, bool *over) const {
  if (count_ == 0) return false;
  int32_t t[MAX_WINDOW];
  for (uint8_t i = 0; i < count_; i++) t[i] = ring_[i];
  // Insertion sort: at most 64 values, nearly sorted on a settled cell.
  for (uint8_t i = 1; i < count_; i++) {
    const int32_t v = t[i];
    int16_t j = (int16_t)i - 1;
    while (j >= 0 && t[j] > v) {
      t[j + 1] = t[j];
      j--;
    }
    t[j + 1] = v;
  }
  // The trim is skipped below 2*trim+2 samples rather than applied to almost
  // nothing -- trimming 2 of 3 reports the median and calls it an average.
  // (scale.cpp's CountWindow::mean, same rule.)
  uint8_t lo = 0, hi = count_;
  if (count_ >= (uint8_t)(2 * cfg_.trim + 2)) {
    lo = cfg_.trim;
    hi = (uint8_t)(count_ - cfg_.trim);
  }
  int64_t acc = 0;
  for (uint8_t i = lo; i < hi; i++) acc += t[i];
  *mean = (int32_t)(acc / (int32_t)(hi - lo));
  // Peak-to-peak is UNTRIMMED on purpose: here the extremes are the measurement.
  *pp = t[count_ - 1] - t[0];
  *over = (t[0] <= -cfg_.overRangeCounts) || (t[count_ - 1] >= cfg_.overRangeCounts);
  return true;
}

bool Filter::add(int32_t raw, int32_t stepThresholdCounts) {
  // STEP DETECTION, as scale.cpp does it for the counter. A moving average crawls
  // after a load change because it is still averaging a platform that no longer
  // exists. When consecutive samples land far outside the current mean that is the
  // load changing, not noise -- so the window is thrown away and restarted from the
  // samples that proved it, and the reading jumps at once.
  //
  // FROM stepMinSamples, NOT FROM A FULL WINDOW: detection clears the window, so
  // gating on a full one would blind the detector for a whole window after every
  // step it caught (the bug recorded in scale.cpp).
  if (count_ >= cfg_.stepMinSamples) {
    int32_t m = 0, pp = 0;
    bool over = false;
    stats(&m, &pp, &over);
    const int32_t d = (raw > m) ? (raw - m) : (m - raw);
    if (d > stepThresholdCounts) {
      if (stepRun_ < cfg_.stepConfirm) stepBuf_[stepRun_] = raw;
      if (++stepRun_ >= cfg_.stepConfirm) {
        count_ = 0;
        next_ = 0;
        // Seeded with the samples that triggered it -- all of them already measure
        // the new load. `raw` is the last of them, so it is NOT pushed again below
        // (scale.cpp pushes it twice; counting one sample double is a small error
        // this core does not need to inherit).
        for (uint8_t k = 0; k < cfg_.stepConfirm; k++) push(stepBuf_[k]);
        stepRun_ = 0;
        return true;
      }
    } else {
      stepRun_ = 0;  // consecutive or nothing: one sample back inside the band was noise
    }
  }
  push(raw);
  return false;
}

// =================================================================================
// BowlTracker
// =================================================================================
//
// THE RULE, as the kitchen states it: a buffer bowl's dry mass is 2.5 kg. When the
// settled weight jumps by 10 kg or more and holds for 5 s, bowls were loaded onto
// the stack; when it falls by 10 kg or more and holds, bowls were taken off. The
// food on the shelf is the gross weight minus 2.5 kg per bowl.
//
// HOW "HOLDS FOR 5 s" IS JUDGED: a RUN is the longest recent stretch of settled
// readings that all fit inside a band of stableBandG. A reading that would stretch
// the band past that width starts a new run. A run that has lasted stableMs is
// stable, and its mean is a LEVEL. Each run is judged exactly once, at the moment it
// first becomes stable, against the previous level.
//
// SMALL CHANGES MOVE THE REFERENCE. Food being taken from a bowl, or a lid going
// on, settles at a new level less than 10 kg away; the reference follows it. Without
// that, minutes of slow consumption would add up to a phantom "unload".
//
// HOW MANY BOWLS: round(jump / typical full bowl), at least one. The typical figure
// starts at 15 kg and is LEARNED from loads that count as exactly one bowl -- only
// loads, because an unloaded bowl may have had food taken out of it.
//
// WHAT IT REFUSES TO CLAIM: after a power cycle the count is remembered but marked
// unconfirmed, because the stack may have changed while the unit was off. It is
// confirmed again by the next load/unload, by a settled empty platform (which IS a
// count of zero), or by an operator. A count that makes the food come out
// impossibly negative is marked unconfirmed rather than silently corrected.

void BowlTracker::configure(const BowlConfig &c) {
  cfg_ = c;
  typical_ = c.typicalFullDefaultG;
  running_ = false;
  haveRef_ = false;
}

void BowlTracker::restore(uint8_t bowls, float typicalFullG) {
  bowls_ = bowls > cfg_.maxBowls ? cfg_.maxBowls : bowls;
  confirmed_ = false;
  if (typicalFullG >= cfg_.typicalFullMinG && typicalFullG <= cfg_.typicalFullMaxG)
    typical_ = typicalFullG;
  running_ = false;
  haveRef_ = false;
}

void BowlTracker::resetEmpty() {
  bowls_ = 0;
  confirmed_ = true;
  running_ = false;
  haveRef_ = false;
}

void BowlTracker::set(uint8_t bowls) {
  bowls_ = bowls > cfg_.maxBowls ? cfg_.maxBowls : bowls;
  confirmed_ = true;
  // THE REFERENCE IS KEPT -- an operator correcting the count has not moved anything,
  // so the next settled level is not a load or an unload -- BUT THE RUN RESTARTS, so
  // that level is judged again within stableMs. Without it a count typed with the
  // platform already still was never checked against the shelf: "2 bowls" on an
  // empty platform read food -4.96 kg indefinitely on the bench, because that run had
  // been judged before the command and nothing moved to start a new one. Now the
  // empty rule overrules it, and an impossible count is flagged unconfirmed.
  running_ = false;
}

BowlChange BowlTracker::update(uint32_t nowMs, float g) {
  BowlChange out;
  out.bowlsAfter = bowls_;

  // --- extend or restart the run ---------------------------------------------------
  // `g > runMin + band` (rather than comparing to the max) is what keeps max - min
  // within the band after g is included.
  if (!running_ || g > runMin_ + cfg_.stableBandG || g < runMax_ - cfg_.stableBandG) {
    running_ = true;
    runStart_ = nowMs;
    runMin_ = runMax_ = g;
    runSum_ = g;
    runN_ = 1;
    runJudged_ = false;
    return out;
  }
  if (g < runMin_) runMin_ = g;
  if (g > runMax_) runMax_ = g;
  runSum_ += g;
  runN_++;

  if (runJudged_ || (uint32_t)(nowMs - runStart_) < cfg_.stableMs) return out;
  runJudged_ = true;
  const float level = (float)(runSum_ / (double)runN_);

  // --- judge the newly stable level -------------------------------------------------
  if (haveRef_) {
    const float step = level - ref_;
    const float mag = step < 0 ? -step : step;
    if (mag >= cfg_.eventMinG) {
      long n = lroundf(mag / typical_);
      if (n < 1) n = 1;
      out.stepG = step;
      if (step > 0) {
        out.event = BowlEvent::Loaded;
        // Learn only from a load that counts as one bowl: it is the cleanest
        // measurement of "one full bowl" this platform ever gets.
        if (n == 1) {
          float t = (1.0f - cfg_.learnAlpha) * typical_ + cfg_.learnAlpha * mag;
          if (t < cfg_.typicalFullMinG) t = cfg_.typicalFullMinG;
          if (t > cfg_.typicalFullMaxG) t = cfg_.typicalFullMaxG;
          typical_ = t;
        }
        const long want = (long)bowls_ + n;
        if (want > cfg_.maxBowls) {
          // More bowls than the stack holds: something other than bowls was put on,
          // or a load was missed. Say so; do not invent a fifth level.
          bowls_ = cfg_.maxBowls;
          confirmed_ = false;
          out.event = BowlEvent::Clamped;
        } else {
          bowls_ = (uint8_t)want;
          confirmed_ = true;
        }
        out.delta = (int8_t)n;
      } else {
        out.event = BowlEvent::Unloaded;
        const long want = (long)bowls_ - n;
        if (want < 0) {
          bowls_ = 0;
          confirmed_ = false;
          out.event = BowlEvent::Clamped;
        } else {
          bowls_ = (uint8_t)want;
          confirmed_ = true;
        }
        out.delta = (int8_t)-n;
      }
    }
  }
  ref_ = level;
  haveRef_ = true;

  // --- an empty platform is a count of zero, and a confirmed one --------------------
  if (level < cfg_.emptyBelowG) {
    if (bowls_ != 0 || !confirmed_) {
      out.event = BowlEvent::EmptyReset;
      out.delta = (int8_t)-(int8_t)bowls_;
      out.stepG = level;
    }
    bowls_ = 0;
    confirmed_ = true;
  } else if (level - (float)bowls_ * cfg_.dryG < -cfg_.inconsistentG && confirmed_) {
    // Less on the shelf than the counted bowls alone would weigh: an unload was
    // missed (a nearly-empty bowl taken off is under the 10 kg threshold). The
    // count is not changed -- guessing would be a claim -- it is marked unconfirmed.
    confirmed_ = false;
    if (out.event == BowlEvent::None) out.event = BowlEvent::Inconsistent;
  }

  out.bowlsAfter = bowls_;
  return out;
}

// =================================================================================
// LoadScale
// =================================================================================

void LoadScale::configure(Role role, const FilterConfig &f, const BowlConfig &b) {
  role_ = role;
  fc_ = f;
  bc_ = b;
  filter_.configure(f);
  bowls_.configure(b);
  link_ = Link::Offline;
  raw_ = 0;
  dirty_ = false;
}

void LoadScale::restore(const Persisted &p) {
  zeroed_ = p.zeroed;
  zero_ = p.zero;
  // A factor outside the band is treated as no factor, not trusted: NaN, a corrupted
  // float or a value from some other firmware must not become kilograms.
  cpg_ = (p.cpg >= fc_.cpgMin && p.cpg <= fc_.cpgMax) ? p.cpg : 0.0f;
  bowls_.restore(p.bowls, p.typicalFullG);
}

void LoadScale::setLink(Link l) {
  if (l != Link::Online && link_ == Link::Online) {
    // A source that stopped must not keep a window that still averages to a
    // plausible weight.
    filter_.clear();
  }
  link_ = l;
}

int32_t LoadScale::stepThreshold() const {
  if (cpg_ > 0.0f) {
    const int32_t c = (int32_t)(fc_.stepG * cpg_);
    return c > fc_.stepCountsMin ? c : fc_.stepCountsMin;
  }
  return fc_.stepCountsUncal;
}

BowlChange LoadScale::addSample(int32_t raw, uint32_t nowMs) {
  raw_ = raw;
  filter_.add(raw, stepThreshold());

  BowlChange ch;
  if (role_ != Role::Buffer) return ch;
  // Bowls are counted in GRAMS, so the tracker runs only once there is a zero and a
  // factor, and only on a settled, in-range reading.
  if (!zeroed_ || cpg_ <= 0.0f || filter_.size() < fc_.minSamplesShown) return ch;
  int32_t m = 0, pp = 0;
  bool over = false;
  if (!filter_.stats(&m, &pp, &over) || over) return ch;
  const float gross = (float)(m - zero_) / cpg_;
  const uint8_t before = bowls_.bowls();
  const float typBefore = bowls_.typicalFullG();
  ch = bowls_.update(nowMs, gross);
  // Dirty only for what is PERSISTED. `confirmed` is deliberately not stored -- a
  // power cycle always comes back unconfirmed -- so a change to it alone is not a
  // reason to write flash.
  if (bowls_.bowls() != before || bowls_.typicalFullG() != typBefore) dirty_ = true;
  return ch;
}

Reading LoadScale::reading() const {
  Reading r;
  r.link = link_;
  r.zeroed = zeroed_;
  r.zero = zero_;
  r.calibrated = cpg_ > 0.0f;
  r.cpg = cpg_;
  if (role_ == Role::Buffer) {
    r.bowls = bowls_.bowls();
    r.bowlsConfirmed = bowls_.confirmed();
    r.typicalFullG = bowls_.typicalFullG();
  }

  if (link_ == Link::Offline) {
    r.state = WState::NoCells;
    return r;
  }
  if (link_ == Link::Warming || filter_.size() < fc_.minSamplesShown) {
    r.state = WState::Settling;
    return r;
  }

  r.raw = raw_;
  r.samples = filter_.size();
  bool over = false;
  filter_.stats(&r.counts, &r.pp, &over);
  r.overRange = over;

  // THE LADDER, in the counter's order (scale_telemetry.cpp): the first thing that
  // stops this being a weight is the one that is reported.
  if (over) {
    r.state = WState::OverRange;
    return r;
  }
  if (!r.calibrated) {
    r.state = WState::Uncalibrated;
    return r;
  }
  if (!zeroed_) {
    r.state = WState::Untared;
    return r;
  }
  r.grossG = (float)(r.counts - zero_) / cpg_;
  r.foodG = (role_ == Role::Buffer) ? r.grossG - (float)r.bowls * bc_.dryG : r.grossG;
  // A figure outside the database's rails is not a weight to publish: report the
  // ceiling as over_range instead of letting a CHECK reject the whole row.
  if (r.foodG < fc_.railMinG || r.foodG > fc_.railMaxG) {
    r.state = WState::OverRange;
    return r;
  }
  r.state = WState::Ok;
  r.kgKnown = true;
  return r;
}

Persisted LoadScale::persisted() const {
  Persisted p;
  p.zeroed = zeroed_;
  p.zero = zero_;
  p.cpg = cpg_;
  p.bowls = bowls_.bowls();
  p.typicalFullG = bowls_.typicalFullG();
  return p;
}

Commit LoadScale::commitReady(int32_t *mean, int32_t *pp) const {
  if (link_ != Link::Online) return Commit::NotConverting;
  if (!filter_.full()) return Commit::Settling;
  bool over = false;
  filter_.stats(mean, pp, &over);
  if (over) return Commit::OverRange;
  // Stored figures come only from a reading that has stopped moving. The window is
  // several seconds long: a key pressed a second after a mass lands would otherwise
  // average two plateaus and persist the result.
  if (*pp > fc_.ppMaxCommit) return Commit::Moving;
  return Commit::Ok;
}

Commit LoadScale::zero() {
  int32_t m = 0, pp = 0;
  const Commit c = commitReady(&m, &pp);
  if (c != Commit::Ok) return c;
  zero_ = m;
  zeroed_ = true;
  // The platform was declared empty, so the stack is empty -- and known to be.
  bowls_.resetEmpty();
  dirty_ = true;
  return Commit::Ok;
}

Commit LoadScale::calibrate(float knownG, float *cpgOut) {
  if (!zeroed_) return Commit::NotZeroed;
  if (!(knownG >= fc_.minCalG)) return Commit::MassTooSmall;  // also catches NaN
  int32_t m = 0, pp = 0;
  const Commit c = commitReady(&m, &pp);
  if (c != Commit::Ok) return c;
  const int32_t defl = m - zero_;
  if (defl < 0 && -defl >= fc_.minCalCounts) return Commit::Negative;
  if (defl < fc_.minCalCounts) return Commit::NoDeflection;
  const float cpg = (float)defl / knownG;
  if (!(cpg >= fc_.cpgMin && cpg <= fc_.cpgMax)) return Commit::Implausible;
  cpg_ = cpg;
  if (cpgOut) *cpgOut = cpg;
  dirty_ = true;
  return Commit::Ok;
}

void LoadScale::clearCalibration() {
  cpg_ = 0.0f;
  dirty_ = true;
}

void LoadScale::setBowls(uint8_t n) {
  bowls_.set(n);
  dirty_ = true;
}

}  // namespace lscale
