// The universal load-scale core: the measurement logic of ONE weighing platform,
// with no knowledge of where its conversions come from.
//
// WHY A SEPARATE CORE, AND WHY IT HAS NO ARDUINO IN IT. Conversions reach it from a
// NAU7802 on this board's I2C today, and from an ATtiny3224 node on an RS485 bus on
// the next generation of hardware. The arithmetic -- filtering, step detection, the
// zero and the counts-per-gram, the weight_state ladder, the bowl tracker -- must be
// identical for both, so it lives here once. And it must be testable on a PC: the
// bowl-counting rules are exactly the kind of logic that is easy to get subtly wrong
// and impractical to exercise with real bowls on a bench. tools/host_test/ runs this
// file under g++ with no hardware at all.
//
// WHAT IT OWNS: the filter window, step detection, zero and factor, the weight_state
// ladder, and -- for a buffer -- the bowl tracker. WHAT IT DOES NOT: I/O, NVS, tasks,
// clocks, logging. The caller passes milliseconds in, persists what persisted()
// returns whenever dirty() says so, and prints the BowlChange it is handed.
//
// THE COUNTER'S OWN SCALE (src/scale.cpp, three cells summed) DOES NOT USE THIS. It
// works in the field and it stays exactly as it is; this core started from its
// filter and step detection, and the Counter role exists so a future single-cell
// counter on RS485 has somewhere to go.

#pragma once

#include <stdint.h>

namespace lscale {

enum class Role : uint8_t { Counter, Buffer };

// Health of whatever produces the conversions. Mirrors CellState so a caller with a
// Nau7802 translates one-to-one.
enum class Link : uint8_t { Offline, Warming, Online };

// THE DATABASE'S weight_state VOCABULARY, reused rather than extended, so the
// device_status and weight_samples CHECKs need no change for a buffer. The meanings
// are the same sentence for both roles: no reading / not settled / saturated / no
// factor / no zero / a weight.
enum class WState : uint8_t { NoCells, CellsPartial, OverRange, Settling, Uncalibrated, Untared, Ok };
const char *wstateToken(WState s);

// --- configuration --------------------------------------------------------------
// Every figure below is a judgement about a 200 kg buffer cell at ~22 counts/g and
// 10 SPS; the defaults are the reasoned values, and a different platform passes its
// own. See load_scale.cpp for why each one is what it is.
struct FilterConfig {
  uint8_t window = 32;            // samples in the trimmed mean: 3.2 s at 10 SPS
  uint8_t trim = 1;               // discarded at EACH end once there are enough
  float stepG = 80.0f;            // a load change, when calibrated
  int32_t stepCountsUncal = 1500; // a load change, before calibration
  int32_t stepCountsMin = 1000;   // floor, so a low factor cannot shrink 80 g into the noise
  uint8_t stepConfirm = 3;        // consecutive samples beyond the threshold
  uint8_t stepMinSamples = 4;     // fewest a step can be judged against
  uint8_t minSamplesShown = 3;    // a step restart seeds exactly this many
  int32_t overRangeCounts = 8000000;
  float railMinG = -5000.0f;      // the DB's weight_g floor
  float railMaxG = 250000.0f;     // the DB's weight_g ceiling for a buffer
  // Committing a zero or a factor stores it, so the reading must have stopped moving.
  int32_t ppMaxCommit = 3000;
  float minCalG = 1000.0f;
  int32_t minCalCounts = 10000;
  float cpgMin = 2.0f;
  float cpgMax = 200.0f;
};

struct BowlConfig {
  float dryG = 2500.0f;           // one empty buffer bowl
  float eventMinG = 10000.0f;     // a jump this size or more is bowls, not food
  // and it must hold this long before it counts. 2.5 s, not the original 5: the owner's
  // ceiling is 3-4 s from a bowl going on to the count changing (2026-10-04), and the
  // 0.3 kg band below is what keeps a leg or a lean from counting, not the length.
  uint32_t stableMs = 2500;
  float stableBandG = 300.0f;     // "holding": the settled reading stays inside this band
  // ONE FULL BOWL ON THE PLATFORM, as the divisor that says HOW MANY bowls one jump is
  // -- not the threshold that says a jump IS bowls (that is eventMinG). 17 kg, the
  // owner's figure (2026-10-03): bowls run 14-18 kg of food + the 2.5 kg bowl, nearer
  // the light end in practice. Loaded one at a time -- the normal way -- any bowl from
  // 10 to 25 kg counts as exactly one whatever this says. It only decides a jump of
  // several at once: round(jump / 17 kg) is right for every mix of 1..4 bowls holding
  // 12.4-16.6 kg of food each (four is the most a stack holds: overheight).
  //
  // FIXED, NOT LEARNED: the window that keeps four-at-once right is only ~0.6 kg wide,
  // and learning from single loads of 16.5-20.5 kg could only walk it out.
  float typicalFullDefaultG = 17000.0f;
  float typicalFullMinG = 14500.0f;  // a figure outside one real bowl is never used
  float typicalFullMaxG = 20500.0f;
  float learnAlpha = 0.0f;  // > 0 learns from single loads; see above for why it is off
  uint8_t maxBowls = 4;
  float emptyBelowG = 1000.0f;    // a settled reading below this IS an empty platform
  float inconsistentG = 1000.0f;  // food below -this means the count cannot be right
};

// --- the trimmed window with step detection ---------------------------------------
// Ported from scale.cpp (CountWindow, and the step logic in scaleTask) so the buffer
// behaves the way the counter already does in the field: quiet when nothing is
// happening, immediate when something is.
class Filter {
 public:
  static const uint8_t MAX_WINDOW = 64;

