// Panel controls: a rotary encoder with a push switch, charge sense, and the
// status LED. See board_waveshare_s3.h section 9 for the pins and why each one
// is where it is.
//
// WHAT THIS OWNS AND WHAT IT REFUSES TO OWN. It debounces, decodes and reports.
// It does not decide what a turn MEANS -- no menu navigation, no brightness, no
// screen blanking -- because those belong to the UI and the power manager, and
// a driver that reaches into either becomes untestable and unshareable. The UI
// asks what happened; this says what happened.
//
// ONE READER. `loop()` must be called from exactly one task, and the same task
// that consumes the events. The encoder's own decode runs in an ISR because a
// hand-turned detent produces four transitions inside a few milliseconds and
// the render loop cannot promise to be there for them -- but that ISR does
// nothing except accumulate an integer, which `loop()` drains under a critical
// section. Everything else is polled, because a plug and a fingertip are slow.

#pragma once

#include <Arduino.h>

namespace inputs {

// What `loop()` observed since the last call. Deliberately a value rather than
// a callback: the caller decides when to act, which matters when the consumer
// is an LVGL task that must not be re-entered from a driver.
struct Events {
  // Net detents this tick, signed. NOT a count of interrupts -- one click of an
  // EC11 is four quadrature transitions, and a wobble that goes out and comes
  // back cancels to zero rather than reporting two turns.
  int16_t turned = 0;

  bool pressed = false;   // switch went down this tick
  bool released = false;  // ...and came back up
  bool longPress = false; // held past LONG_PRESS_MS, fired once per hold

  // Edges, not levels. `charging` is the ETA6098 actually pushing current;
  // `external` is 5 V present. They diverge when the cell is full, which is
  // the whole reason both exist -- see board_waveshare_s3.h section 9.
  bool chargeChanged = false;
  bool externalChanged = false;

  bool any() const {
    return turned || pressed || released || longPress || chargeChanged ||
           externalChanged;
  }
};

void begin();

// Drains the ISR accumulator and re-polls the slow inputs. Returns what
// changed. Call it every pass of the main loop -- it is a few microseconds when
// nothing has happened.
Events loop(uint32_t nowMs);

// --- current state ---------------------------------------------------------
int32_t position();       // cumulative detents since boot, signed
bool switchDown();
bool charging();          // STAT low. Meaningless unless chargeSenseFitted().
bool externalPower();     // VBUS present
bool chargeSenseFitted(); // board::CHARGER_STATUS_READABLE, restated here so
                          // callers do not have to include the board header

void setStatusLed(bool on);
bool statusLed();

// --- bring-up ---------------------------------------------------------------
// Raw pin levels, undebounced and undecoded, for confirming a solder joint
// before trusting anything above. `traceLine()` formats them into one console
// line; `dumpState()` prints the whole picture including the decoder's own
// health counters.
//
// THE COUNTERS ARE THE POINT. A rotary encoder that reads as jumpy and one that
// is wired to the wrong pins look identical from the position figure alone --
// both give the wrong number. `invalidTransitions()` separates them: wiring
// faults produce almost none because the pin simply never moves, while contact
// bounce produces a stream of them.
void traceLine(char *out, size_t n);
void dumpState();
uint32_t transitions();         // valid quadrature edges seen
uint32_t invalidTransitions();  // edges the state table rejected
int8_t subPosition();           // partial detent, -3..3

}  // namespace inputs
