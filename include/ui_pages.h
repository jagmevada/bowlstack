// The two pages, swipeable.
//
//   1  menu   -- settings tree, unchanged from the touch-ui branch
//   2  weight -- the shipping screen, and home
//
// The menu tree keeps its shape. What changed on this branch is what the pages
// UNDER it are about: Sensors is the load-cell scope rather than four ToF
// traces, and Settings gained a Scale entry for tare and calibration, which are
// the two things a weighing device cannot work without and which have no home
// on a readout-only dashboard.
//
// Replaces the earlier six-page gallery. Those pages existed to settle
// questions -- which type sizes are legible on this panel, how big a touch
// target needs to be, whether pixel-shifting softens edges -- and all three are
// settled and written down: the type scale in lv_conf.h, the shift verdict in
// todo.md. Scaffolding kept past the decision it was built for stops being
// documentation and starts being clutter. Git has them if they are ever wanted.

#pragma once

#include <stdint.h>

namespace ui {

// Replaces the active screen with a two-page tileview.
void buildPages();

// Drives whichever page needs a clock. Call every loop iteration.
void pagesTick(uint32_t nowMs);

// Returns to the stock page and closes any detail overlay. Called by the idle
// timeout, and available to anything else that needs to get home.
void pagesGoHome();

// Up one level in the menu tree. Wired to every sub-page's back button.
void pagesBack();

// --- what the Scale page does ----------------------------------------------
// Function pointers rather than direct calls, for the same reason ui_state.h
// exists: src/ui/ is pure LVGL and cannot reach scale.cpp, which needs
// Arduino, FreeRTOS and NVS. The firmware installs the real handlers; the
// desktop preview installs none and the rows are inert, which is the honest
// behaviour for a machine with no cells attached.
//
// Calibration itself is NOT here: it has its own page with a keypad, and it
// installs its own handler through ui_calib.h. See that header for why the mass
// stopped being a build constant.
void pagesOnScaleTare(void (*cb)(void));
void pagesOnScaleClearCal(void (*cb)(void));

// Steps the moving-average length to the next of 8/16/32/64/128 and wraps. A
// runtime setting rather than a build flag because the right value is a
// judgement about the gesture -- how long a bowl may take to settle against how
// much the last digit may wander -- and a judgement is far easier to make by
// flipping between two of them with a mass on the platform than by reflashing.
void pagesOnScaleCycleAvg(void (*cb)(void));

// The empty-vessel offset, in GRAMS, from the keypad on Settings > Scale >
// Vessel offset. Zero means no offset -- there is no separate enable.
//
// This was a cycling row offering half-kilogram steps, on the reasoning that
// ten vessels averaged 2.5 kg. Wrong about the kitchen: a real vessel weighs
// 1.1 or 2.6 kg, and rounding a SUBTRACTED mass to the nearest 500 g injects up
// to 250 g of error into every reading the trial exists to measure.
void pagesOnVesselApply(void (*cb)(float grams));

// Settings > Precision: steps the dashboard between 0.0 and 0.00 kg. The firmware
// persists it; the row's hint reads ScaleView::decimals back.
void pagesOnCyclePrecision(void (*cb)(void));

// Settings > Vessel correction: on/off for every platform at once (C1's vessel
// offset, the buffers' bowls x dry mass). The firmware persists it; the row's hint
// reads ScaleView::vesselOn back.
void pagesOnToggleVessel(void (*cb)(void));

// Loads the factor the firmware was built with and persists it as this unit's
// own. It is what lets "Clear calibration" mean cleared: without a deliberate
// route back to the built-in figure, clearing had to leave the stored key
// absent so a reflash could still take effect -- and a unit cleared on purpose
// then came back from its next power cycle showing kilograms again.
void pagesOnScaleRestore(void (*cb)(void));

// Stores the platform's own weight in NVS -- the commissioning zero, done once
// per device after the platform is bolted on. Distinct from Tare, which is
// volatile and belongs to the next measurement rather than to the assembly.
void pagesOnScalePlatformZero(void (*cb)(void));

// (pagesOnScaleToggleCells is gone with the A/B/C rows it showed; the per-cell
// breakdown lives on Settings > Diagnose.)

// --- Settings > Buffers ---------------------------------------------------------
// One handler per command, each told WHICH buffer platform (slot 0..2 = B1..B3).
// INERT in the desktop preview, where nothing installs them. All four only queue
// work for the firmware's buffer task; the outcome comes back as
// State::buffers[slot].lastResult and is shown on that platform's page.
//
// Zero and Clear take two taps on the panel (the first arms the row) because both
// are stored and a stray one is invisible as a mistake. See ui_pages.cpp.
void pagesOnBufferZero(void (*cb)(uint8_t slot));
void pagesOnBufferCalibrate(void (*cb)(uint8_t slot, float grams));
void pagesOnBufferBowls(void (*cb)(uint8_t slot, int8_t delta));
void pagesOnBufferClear(void (*cb)(uint8_t slot));

// PREVIEW ONLY: opens an overlay by name ("settings", "scale", "buffers", "buf1".."buf3",
// "bufcal", "diagnose") so the simulator can screenshot it. Nothing on the device
// calls it.
void pagesPreviewOpen(const char *name);

// DIAGNOSTIC: builds every lazily built page now, without showing any. Pages are
// never freed once built, so the LVGL pool after this is the most a session can ask
// of it -- the number to check whenever a page is added.
void pagesBuildAll();

// TRIAL HARNESS. Cycles which page the device settles on -- weight or knob.
// The UI does not persist it: src/ui/ has no NVS and no business having one, so
// the platform stores the choice and hands it back through State::defaultPage.
void pagesOnCycleDefaultPage(void (*cb)(void));

// How long without a touch before the UI returns to the stock page.
//
// A device left on the WiFi page is a device whose primary readout -- the bowl
// count someone walked over to see -- is not on screen. It also stops the more
// expensive pages rendering unattended, which matters on a board that has
// sensors to poll and a network to hold up.
static const uint32_t IDLE_HOME_MS = 60000;

// THE KEYPAD PAGES GET FIVE MINUTES INSTEAD OF ONE, and it is not a preference.
// A WPA2 passphrase on a 240 px on-screen keyboard is two or three minutes of
// deliberate work, read off a slip of paper, with pauses. Every key tap resets
// LVGL's inactivity clock, so continuous typing was always safe -- but one look
// away longer than a minute and pagesGoHome() closed the overlay and
// wifiResetView() wiped the half-typed key, with nothing on screen to say why.
//
// Not disabled outright: a panel abandoned on a password screen is a panel not
// showing the page this device exists to show, and during the trial that is the
// blinded knob page. Five minutes is longer than anyone types and shorter than
// a walk away.
static const uint32_t IDLE_ENTRY_MS = 300000;

}  // namespace ui