  void configure(const FilterConfig &c);
  void clear();
  // Adds one conversion. Returns true if it caused a step restart.
  bool add(int32_t raw, int32_t stepThresholdCounts);
  uint8_t size() const { return count_; }
  bool full() const { return count_ >= window_; }
  // Trimmed mean, peak-to-peak and the over-range flag of what is in the window.
  bool stats(int32_t *mean, int32_t *pp, bool *over) const;

 private:
  void push(int32_t v);

  FilterConfig cfg_{};
  uint8_t window_ = 32;
  int32_t ring_[MAX_WINDOW] = {0};
  uint8_t count_ = 0;
  uint8_t next_ = 0;
  uint8_t stepRun_ = 0;
  int32_t stepBuf_[8] = {0};
};

// --- the bowl tracker (buffer role) ------------------------------------------------
enum class BowlEvent : uint8_t { None, Loaded, Unloaded, EmptyReset, Inconsistent, Clamped };
const char *bowlEventText(BowlEvent e);

struct BowlChange {
  BowlEvent event = BowlEvent::None;
  int8_t delta = 0;         // bowls added (+) or removed (-)
  uint8_t bowlsAfter = 0;
  float stepG = 0.0f;       // the settled jump that caused it
};

class BowlTracker {
 public:
  void configure(const BowlConfig &c);
  // After a power cycle: the count is remembered but NOT confirmed -- the stack may
  // have changed while the unit was off.
  void restore(uint8_t bowls, float typicalFullG);
  // The platform was just zeroed, so it is empty: nothing on it, and known to be so.
  void resetEmpty();
  // An operator says how many bowls are there.
  void set(uint8_t bowls);
  // One settled gross reading per conversion. Returns what, if anything, changed.
  BowlChange update(uint32_t nowMs, float grossG);

  uint8_t bowls() const { return bowls_; }
  bool confirmed() const { return confirmed_; }
  float typicalFullG() const { return typical_; }

 private:
  BowlConfig cfg_{};
  uint8_t bowls_ = 0;
  bool confirmed_ = false;
  float typical_ = 15000.0f;

  // The current run of readings that stay inside the stability band.
  bool running_ = false;
  uint32_t runStart_ = 0;
  float runMin_ = 0, runMax_ = 0;
  double runSum_ = 0;
  uint32_t runN_ = 0;
  bool runJudged_ = false;   // this run has already been evaluated once

