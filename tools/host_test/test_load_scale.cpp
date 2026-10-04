// Host tests for the load-scale core (include/load_scale.h, src/loadcell/load_scale.cpp).
//
// No hardware, no Arduino: a simulated platform feeds the REAL core one conversion
// every 100 ms (10 SPS, the converter's rate) with deterministic noise, and the tests
// check what the kitchen will see -- when a bowl is counted, what the food reads,
// what is refused. Run with tools/host_test/run.sh.
//
// The noise generator is a fixed-seed LCG, so a failure reproduces exactly.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "load_scale.h"

using namespace lscale;

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    g_checks++;                                           \
    if (!(cond)) {                                        \
      g_fails++;                                          \
      std::printf("  FAIL %s:%d  ", __FILE__, __LINE__);  \
      std::printf(__VA_ARGS__);                           \
      std::printf("\n");                                  \
    }                                                     \
  } while (0)

// The bench figures measured on LDC-001's buffer cell today.
static const double CPG = 22.143;     // counts per gram
static const double ZERO = 106060;    // empty-platform counts
static const double NOISE = 175;      // +/- counts -> p-p 350, the worst still-shelf figure seen

struct Rig {
  LoadScale s;
  uint32_t t = 0;
  uint32_t seed = 12345;
  double gross = 0;      // grams actually on the platform
  double noise = NOISE;
  std::vector<BowlChange> events;

  explicit Rig(Role role = Role::Buffer) {
    s.configure(role, FilterConfig{}, BowlConfig{});
    s.setLink(Link::Online);
  }
  double rnd() {
    seed = seed * 1664525u + 1013904223u;
    return ((seed >> 8) / (double)(1u << 24)) * 2.0 - 1.0;
  }
  int32_t raw() { return (int32_t)std::lround(ZERO + gross * CPG + noise * rnd()); }
  // Advance `ms` at the current load, one conversion per 100 ms.
  void run(double ms) {
    for (double e = 0; e < ms; e += 100) {
      t += 100;
      const BowlChange c = s.addSample(raw(), t);
      if (c.event != BowlEvent::None) events.push_back(c);
    }
  }
  // A commissioned platform (zero + factor already stored), empty and confirmed.
  void commission() {
    Persisted p;
    p.zeroed = true;
    p.zero = (int32_t)ZERO;
    p.cpg = (float)CPG;
    p.bowls = 0;
    p.typicalFullG = 17000;
    s.restore(p);
    gross = 0;
    run(8000);  // an empty platform settles, which confirms the zero count
    events.clear();
  }
  Reading r() const { return s.reading(); }
  int count(BowlEvent e) const {
    int n = 0;
    for (const auto &c : events) n += (c.event == e);
    return n;
  }
};

static void section(const char *name) { std::printf("- %s\n", name); }

// ---------------------------------------------------------------------------------
static void filterStepResponse() {
  section("filter: a load shows within a second, and returns to zero within a second");
  Rig g;
  g.commission();
  g.gross = 1060;  // the boxed spool used on the bench
  double at = -1;
  for (int i = 0; i < 100 && at < 0; i++) {
    g.run(100);
    const Reading r = g.r();
    if (r.kgKnown && std::fabs(r.grossG - 1060) < 100) at = (i + 1) * 0.1;
  }
  CHECK(at > 0 && at <= 1.0, "load visible within 100 g after %.1f s (want <= 1.0 s)", at);
  g.gross = 0;
  at = -1;
  for (int i = 0; i < 100 && at < 0; i++) {
    g.run(100);
    const Reading r = g.r();
    if (r.kgKnown && std::fabs(r.grossG) < 100) at = (i + 1) * 0.1;
  }
  CHECK(at > 0 && at <= 1.0, "back to zero within 100 g after %.1f s (want <= 1.0 s)", at);
}

