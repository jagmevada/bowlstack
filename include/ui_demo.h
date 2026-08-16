// The mock device states, shared by the desktop preview and the device build.
//
// Deliberately ONE definition rather than two. The point of the simulator is
// that what you see there is what the panel will show, and that guarantee is
// only as good as the weakest thing the two builds have in common. If each kept
// its own fixture list they would drift -- silently, and in the direction of the
// one being looked at more often -- and the preview would quietly stop being
// evidence about the device.
//
// It also does the job todo.md lists as the blocker for UI work: something that
// produces DeviceStatus-shaped values with no sensors attached. When the real
// sensors arrive this is what gets swapped out, and nothing in ui_screens.cpp
// has to know it happened.

#pragma once

#include <stdint.h>

#include "ui_state.h"

namespace ui {

// The states worth looking at: not just the happy path, but the ones that are
// awkward to produce on a bench and disastrous to get wrong in a kitchen --
// an impossible stack, a dead sensor, a missing cell, a dropped link.
uint8_t demoCount();
const char *demoName(uint8_t i);
State demoState(uint8_t i);

// Advances on a fixed cadence and pushes into the UI. Driven by a caller-
// supplied millisecond clock so this file needs neither Arduino nor SDL --
// which is exactly what lets both builds share it.
//
// Returns true when the scenario changed, so a caller can log it.
bool demoTick(uint32_t nowMs);

// Current index, for callers that want to name what is on screen.
uint8_t demoIndex();

// The current fabricated state, with the wall clock applied. Held here rather
// than pushed into widgets by demoTick, so a page can stop rendering without
// its data going stale -- which is the whole point of ui_pages' visibility
// handling.
const State &demoLatest(uint32_t nowMs);

// Installs the fabricated WiFi scan results and connection state.
//
// It lives HERE, with the rest of the mock data, for the same reason the
// scenarios do: both targets must be handed identical values or the desktop
// preview stops being evidence about the device. That is not hypothetical --
// the sim was previously told it was connected to a network at -58 dBm while
// the device was told nothing, so the two rendered different headers from the
// same source file, and it read as a rendering bug.
//
// The MAC is deliberately NOT set here: the device has a real one in its eFuse
// and the preview does not, which is a difference in the world rather than in
// the fixtures.
void demoInstallWifiMocks();

}  // namespace ui