  bool haveRef_ = false;
  float ref_ = 0.0f;         // the last settled level
};

// --- commands -----------------------------------------------------------------------
enum class Commit : uint8_t {
  Ok,
  NotConverting,   // the source is not producing conversions
  Settling,        // the window has not filled since boot or the last step
  Moving,          // peak-to-peak too large: something is still changing
  OverRange,
  NotZeroed,       // calibrate needs an empty-platform zero first
  MassTooSmall,
  NoDeflection,    // the mass did not move the reading enough to derive anything from
  Negative,        // the reading FELL when mass was added: A+/A- or a cell upside down
  Implausible,     // the factor is nowhere near what this hardware can produce
};
const char *commitText(Commit c);

// --- what one platform reports ---------------------------------------------------
struct Reading {
  Link link = Link::Offline;
  WState state = WState::NoCells;
  // ZERO unless link == Online: a stale figure beside a state that says the source
  // stopped would read as a live one.
  int32_t raw = 0;
  int32_t counts = 0;       // filtered, absolute (before the zero)
  int32_t pp = 0;
  uint8_t samples = 0;
  bool zeroed = false;
  int32_t zero = 0;
  bool calibrated = false;
  float cpg = 0.0f;
  bool overRange = false;
  // Kilograms exist only when state == Ok. Zero is a real weight -- an empty shelf --
  // so it cannot stand in for unknown.
  bool kgKnown = false;
  float grossG = 0.0f;      // everything on the platform
  float foodG = 0.0f;       // buffer: gross minus the bowls' dry mass; counter: gross
  // Buffer role only.
  uint8_t bowls = 0;
  bool bowlsConfirmed = false;
  float typicalFullG = 0.0f;
};

// What survives a power cycle. The caller stores it; the core never touches flash.
struct Persisted {
  bool zeroed = false;
  int32_t zero = 0;
  float cpg = 0.0f;         // 0 = uncalibrated
  uint8_t bowls = 0;
  float typicalFullG = 15000.0f;
};

// --- the counter's empty-vessel rule -------------------------------------------------
// The counter holds at most ONE serving vessel, so its offset is subtracted only while
// the platform plainly carries one: a 100 g spoon reads 100 g, not -2.4 kg, and a
// 2.5 kg vessel holding 150 g reads 150 g. Here rather than in each consumer because
// the panel row, the uplink's weight_g and its plausibility band must all agree.
//
// HALF THE VESSEL, NOT THE WHOLE OF IT. Vessels vary: "if heavier than the offset"
// would show an empty 2.4 kg vessel as 2.4 kg of food -- the one reading that sends
// nobody to refill a counter that needs it. At half, that vessel reads -0.1 kg,
// visibly impossible, and nothing a person puts down without a vessel comes near
// 1.25 kg. Food left in a vessel never takes it below half, so the rule does not
// flicker as a dish empties.
//
// NOT CLAMPED, for scale_telemetry.cpp's reason: a clamp turns "lighter vessel than
// the offset" into "empty". Stateless, so a reboot with a vessel on reads correctly.
// The buffers do not use it: they count bowls by the size of each jump (BowlTracker).
inline bool vesselOnPlatform(float grossG, float vesselG) {
  return vesselG > 0.0f && grossG >= vesselG * 0.5f;
}
inline float netOfVessel(float grossG, float vesselG) {
  return vesselOnPlatform(grossG, vesselG) ? grossG - vesselG : grossG;
}

class LoadScale {
 public:
  void configure(Role role, const FilterConfig &f, const BowlConfig &b);
  void restore(const Persisted &p);
  void setLink(Link l);
  // A new conversion from the source. Returns the bowl change it caused, if any
  // (always None for the counter role).
  BowlChange addSample(int32_t raw, uint32_t nowMs);

  Reading reading() const;
  Persisted persisted() const;
  bool dirty() const { return dirty_; }
  void clearDirty() { dirty_ = false; }

  Commit zero();                              // the platform must be EMPTY
  Commit calibrate(float knownG, float *cpgOut = nullptr);
  void clearCalibration();
  void setBowls(uint8_t n);

  Role role() const { return role_; }

 private:
  int32_t stepThreshold() const;
  Commit commitReady(int32_t *mean, int32_t *pp) const;

  Role role_ = Role::Buffer;
  FilterConfig fc_{};
  BowlConfig bc_{};
  Filter filter_;
  BowlTracker bowls_;
  Link link_ = Link::Offline;
  int32_t raw_ = 0;
  bool zeroed_ = false;
  int32_t zero_ = 0;
  float cpg_ = 0.0f;
  bool dirty_ = false;
};

}  // namespace lscale