static void filterNoFalseSteps() {
  section("filter: no step restarts from noise (20 min, up to 3x the worst noise) or slow drift");
  FilterConfig fc;
  const int32_t thr = (int32_t)(fc.stepG * CPG);
  for (double amp : {175.0, 350.0, 525.0}) {
    Filter f;
    f.configure(fc);
    uint32_t seed = 7;
    int restarts = 0;
    for (int i = 0; i < 12000; i++) {
      seed = seed * 1664525u + 1013904223u;
      const double n = ((seed >> 8) / (double)(1u << 24)) * 2.0 - 1.0;
      restarts += f.add((int32_t)(ZERO + amp * n), thr);
    }
    CHECK(restarts == 0, "noise +/-%.0f: %d restarts (want 0)", amp, restarts);
  }
  Filter f;
  f.configure(fc);
  int restarts = 0;
  for (int i = 0; i < 600; i++) restarts += f.add((int32_t)(ZERO + 1500.0 * i / 600.0), thr);
  CHECK(restarts == 0, "slow drift of 1500 counts over 60 s: %d restarts (want 0)", restarts);
}

// ---------------------------------------------------------------------------------
static void singleLoad() {
  section("bowls: one bowl is counted within 4 s (owner's ceiling), food = gross - 2.5 kg");
  Rig g;
  g.commission();
  g.gross = 18500;  // a 2.5 kg bowl with 16 kg of food
  g.run(2000);
  CHECK(g.events.empty(), "no event before the load has held 2.5 s (got %zu)", g.events.size());
  g.run(2000);
  CHECK(g.count(BowlEvent::Loaded) == 1, "exactly one Loaded event (got %d)", g.count(BowlEvent::Loaded));
  const Reading r = g.r();
  CHECK(r.bowls == 1 && r.bowlsConfirmed, "1 bowl, confirmed (got %u, %d)", r.bowls, r.bowlsConfirmed);
  CHECK(std::fabs(r.foodG - 16000) < 60, "food %.0f g (want 16000 +/- 60)", r.foodG);
  CHECK(std::fabs(r.grossG - 18500) < 60, "gross %.0f g (want 18500 +/- 60)", r.grossG);
}

static void doubleLoad() {
  section("bowls: two bowls at once are counted by size");
  Rig g;
  g.commission();
  g.gross = 37000;
  g.run(8000);
  const Reading r = g.r();
  CHECK(r.bowls == 2, "2 bowls (got %u)", r.bowls);
  CHECK(g.events.size() == 1 && g.events[0].delta == 2, "one event of +2 (got %zu events)", g.events.size());
  CHECK(std::fabs(r.foodG - 32000) < 80, "food %.0f g (want 32000)", r.foodG);
}

static void countsOneAtATimeAnyWeight() {
  section("bowls: four loaded one at a time count four, at either end of the owner's range");
  // The normal way bowls arrive. Each jump is one bowl whatever the divisor, so the
  // whole 14-18 kg-of-food range (16.5-20.5 kg on the platform) and lighter fills count.
  for (double each : {16500.0, 18500.0, 20500.0}) {
    Rig g;
    g.commission();
    for (int n = 1; n <= 4; n++) {
      g.gross = n * each;
      g.run(8000);
    }
    CHECK(g.r().bowls == 4 && g.r().bowlsConfirmed, "4 x %.1f kg one by one -> 4 (got %u, %d)", each / 1000,
          g.r().bowls, g.r().bowlsConfirmed);
  }
}

static void countsEveryMixOfBowls() {
  section("bowls: 1..4 bowls at ONCE, 16.5-20.5 kg each (14-18 kg of food), all lightest or heaviest");
  // round(jump / 17 kg) counts every n <= 4 right while each bowl is 14.9-19.1 kg on
  // the platform. The extremes are the hard cases -- any mix in between rounds the same.
  for (int n = 1; n <= 4; n++) {
    for (double each : {16500.0, 20500.0}) {
      Rig g;
      g.commission();
      g.gross = n * each;
      g.run(8000);
      CHECK(g.r().bowls == n && g.r().bowlsConfirmed, "%d x %.1f kg -> %d bowls (got %u, %d)", n, each / 1000, n,
            g.r().bowls, g.r().bowlsConfirmed);
    }
  }
}

