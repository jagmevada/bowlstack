// Turns per-level distances into a bowl count.
//
// Two things happen here. Each level is thresholded with hysteresis into
// present/absent, then the stack's physical constraint -- bowls rest on each
// other and cannot float -- is applied to derive a count and catch impossible
// readings.

#pragma once

#include <Arduino.h>

#include "config.h"
#include "sensor_array.h"

enum class LevelState : uint8_t {
  Absent,
  Present,
  Unknown,  // sensor offline; this level's true state is not observable
};

enum class StackStatus : uint8_t {
  Ok,             // count is trustworthy
  Discontiguous,  // a present level sits above an absent one -- impossible
  Degraded,       // an offline sensor leaves the count ambiguous
};

class BowlLogic {
 public:
  void update(const SensorArray &sensors);

  // Same logic, fed directly rather than through the hardware class.
  //
  // Exists so the fleet simulator can be a DIGITAL TWIN rather than a plausible
  // imitation: it synthesises distances and sensor health, then runs this --
  // the production presence hysteresis, contiguity rule, count and status --
  // instead of reimplementing them. A reimplementation drifts, and it did:
  // the simulator's own copy disagreed with recompute() on 23 of the 80
  // reachable states, including reintroducing a bug this file had already
  // fixed. There is now one implementation, so that cannot recur.
  //
  // update(SensorArray) forwards here, so the production path is unchanged.
  void update(const SensorState *states, const Reading *readings);

  uint8_t count() const { return count_; }
  StackStatus status() const { return status_; }
  LevelState level(uint8_t i) const { return level_[i]; }

  // True when the count is safe to act on.
  bool trustworthy() const { return status_ == StackStatus::Ok; }

  // Human-readable, for the serial log and plotter only. Free to change.
  static const char *stateName(LevelState s);
  static const char *statusName(StackStatus s);

  // Canonical lowercase tokens for the wire. These are pinned by CHECK
  // constraints in supabase/schema.sql, so they are an API, not a display
  // choice -- editing one 400s the entire fleet at once. Kept separate from
  // stateName/statusName precisely so a cosmetic tidy-up of the log format
  // cannot silently break telemetry.
  static const char *wireName(LevelState s);
  static const char *wireName(StackStatus s);

 private:
  void recompute();

  // Hysteresis state, kept separately from level_ so it survives a sensor
  // dropping offline and returning: level_ overlays sensor health on top of
  // it, rather than destroying the last known presence.
  bool present_[config::SENSOR_COUNT] = {false};
  LevelState level_[config::SENSOR_COUNT] = {LevelState::Unknown};

  uint8_t count_ = 0;
  StackStatus status_ = StackStatus::Degraded;
};
