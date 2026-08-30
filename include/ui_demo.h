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

// Replaces the fabricated battery figures with measured ones.
//
// This is the SECOND legitimate platform-specific source, alongside the MAC:
// the device has an ADC on a real divider and the desktop has neither. Once
// called, every demoLatest() carries the measured values, so the battery page
// on hardware shows the cell in front of it rather than a fixture.
//
// The simulator never calls it and keeps the fabricated values, which is what
// lets the page's states -- full, critical, no cell -- be looked at without
// having to discharge an actual battery to see them.
void demoOverrideBattery(uint16_t cellMv, uint16_t pinMv, int8_t pct, Battery band);

// Replaces the fabricated bowl state with a measured one, and STOPS the
// scenario cycle. Once real sensors are talking, rotating through fixtures
// would overwrite them a third of a second later -- and a screen that alternates
// between the truth and a demo is worse than either.
//
// The battery override stays separate because the two arrive from different
// places at different rates, and either can exist without the other: sensors
// wired but no cell, or a cell but no sensors, are both real states of this
// board today.
void demoOverrideState(const State &s);

// Link state from the radio. Separate from demoOverrideState because the two
// arrive from different places at different rates, and the status bar showed
// four solid signal bars on a disconnected device when the fixture's value was
// allowed to ride along with the sensor state.
void demoOverrideWifi(bool connected, int16_t rssi);

// Wall clock from NTP. Separate again, because it arrives from a third place on
// a third cadence -- and because on this board it is legitimately absent until
// a sync lands, which is a state the bar has to be able to show.
void demoOverrideTime(bool known, uint8_t hh, uint8_t mm);

// What the board can see of the charger, stated by the firmware rather than
// inherited from a fixture.
//
// IT RENDERED CORRECTLY BY LUCK, WHICH IS WHY THIS EXISTS. Nothing on hardware
// set either field: demoOverrideState() copies the adapter's snapshot, and that
// snapshot begins life as demoLatest()'s seed -- scenario 0, whose base() leaves
// chargingKnown false. The panel therefore said "unknown", correctly, because
// of the order the scenario list happens to be written in. sCharging() sets
// chargingKnown true, so moving it to the front would have put a charging bolt
// on a board whose ETA6098 STAT pin reaches no GPIO at all.
//
// `known` is board::CHARGER_STATUS_READABLE on the device, which is false until
// somebody fits the one-resistor mod in todo.md. The simulator does not call
// this, so its scenarios keep exercising both states -- which is what the
// preview is for.
void demoOverrideCharging(bool known, bool charging);

// The panel knob's position and press count, from whatever is reading the
// encoder. Platform-specific for the obvious reason -- the desktop has no
// GPIO -- and installed through the same fixture as everything else, so the
// simulator renders the row from the product's own code rather than from a
// second copy of it. Until this is called the fixture supplies a demo value,
// which is what makes the row visible in `pio run -e sim`.
void demoOverrideEncoder(int32_t pos, uint32_t pressCount);

}  // namespace ui