static void typicalIsFixed() {
  section("bowls: the full-bowl figure is fixed at 17 kg -- not learned, not restored from old firmware");
  Rig g;
  g.commission();
  g.gross = 20500;  // a heavy single bowl: with learning this would move the figure
  g.run(8000);
  CHECK(std::fabs(g.r().typicalFullG - 17000) < 1, "still 17000 g after a single load (%.0f)", g.r().typicalFullG);
  CHECK(!g.s.dirty() || g.s.persisted().typicalFullG == 17000, "nothing learned to persist");
  Rig h;
  Persisted p;
  p.zeroed = true;
  p.zero = (int32_t)ZERO;
  p.cpg = (float)CPG;
  p.typicalFullG = 15000;  // what older firmware stored
  h.s.restore(p);
  CHECK(std::fabs(h.r().typicalFullG - 17000) < 1, "an old stored 15 kg is ignored (%.0f)", h.r().typicalFullG);
}

static void unload() {
  section("bowls: one bowl taken off");
  Rig g;
  g.commission();
  g.gross = 37000;
  g.run(8000);
  g.gross = 18500;
  g.run(8000);
  const Reading r = g.r();
  CHECK(r.bowls == 1 && r.bowlsConfirmed, "1 bowl left, confirmed (got %u)", r.bowls);
  CHECK(g.count(BowlEvent::Unloaded) == 1, "one Unloaded event (got %d)", g.count(BowlEvent::Unloaded));
}

static void belowThreshold() {
  section("bowls: a change under 14 kg is food, not a bowl");
  Rig g;
  g.commission();
  g.gross = 12000;  // a lean or a leg, under the 14 kg step
  g.run(10000);
  const Reading r = g.r();
  CHECK(g.events.empty() && r.bowls == 0, "no event, 0 bowls (got %zu events, %u bowls)", g.events.size(), r.bowls);
  CHECK(std::fabs(r.foodG - 12000) < 60, "food %.0f g (want 12000)", r.foodG);
}

static void unstableThenStable() {
  section("bowls: nothing is counted while the load is still moving");
  Rig g;
  g.commission();
  for (int k = 0; k < 20; k++) {  // a bowl being shuffled about for 20 s
    g.gross = (k % 2) ? 20500 : 16500;
    g.run(1000);
  }
  CHECK(g.events.empty(), "no event while moving (got %zu)", g.events.size());
  g.gross = 18500;
  g.run(8000);
  CHECK(g.count(BowlEvent::Loaded) == 1 && g.r().bowls == 1, "counted once it holds (bowls %u)", g.r().bowls);
}

static void slowConsumption() {
  section("bowls: food taken slowly over 10 min is never mistaken for an unload");
  Rig g;
  g.commission();
  g.gross = 37000;
  g.run(8000);
  g.events.clear();
  for (int i = 1; i <= 600; i++) {  // 6 kg taken off over 600 s
    g.gross = 37000 - 6000.0 * i / 600.0;
    g.run(1000);
  }
  g.run(8000);
  const Reading r = g.r();
  CHECK(g.events.empty(), "no event (got %zu)", g.events.size());
  CHECK(r.bowls == 2, "still 2 bowls (got %u)", r.bowls);
  CHECK(std::fabs(r.foodG - 26000) < 80, "food %.0f g (want 37000 - 6000 - 2x2500 = 26000)", r.foodG);
}

