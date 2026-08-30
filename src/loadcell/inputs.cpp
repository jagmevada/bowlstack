#include "inputs.h"

#include "board_waveshare_s3.h"

namespace inputs {
namespace {

// --- quadrature -------------------------------------------------------------
// A TRANSITION TABLE, NOT AN EDGE COUNT, and the difference is bounce.
//
// The naive decoder -- interrupt on CLK's falling edge, read DT, call it a
// direction -- works on a clean signal and fails on a mechanical encoder,
// because the contacts bounce and every bounce looks like another click. The
// usual patch is a time filter, which then eats fast turns.
//
// This instead tracks the two-bit state and consults what the PREVIOUS state
// was. A valid quadrature step moves exactly one bit, and the table says which
// way. A bounce moves a bit and moves it straight back, so it scores +1 then
// -1 and cancels to nothing. A transition that changes BOTH bits at once is
// impossible on a real encoder -- it means an edge was missed -- and scores
// zero rather than guessing at the direction.
//
// Index is (previous << 2) | current, where the state is (CLK << 1) | DT.
const int8_t QDEC[16] = {
     0, -1, +1,  0,
    +1,  0,  0, -1,
    -1,  0,  0, +1,
     0, +1, -1,  0,
};

// FOUR TRANSITIONS PER CLICK is what a detented EC11 gives: the shaft rests
// with both contacts open and passes through the whole Gray cycle between
// detents. Some cheap encoders are half-step and give two. If a click reports
// as two turns, this is the number to change -- and `subPosition()` in the
// bring-up dump is how you find that out in one turn of the knob rather than by
// reading a datasheet nobody shipped with the part.
const int8_t STEPS_PER_DETENT = 4;

// Set true if CW turns report negative. Which pin is "A" is a property of how
// the encoder was wired, not of the encoder, so this is a wiring constant and
// not a preference -- swapping two Dupont wires does the same job.
const bool INVERT_DIRECTION = false;

// ISR state. Touched from both an interrupt and the main loop, so every access
// is inside the spinlock -- `volatile` alone would not make the accumulator's
// read-and-clear atomic against an interrupt landing between the two halves.
portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t encPrev_ = 0;
volatile int8_t encSub_ = 0;
volatile int16_t encPending_ = 0;
volatile uint32_t encEdges_ = 0;
volatile uint32_t encInvalid_ = 0;

int32_t position_ = 0;

void IRAM_ATTR encIsr() {
  // digitalRead is IRAM_ATTR in this core, so it is safe from an ISR. Both pins
  // are read here rather than one being inferred: inferring the other is the
  // same mistake as counting edges, one layer down.
  const uint8_t s = (uint8_t)((digitalRead(board::PIN_ENC_CLK) << 1) |
                              digitalRead(board::PIN_ENC_DT));

  portENTER_CRITICAL_ISR(&mux_);
  const uint8_t idx = (uint8_t)((encPrev_ << 2) | s);
  if (idx != (uint8_t)((encPrev_ << 2) | encPrev_)) {  // state actually moved
    const int8_t d = QDEC[idx];
    if (d == 0) {
      // Both bits changed: an edge was missed, usually because the knob was
      // spun hard. Counted rather than corrected -- guessing a direction here
      // is how a fast turn ends up reporting backwards.
      encInvalid_++;
    } else {
      encEdges_++;
      encSub_ = (int8_t)(encSub_ + d);
      while (encSub_ >= STEPS_PER_DETENT) {
        encSub_ = (int8_t)(encSub_ - STEPS_PER_DETENT);
        encPending_++;
      }
      while (encSub_ <= -STEPS_PER_DETENT) {
        encSub_ = (int8_t)(encSub_ + STEPS_PER_DETENT);
        encPending_--;
      }
    }
    encPrev_ = s;
  }
  portEXIT_CRITICAL_ISR(&mux_);
}

// --- the slow inputs --------------------------------------------------------
// One filter for three signals that bounce on completely different timescales,
// so the dwell is a parameter rather than a constant. A fingertip on a tactile
// switch settles in a few milliseconds; a USB plug scraping into its socket can
// chatter for tens.
struct Debounced {
  bool level = false;
  bool candidate = false;
  bool primed = false;
  uint32_t sinceMs = 0;
};

// Returns true when the STABLE level changed. The first call primes the filter
// and reports nothing -- a device that boots with the charger already connected
// should not announce that it was just plugged in.
bool settle(Debounced &d, bool raw, uint32_t nowMs, uint32_t dwellMs) {
  if (!d.primed) {
    d.primed = true;
    d.level = d.candidate = raw;
    d.sinceMs = nowMs;
    return false;
  }
  if (raw != d.candidate) {
    d.candidate = raw;
    d.sinceMs = nowMs;
    return false;
  }
  if (raw != d.level && (uint32_t)(nowMs - d.sinceMs) >= dwellMs) {
    d.level = raw;
    return true;
  }
  return false;
}

const uint32_t SWITCH_DWELL_MS = 25;
const uint32_t CHARGE_DWELL_MS = 60;
const uint32_t LONG_PRESS_MS = 700;

Debounced sw_, stat_, vbus_;
uint32_t swDownAtMs_ = 0;
bool longFired_ = false;
bool ledOn_ = false;
LedMode ledMode_ = LedMode::Auto;

// Half-period of the mains blink. 500 ms each way is the 1 Hz asked for.
const uint32_t BLINK_HALF_MS = 500;

void driveLed(bool on) {
  ledOn_ = on;
  digitalWrite(board::PIN_STATUS_LED,
               (on == board::STATUS_LED_ACTIVE_HIGH) ? HIGH : LOW);
}

// FREE-RUNNING off millis() rather than a toggle with its own timer. A stored
// phase would have to be reset whenever the mode changed, and getting that
// wrong gives a blink that stalls on one edge -- which reads as a failed LED.
// Derived from the clock, it cannot stall; it only glitches once per millis()
// wrap, every 49.7 days, by at most one half-period.
void applyLedPolicy(uint32_t nowMs) {
  switch (ledMode_) {
    case LedMode::ForceOn:  driveLed(true);  return;
    case LedMode::ForceOff: driveLed(false); return;
    case LedMode::Auto:
    default:
      // Steady on battery, blinking on mains. The steady state is the one that
      // costs current, and it is deliberately the battery one: a light left on
      // a device that looks switched off is the message.
      driveLed(vbus_.level ? (((nowMs / BLINK_HALF_MS) & 1u) == 0u) : true);
      return;
  }
}

}  // namespace

void begin() {
  // INPUT_PULLUP on all three encoder lines. A KY-040-style breakout brings its
  // own 10k to its + rail, which parallels this to ~8k -- harmless and in fact
  // better. A bare EC11 has nothing, and the internal pull-up alone is what
  // makes it work at all.
  pinMode(board::PIN_ENC_CLK, INPUT_PULLUP);
  pinMode(board::PIN_ENC_DT, INPUT_PULLUP);
  pinMode(board::PIN_ENC_SW, INPUT_PULLUP);

  // STAT is open-drain and pulls LOW while charging; nothing else drives the
  // net, so the pull-up is what supplies the idle high. board section 5 has the
  // arithmetic showing VBAT cannot push this pin above the rail.
  pinMode(board::PIN_CHARGE_STAT, INPUT_PULLUP);

  // PULL-DOWN, not pull-up. An external 5k/10k divider already sets the level,
  // so this one is redundant BY DESIGN: if the 10k leg is ever knocked off the
  // prototype, the pin still reads a definite LOW rather than floating and
  // reporting mains power that is not connected. INPUT_PULLUP here would fight
  // the 5 V source and read high forever.
  pinMode(board::PIN_VBUS_SENSE, INPUT_PULLDOWN);

  pinMode(board::PIN_STATUS_LED, OUTPUT);
  driveLed(false);

  // Seed the decoder from the pins as they are RIGHT NOW. Starting from a
  // hardcoded zero would make the first movement look like a transition from a
  // state the shaft was never in, which the table would score as invalid and
  // the user would see as a first click that did nothing.
  encPrev_ = (uint8_t)((digitalRead(board::PIN_ENC_CLK) << 1) |
                       digitalRead(board::PIN_ENC_DT));

  attachInterrupt(digitalPinToInterrupt(board::PIN_ENC_CLK), encIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(board::PIN_ENC_DT), encIsr, CHANGE);

  Serial.println("\n--- panel controls ---");
  Serial.printf("  encoder  CLK GPIO%d (P1-9)  DT GPIO%d (P1-8)  SW GPIO%d (P1-6)\n",
                board::PIN_ENC_CLK, board::PIN_ENC_DT, board::PIN_ENC_SW);
  Serial.println("           !! P1-7 between SW and DT is CELL_SDA -- no wire there");
  Serial.printf("  LED      GPIO%d (P1-3), active %s -- steady on battery, "
                "1 Hz on mains\n",
                board::PIN_STATUS_LED,
                board::STATUS_LED_ACTIVE_HIGH ? "HIGH" : "LOW");
  Serial.printf("  charge   GPIO%d (P2-8) STAT, LOW = charging -- %s\n",
                board::PIN_CHARGE_STAT,
                board::CHARGER_STATUS_READABLE
                    ? "mod fitted, will be published"
                    : "NOT declared fitted; sensed and printed here, published "
                      "as unknown until -DBOWLSTACK_CHARGE_SENSE=1");
  Serial.printf("  vbus     GPIO%d (P1-10) 5k from P1-14 (5V), 10k to GND\n",
                board::PIN_VBUS_SENSE);
}

Events loop(uint32_t nowMs) {
  Events e;

  // Drain the ISR's accumulator in one go. Read-and-clear rather than read-then
  // -clear, so a detent arriving between the two is carried to the next tick
  // instead of being dropped.
  portENTER_CRITICAL(&mux_);
  const int16_t pending = encPending_;
  encPending_ = 0;
  portEXIT_CRITICAL(&mux_);

  if (pending != 0) {
    e.turned = INVERT_DIRECTION ? (int16_t)-pending : pending;
    position_ += e.turned;
  }

  const bool swRaw = (digitalRead(board::PIN_ENC_SW) == LOW);  // pull-up: LOW = pressed
  if (settle(sw_, swRaw, nowMs, SWITCH_DWELL_MS)) {
    if (sw_.level) {
      e.pressed = true;
      swDownAtMs_ = nowMs;
      longFired_ = false;
    } else {
      e.released = true;
    }
  }
  if (sw_.level && !longFired_ &&
      (uint32_t)(nowMs - swDownAtMs_) >= LONG_PRESS_MS) {
    longFired_ = true;
    e.longPress = true;
  }

  e.chargeChanged =
      settle(stat_, digitalRead(board::PIN_CHARGE_STAT) == LOW, nowMs, CHARGE_DWELL_MS);
  e.externalChanged =
      settle(vbus_, digitalRead(board::PIN_VBUS_SENSE) == HIGH, nowMs, CHARGE_DWELL_MS);

  // AFTER the VBUS read, so a plug event is reflected on the same tick it is
  // reported rather than 250 ms later. Unconditional -- the blink needs driving
  // every pass, not only when something changed.
  applyLedPolicy(nowMs);

  return e;
}

int32_t position() { return position_; }
bool switchDown() { return sw_.level; }
bool charging() { return stat_.level; }
bool externalPower() { return vbus_.level; }
bool chargeSenseFitted() { return board::CHARGER_STATUS_READABLE; }

void setLedMode(LedMode m) { ledMode_ = m; }
LedMode ledMode() { return ledMode_; }
const char *ledModeName() {
  switch (ledMode_) {
    case LedMode::ForceOn:  return "forced ON";
    case LedMode::ForceOff: return "forced off";
    default:                return "auto (steady on battery, 1 Hz on mains)";
  }
}
bool statusLed() { return ledOn_; }

uint32_t transitions() { return encEdges_; }
uint32_t invalidTransitions() { return encInvalid_; }
int8_t subPosition() { return encSub_; }

void traceLine(char *out, size_t n) {
  // RAW levels, deliberately bypassing every filter above. This is the line
  // that says whether a wire is on the pin at all, and a debounced view would
  // hide exactly the fault it is meant to find.
  snprintf(out, n,
           "CLK=%d DT=%d SW=%d | STAT=%d VBUS=%d | pos=%ld sub=%d edges=%lu bad=%lu",
           digitalRead(board::PIN_ENC_CLK), digitalRead(board::PIN_ENC_DT),
           digitalRead(board::PIN_ENC_SW), digitalRead(board::PIN_CHARGE_STAT),
           digitalRead(board::PIN_VBUS_SENSE), (long)position_, (int)encSub_,
           (unsigned long)encEdges_, (unsigned long)encInvalid_);
}

void dumpState() {
  char line[160];
  traceLine(line, sizeof(line));
  Serial.println("\n--- panel controls ---");
  Serial.printf("  raw      %s\n", line);
  Serial.printf("  encoder  position %ld detents, %lu valid edges, %lu rejected\n",
                (long)position_, (unsigned long)encEdges_,
                (unsigned long)encInvalid_);
  Serial.printf("  switch   %s\n", sw_.level ? "DOWN" : "up");
  Serial.printf("  charging %s%s\n", stat_.level ? "YES" : "no",
                board::CHARGER_STATUS_READABLE ? "" : "   (mod not declared fitted)");
  Serial.printf("  external %s\n", vbus_.level ? "5 V PRESENT" : "on battery");
  Serial.printf("  LED      %s, %s\n", ledOn_ ? "lit" : "dark", ledModeName());

  // THE TWO FAILURES THAT LOOK THE SAME FROM THE POSITION FIGURE ALONE, named
  // so the console answers the question rather than just posing it.
  if (encEdges_ == 0) {
    Serial.println("  !! no quadrature edges at all -- CLK/DT are not moving.");
    Serial.println("     Check the encoder's ground at P1-13 and that CLK/DT");
    Serial.println("     are on P1-9 and P1-8. If the load cells have ALSO gone");
    Serial.println("     quiet, a wire is on P1-7 -- that is CELL_SDA.");
  } else if (encInvalid_ > encEdges_ / 4) {
    Serial.println("  !! a quarter of transitions rejected -- bouncing badly.");
    Serial.println("     A bare EC11 on internal pull-ups alone can do this;");
    Serial.println("     10k external pull-ups, or 100nF to GND on CLK and DT,");
    Serial.println("     is the fix. Position stays correct meanwhile: rejected");
    Serial.println("     transitions are discarded, not counted backwards.");
  }
}

}  // namespace inputs
