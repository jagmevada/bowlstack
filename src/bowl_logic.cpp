#include "bowl_logic.h"

void BowlLogic::update(const SensorArray &sensors) {
  SensorState states[config::SENSOR_COUNT];
  Reading readings[config::SENSOR_COUNT];
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    states[i] = sensors.state(i);
    readings[i] = sensors.reading(i);
  }
  update(states, readings);
}

void BowlLogic::update(const SensorState *states, const Reading *readings) {
  for (uint8_t i = 0; i < config::SENSOR_COUNT; i++) {
    if (states[i] != SensorState::Online) {
      // Do NOT touch present_: an offline sensor tells us nothing about the
      // bowl, so the last observation is preserved for when it returns.
      level_[i] = LevelState::Unknown;
      continue;
    }

    const Reading r = readings[i];

    if (!r.valid) {
      // No target anywhere in range. That is a direct observation of absence,
      // not a missing measurement, so it drives the trigger low outright
      // rather than going through the distance thresholds.
      present_[i] = false;
    } else if (r.distanceMm < config::PRESENT_BELOW_MM) {
      present_[i] = true;
    } else if (r.distanceMm > config::ABSENT_ABOVE_MM) {
      present_[i] = false;
    }
    // Between the thresholds the previous state is held -- this is the
    // hysteresis, and the reason a reading parked near a boundary does not
    // chatter.

    level_[i] = present_[i] ? LevelState::Present : LevelState::Absent;
  }

  recompute();
}

void BowlLogic::recompute() {
  // Bowls rest on one another, so a stack is always contiguous from the
  // bottom. That single physical fact does two useful things: it detects
  // impossible sensor output, and it lets a level whose sensor is dead be
  // inferred from the levels above it.

  int8_t topPresent = -1;
  for (int8_t i = 0; i < (int8_t)config::SENSOR_COUNT; i++) {
    if (level_[i] == LevelState::Present) topPresent = i;
  }

  // Anything below the highest present bowl must itself be present. An Absent
  // down there is physically impossible -- a floating bowl -- so it means a
  // failed sensor, a misaligned mount, or an obstruction, and must be
  // reported as a fault rather than silently counted.
  for (int8_t i = 0; i < topPresent; i++) {
    if (level_[i] == LevelState::Absent) {
      count_ = 0;
      status_ = StackStatus::Discontiguous;
      return;
    }
  }

  // An Unknown BELOW topPresent needs no sensor: contiguity proves a bowl is
  // there. So a dead sensor low in the stack costs nothing, which is worth
  // having while redundancy is deferred.
  //
  // An Unknown ABOVE topPresent is a different matter -- the stack might
  // extend into it, so the count becomes a lower bound rather than a fact.
  bool ambiguous = false;
  for (int8_t i = topPresent + 1; i < (int8_t)config::SENSOR_COUNT; i++) {
    // Stop at the first observed Absent. Nothing can rest above an empty
    // level, so contiguity has already settled every level beyond it and an
    // Unknown up there proves nothing. Scanning past this point flagged 12 of
    // the 81 possible level combinations as ambiguous when the count was in
    // fact exact -- including the everyday case of an empty stack with one
    // dead sensor, which could then never report OK again.
    if (level_[i] == LevelState::Absent) break;
    if (level_[i] == LevelState::Unknown) ambiguous = true;
  }

  count_ = (uint8_t)(topPresent + 1);
  status_ = ambiguous ? StackStatus::Degraded : StackStatus::Ok;
}

const char *BowlLogic::stateName(LevelState s) {
  switch (s) {
    case LevelState::Present: return "PRESENT";
    case LevelState::Absent:  return "absent";
    default:                  return "UNKNOWN";
  }
}

const char *BowlLogic::statusName(StackStatus s) {
  switch (s) {
    case StackStatus::Ok:            return "OK";
    case StackStatus::Discontiguous: return "DISCONTIGUOUS";
    default:                         return "DEGRADED";
  }
}

// The wire vocabulary. Must match the CHECK constraints in
// supabase/schema.sql exactly -- these strings are a contract with the server,
// not a presentation detail.

const char *BowlLogic::wireName(LevelState s) {
  switch (s) {
    case LevelState::Present: return "present";
    case LevelState::Absent:  return "absent";
    default:                  return "unknown";
  }
}

const char *BowlLogic::wireName(StackStatus s) {
  switch (s) {
    case StackStatus::Ok:            return "ok";
    case StackStatus::Discontiguous: return "discontiguous";
    default:                         return "degraded";
  }
}