static void restoreAndEmptyReset() {
  section("power cycle: count remembered but unconfirmed; an empty platform confirms 0");
  Rig g;
  Persisted p;
  p.zeroed = true;
  p.zero = (int32_t)ZERO;
  p.cpg = (float)CPG;
  p.bowls = 3;
  g.s.restore(p);
  g.run(500);
  CHECK(g.r().bowls == 3 && !g.r().bowlsConfirmed, "3 bowls, NOT confirmed after restore");
  g.gross = 200;  // empty shelf, crumbs
  g.run(8000);
  const Reading r = g.r();
  CHECK(r.bowls == 0 && r.bowlsConfirmed, "empty platform -> 0 bowls, confirmed (got %u, %d)", r.bowls, r.bowlsConfirmed);
  CHECK(g.count(BowlEvent::EmptyReset) == 1, "one EmptyReset event (got %d)", g.count(BowlEvent::EmptyReset));
}

static void restoreLoadedThenEvent() {
  section("power cycle with bowls on: stays unconfirmed until the next load confirms it");
  Rig g;
  Persisted p;
  p.zeroed = true;
  p.zero = (int32_t)ZERO;
  p.cpg = (float)CPG;
  p.bowls = 2;
  g.s.restore(p);
  g.gross = 37000;
  g.run(15000);
  CHECK(g.r().bowls == 2 && g.r().bowlsConfirmed, "2 bowls, confirmed by the weight -- nobody touches the panel");
  g.gross = 55500;
  g.run(8000);
  CHECK(g.r().bowls == 3 && g.r().bowlsConfirmed, "a load confirms: 3 bowls (got %u, %d)", g.r().bowls, g.r().bowlsConfirmed);
}

static void inconsistentCount() {
  section("bowls: a count heavier than the shelf is limited to what it holds, unconfirmed");
  Rig g;
  g.commission();
  g.s.setBowls(3);  // operator says 3, but only 4.5 kg is on the shelf (2 bowls fit: 4.5 - 2 x 2.5 is inside the 1 kg tolerance)
  g.gross = 4500;
  g.run(8000);
  const Reading r = g.r();
  CHECK(r.bowls == 2 && !r.bowlsConfirmed, "limited to 2 bowls, unconfirmed (got %u, %d)", r.bowls, r.bowlsConfirmed);
  CHECK(g.count(BowlEvent::Clamped) == 1, "one Clamped event (got %d)", g.count(BowlEvent::Clamped));
}

static void restoreImpossibleCount() {
  section("power cycle: a remembered count the shelf cannot hold is limited, not kept");
  // Found on the panel: "2 bw?" over a 3.26 kg load after a power cycle -- food read
  // -1.7 kg and stayed there, because the check skipped an already-unconfirmed count.
  Rig g;
  Persisted p;
  p.zeroed = true;
  p.zero = (int32_t)ZERO;
  p.cpg = (float)CPG;
  p.bowls = 2;
  g.s.restore(p);
  g.gross = 3260;
  g.run(8000);
  const Reading r = g.r();
  CHECK(r.bowls == 1 && !r.bowlsConfirmed, "limited to 1 bowl, unconfirmed (got %u, %d)", r.bowls, r.bowlsConfirmed);
  CHECK(r.kgKnown && std::fabs(r.foodG - 760) < 80, "food %.0f g (want 3260 - 2500 = 760)", r.foodG);
}

static void restoreReestimates() {
  section("power cycle: a count the weight cannot be is re-estimated (field, 2026-10-04)");
  // Two bowls went on while the panel was off: it came back "1 bowl" over 55.7 kg.
  Rig g;
  Persisted p;
  p.zeroed = true;
  p.zero = (int32_t)ZERO;
  p.cpg = (float)CPG;
  p.bowls = 1;
  g.s.restore(p);
  g.gross = 55700;
  g.run(4000);
  CHECK(g.r().bowls == 3 && g.r().bowlsConfirmed, "55.7 kg -> 3 bowls, confirmed by the weight (got %u, %d)",
        g.r().bowls, g.r().bowlsConfirmed);
  CHECK(g.count(BowlEvent::Estimated) == 1, "one Estimated event (got %d)", g.count(BowlEvent::Estimated));
  CHECK(std::fabs(g.r().foodG - 48200) < 100, "food %.0f g (want 55700 - 3 x 2500)", g.r().foodG);
  g.gross = 37700;  // one bowl lifted off: counts from the right base now
  g.run(4000);
  CHECK(g.r().bowls == 2 && g.r().bowlsConfirmed, "then an unload -> 2, confirmed (got %u, %d)",
        g.r().bowls, g.r().bowlsConfirmed);

  // The owner's bench case: a 20 kg bowl on, but 0 remembered.
  Rig h;
  p.bowls = 0;
  h.s.restore(p);
  h.gross = 20000;
  h.run(4000);
  CHECK(h.r().bowls == 1 && h.r().bowlsConfirmed, "0 remembered over 20 kg -> 1, confirmed (got %u)", h.r().bowls);

  // Bowls taken off while off: 3 remembered over 22 kg -- under 3 x 14 kg, the
  // lightest three bowls the tracker could ever have counted.
  Rig k;
  p.bowls = 3;
  k.s.restore(p);
  k.gross = 22000;
  k.run(4000);
  CHECK(k.r().bowls == 1, "3 remembered over 22 kg -> 1 (got %u)", k.r().bowls);

  // And WHILE RUNNING, not only after a restore: the field's second case -- a bowl
  // lifted off a count that was already wrong left "0 bowls" confirmed over 37.6 kg.
  Rig q;
  q.commission();
  q.gross = 37600;
  q.run(4000);
  q.s.setBowls(0);  // a wrong base with the load already settled -- nothing moves
  q.run(4000);
  CHECK(q.r().bowls == 2 && q.r().bowlsConfirmed, "0 over 37.6 kg -> 2, confirmed by the weight (got %u, %d)",
        q.r().bowls, q.r().bowlsConfirmed);

  // A plausible remembered count is KEPT, even where gross / 17 kg would say otherwise:
  // three heavy bowls (3 x 20.5 kg) round to 4.
  Rig m;
  p.bowls = 3;
  m.s.restore(p);
  m.gross = 61500;
  m.run(4000);
  CHECK(m.r().bowls == 3 && m.count(BowlEvent::Estimated) == 0, "3 over 61.5 kg kept (got %u)", m.r().bowls);
}

static void heavyBowlsAndFieldCount() {
  section("bowls: three 20 kg bowls are 3, not 4; 2 remembered over 20.2 kg is 1 (field, 2026-10-04)");
  // round(60 / 17) = 4. The fewest bowls that fit 60 kg at 17-20.5 kg each is 3.
  Rig g;
  g.commission();
  g.gross = 60000;  // three 20 kg bowls placed together
  g.run(4000);
  CHECK(g.r().bowls == 3 && g.r().bowlsConfirmed, "60 kg at once -> 3 bowls (got %u, %d)", g.r().bowls,
        g.r().bowlsConfirmed);
  Rig h;
  Persisted p;
  p.zeroed = true;
  p.zero = (int32_t)ZERO;
  p.cpg = (float)CPG;
  p.bowls = 1;
  h.s.restore(p);
  h.gross = 60000;  // ...and remembered wrong across a power cycle
  h.run(4000);
  CHECK(h.r().bowls == 3 && h.r().bowlsConfirmed, "1 remembered over 60 kg -> 3 (got %u)", h.r().bowls);
  Rig k;
  p.bowls = 2;
  k.s.restore(p);
  k.gross = 20200;  // one bowl, 15 kg of food: the panel showed "2 bowls"
  k.run(4000);
  CHECK(k.r().bowls == 1 && k.r().bowlsConfirmed, "2 remembered over 20.2 kg -> 1, confirmed (got %u, %d)",
        k.r().bowls, k.r().bowlsConfirmed);
}

static void negativeLevelIsNotEmpty() {
  section("bowls: a settled -3 kg is a failing cell, not an empty platform (field, 2026-10-04)");
  Rig g;
  g.commission();
  g.gross = 19100;
  g.run(4000);
  CHECK(g.r().bowls == 1 && g.r().bowlsConfirmed, "1 bowl (got %u)", g.r().bowls);
  g.events.clear();
  g.gross = -3200;
  g.run(4000);
  CHECK(g.r().bowls == 1 && g.count(BowlEvent::EmptyReset) == 0, "still 1, no EmptyReset (got %u)", g.r().bowls);
  g.gross = 19100;  // the cell recovers
  g.run(4000);
  CHECK(g.r().bowls == 1 && g.count(BowlEvent::Loaded) == 0, "still 1, no phantom load (got %u)", g.r().bowls);
}

static void operatorCorrectionIsRechecked() {
  section("bowls: an operator's count is re-checked against the shelf without the load moving");
  // Found on the bench: "bowls 2" typed with the platform still and empty left food
  // reading -4.96 kg indefinitely, because the stable run had already been judged
  // and nothing moved to start a new one.
  Rig g;
  g.commission();
  g.s.setBowls(2);
  g.run(7000);
  const Reading r = g.r();
  CHECK(r.bowls == 0 && r.bowlsConfirmed, "an empty shelf overrules '2 bowls' (got %u, %d)", r.bowls,
        r.bowlsConfirmed);
  CHECK(std::fabs(r.foodG) < 100, "food back near 0 (got %.0f g)", r.foodG);
  // And with load on it, a count heavier than the shelf is flagged, not kept as true.
  Rig h;
  h.commission();
  h.gross = 4500;
  h.run(8000);
  h.events.clear();
  h.s.setBowls(3);
  h.run(7000);
  CHECK(h.r().bowls == 2 && !h.r().bowlsConfirmed, "3 bowls on 4.5 kg is limited to 2, unconfirmed (got %u, %d)",
        h.r().bowls, h.r().bowlsConfirmed);
  CHECK(h.count(BowlEvent::Loaded) == 0 && h.count(BowlEvent::Unloaded) == 0,
        "no phantom load/unload from the correction");
}

static void clampAtMax() {
  section("bowls: a fifth bowl is refused, not invented");
  Rig g;
  g.commission();
  g.gross = 74000;  // four bowls in one go
  g.run(8000);
  CHECK(g.r().bowls == 4 && g.r().bowlsConfirmed, "4 bowls (got %u)", g.r().bowls);
  g.gross = 92500;
  g.run(8000);
  CHECK(g.r().bowls == 4 && !g.r().bowlsConfirmed, "still 4, unconfirmed (got %u, %d)", g.r().bowls, g.r().bowlsConfirmed);
  CHECK(g.count(BowlEvent::Clamped) == 1, "one Clamped event (got %d)", g.count(BowlEvent::Clamped));
}

static void staleCountIsNotOver() {
  section("bowls: stepping off reads 'settling', not over_range, until the count catches up");
  // Found on the bench: a 65 kg person on B1 counted as 4 bowls; stepping off left the
  // count at 4 for the 5 s an unload must hold, so food read 0 - 4 x 2.5 = -10 kg, under
  // the -5 kg floor -- and the panel said OVER for a platform that was simply empty.
  Rig g;
  g.commission();
  g.gross = 65000;
  g.run(8000);
  CHECK(g.r().bowls == 4, "65 kg counted as 4 bowls (got %u)", g.r().bowls);
  g.gross = 0;
  g.run(2000);  // off, but not yet held the 5 s an unload needs
  CHECK(g.r().state == WState::Settling && !g.r().kgKnown, "stale count -> settling, no weight (%s)",
        wstateToken(g.r().state));
  CHECK(std::fabs(g.r().grossG) < 100, "the gross still reads the empty platform (%.0f g)", g.r().grossG);
  g.run(8000);
  CHECK(g.r().state == WState::Ok && g.r().bowls == 0, "then ok, 0 bowls (%s, %u)", wstateToken(g.r().state),
        g.r().bowls);
}

static void counterRole() {
  section("counter role: no bowl tracking, food == gross");
  Rig g(Role::Counter);
  g.commission();
  g.gross = 15000;
  g.run(8000);
  const Reading r = g.r();
  CHECK(g.events.empty() && r.bowls == 0, "no bowl events (got %zu)", g.events.size());
  CHECK(std::fabs(r.foodG - r.grossG) < 0.01, "food == gross (%.0f vs %.0f)", r.foodG, r.grossG);
}

// ---------------------------------------------------------------------------------
static void stateLadder() {
  section("weight_state ladder: each thing that stops this being a weight is named");
  {
    Rig g;
    g.commission();
    g.s.setLink(Link::Offline);
    const Reading r = g.r();
    CHECK(r.state == WState::NoCells && !r.kgKnown && r.counts == 0 && r.samples == 0,
          "offline -> no_cells, no figures (state %s)", wstateToken(r.state));
    g.s.setLink(Link::Online);
    CHECK(g.r().state == WState::Settling, "back online with an empty window -> settling (%s)", wstateToken(g.r().state));
  }
  {
    Rig g;
    g.s.setLink(Link::Warming);
    CHECK(g.r().state == WState::Settling, "warming -> settling");
  }
  {
    Rig g;  // never calibrated, never zeroed
    g.run(4000);
    CHECK(g.r().state == WState::Uncalibrated, "no factor -> uncalibrated (%s)", wstateToken(g.r().state));
  }
  {
    Rig g;
    Persisted p;
    p.cpg = (float)CPG;  // factor but no zero
    g.s.restore(p);
    g.run(4000);
    CHECK(g.r().state == WState::Untared, "no zero -> untared (%s)", wstateToken(g.r().state));
  }
  {
    Rig g;
    g.commission();
    g.gross = 380000;  // counts past 8,000,000: the converter's ceiling
    g.run(4000);
    CHECK(g.r().state == WState::OverRange && !g.r().kgKnown, "ADC ceiling -> over_range (%s)", wstateToken(g.r().state));
  }
  {
    Rig g;
    g.commission();
    g.gross = 262500;  // under the ADC ceiling, over the 250 kg rail even after 0 bowls
    g.run(4000);
    CHECK(g.r().state == WState::OverRange, "past the 250 kg rail -> over_range (%s)", wstateToken(g.r().state));
  }
  {
    Rig g;
    g.commission();
    CHECK(g.r().state == WState::Ok && g.r().kgKnown, "commissioned and still -> ok (%s)", wstateToken(g.r().state));
  }
  {
    Persisted p;
    p.cpg = NAN;  // a corrupted float in NVS must not become kilograms
    LoadScale s;
    s.configure(Role::Buffer, FilterConfig{}, BowlConfig{});
    s.restore(p);
    CHECK(!s.reading().calibrated, "a NaN factor is treated as no factor");
  }
}

static void commits() {
  section("zero / calibrate: refused unless the reading is settled and still");
  Rig g;  // fresh: nothing stored
  g.run(1000);
  CHECK(g.s.zero() == Commit::Settling, "zero refused while the window is filling");
  // A FAST ramp keeps tripping the step detector, so the window restarts every three
  // samples and never fills: refused as Settling. Either refusal is correct; what
  // matters is that nothing is stored from a reading that is still changing.
  for (int i = 0; i < 40; i++) {
    g.gross = i * 200.0;
    g.run(100);
  }
  {
    const Commit c = g.s.zero();
    CHECK(c == Commit::Settling || c == Commit::Moving, "zero refused during a fast ramp (%s)", commitText(c));
  }
  // A SLOW wobble stays under the step threshold, so the window fills -- and its
  // peak-to-peak is what refuses it. +/-68 g is ~1500 counts each way: a 3000+ p-p
  // with no single sample far enough from the mean to restart the window.
  {
    Rig w;
    w.commission();
    for (int i = 0; i < 60; i++) {
      w.gross = 68.0 * std::sin(i * 2.0 * 3.14159265 / 20.0);
      w.run(100);
    }
    const Commit c = w.s.zero();
    CHECK(c == Commit::Moving, "zero refused as Moving during a slow wobble (%s)", commitText(c));
  }
  CHECK(g.s.calibrate(20000) == Commit::NotZeroed, "calibrate refused before a zero");
  g.gross = 0;
  g.run(5000);
  CHECK(g.s.zero() == Commit::Ok, "zero accepted on a still, empty platform");
  CHECK(g.r().bowls == 0 && g.r().bowlsConfirmed, "zeroing confirms an empty stack");
  CHECK(g.s.calibrate(500) == Commit::MassTooSmall, "500 g refused as too small");
  CHECK(g.s.calibrate(2000) == Commit::NoDeflection, "claiming 2 kg with nothing on it is refused");
  g.gross = -500;  // reading FELL
  g.run(5000);
  CHECK(g.s.calibrate(2000) == Commit::Negative, "a negative deflection is named as such");
  g.gross = 20000;
  g.run(5000);
  CHECK(g.s.calibrate(1000000) == Commit::Implausible, "a mass typed 50x too big is implausible");
  float cpg = 0;
  CHECK(g.s.calibrate(20000, &cpg) == Commit::Ok, "20 kg accepted");
  CHECK(std::fabs(cpg - CPG) / CPG < 0.01, "factor %.3f within 1%% of %.3f", cpg, CPG);
  CHECK(g.s.dirty(), "a calibration marks the persisted state dirty");
  g.s.setLink(Link::Offline);
  CHECK(g.s.zero() == Commit::NotConverting, "zero refused while offline");
}

// The counter's vessel rule, in the cases the kitchen described.
static void vesselRule() {
  section("vessel rule (counter)");
  const float V = 2500;
  CHECK(netOfVessel(0, V) == 0, "empty platform reads 0, not -2.5 kg");
  CHECK(netOfVessel(100, V) == 100, "a 100 g item with no vessel reads 100 g");
  CHECK(netOfVessel(2650, V) == 150, "vessel + 150 g reads 150 g");
  CHECK(netOfVessel(2600, V) == 100, "a dish run down to 100 g residue reads 100 g");
  CHECK(netOfVessel(2400, V) == -100, "an empty LIGHTER vessel reads -100 g, not 2.4 kg of food");
  CHECK(netOfVessel(12500, V) == 10000, "a full dish reads its food");
  CHECK(!vesselOnPlatform(1249, V) && vesselOnPlatform(1250, V), "the line is half the vessel");
  CHECK(netOfVessel(2650, 0) == 2650 && !vesselOnPlatform(2650, 0), "offset off: gross untouched");
}

int main() {
  std::printf("load_scale host tests\n");
  vesselRule();
  filterStepResponse();
  filterNoFalseSteps();
  singleLoad();
  doubleLoad();
  countsOneAtATimeAnyWeight();
  countsEveryMixOfBowls();
  typicalIsFixed();
  unload();
  belowThreshold();
  unstableThenStable();
  slowConsumption();
  restoreAndEmptyReset();
  restoreLoadedThenEvent();
  inconsistentCount();
  restoreImpossibleCount();
  restoreReestimates();
  heavyBowlsAndFieldCount();
  negativeLevelIsNotEmpty();
  operatorCorrectionIsRechecked();
  clampAtMax();
  staleCountIsNotOver();
  counterRole();
  stateLadder();
  commits();
  std::printf("\n%d checks, %d failed -> %s\n", g_checks, g_fails, g_fails ? "FAIL" : "PASS");
  return g_fails ? 1 : 0;
}
