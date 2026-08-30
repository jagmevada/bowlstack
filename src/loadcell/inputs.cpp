#include "inputs.h"

#include <Preferences.h>

#include "board_waveshare_s3.h"

namespace inputs {
namespace {

// --- quadrature -------------------------------------------------------------
// A FULL-STEP STATE MACHINE, because counting transitions loses slow detents.
//
// THE BUG THIS REPLACES, recorded because the symptom pointed away from the
// cause. The first version accumulated +/-1 per valid transition and emitted a
// detent every fourth one. That is correct arithmetic and it failed in the
// field: turning SLOWLY produced detents that did not register, while turning
// FAST worked perfectly.
//
// The asymmetry is the clue, and the mechanism is worse than "a detent got
// dropped". The sub-count PERSISTS between detents, so one lost edge does not
// cost one click -- it leaves the counter permanently off-phase, and from then
// on a single detent swings it between +2 and -2 without ever crossing the +/-4
// threshold. Simulated over a dirty detent followed by eight clean
// back-and-forth clicks, the accumulator reports ONE of the eight. The state
// machine reports all eight.
//
// Turning fast crosses several detents in one gesture, so 4N transitions
// arrive, the total clears the threshold anyway, and the fault vanishes -- it
// hides in precisely the slow, deliberate movement someone makes when they are
// checking whether it works.
//
// THE FIX IS TO STOP COUNTING AND START RECOGNISING. This is Ben Buxton's
// full-step table: seven states describing the two legal routes from one detent
// to the next, with a step emitted only on ARRIVAL at rest by a complete valid
// route. Bounce anywhere in the middle drops the machine back to R_START and it
// waits for a clean run, which costs nothing -- the shaft has not moved between
// detents anyway.
//
// So a detent is emitted when the encoder reaches one, not when four edges have
// been counted, and losing an edge mid-sequence costs a retry rather than a
// click.
const uint8_t R_START     = 0x0;
const uint8_t R_CW_FINAL  = 0x1;
const uint8_t R_CW_BEGIN  = 0x2;
const uint8_t R_CW_NEXT   = 0x3;
const uint8_t R_CCW_BEGIN = 0x4;
const uint8_t R_CCW_FINAL = 0x5;
const uint8_t R_CCW_NEXT  = 0x6;

const uint8_t DIR_CW     = 0x10;
const uint8_t DIR_CCW    = 0x20;
const uint8_t DIR_MASK   = 0x30;
const uint8_t STATE_MASK = 0x0F;

// Row = current state, column = the pin pair (CLK << 1) | DT. R_START is both
// contacts open, which is where a detented EC11 rests -- and matches the idle
// CLK=1 DT=1 the boot trace reports on this board.
const uint8_t QTABLE[7][4] = {
    // R_START
    {R_START,    R_CW_BEGIN,  R_CCW_BEGIN, R_START},
    // R_CW_FINAL   -- one transition short of a completed clockwise detent
    {R_CW_NEXT,  R_START,     R_CW_FINAL,  (uint8_t)(R_START | DIR_CW)},
    // R_CW_BEGIN
    {R_CW_NEXT,  R_CW_BEGIN,  R_START,     R_START},
    // R_CW_NEXT
    {R_CW_NEXT,  R_CW_BEGIN,  R_CW_FINAL,  R_START},
    // R_CCW_BEGIN
    {R_CCW_NEXT, R_START,     R_CCW_BEGIN, R_START},
    // R_CCW_FINAL  -- one transition short of a completed anticlockwise detent
    {R_CCW_NEXT, R_CCW_FINAL, R_START,     (uint8_t)(R_START | DIR_CCW)},
    // R_CCW_NEXT
    {R_CCW_NEXT, R_CCW_FINAL, R_CCW_BEGIN, R_START},
};

// IF ONE CLICK EVER REPORTS AS TWO, the encoder is half-step and this is the
// wrong table -- Buxton publishes a second one that emits at the midpoint as
// well. That is a table swap, not a threshold tweak, which is the other reason
// the accumulator had to go: setting its STEPS_PER_DETENT to 2 looked like it
// would handle a half-step part and would instead have doubled the slow-turn
// losses.

// Set true if CW turns report negative. Which pin is "A" is a property of how
// the encoder was wired, not of the encoder, so this is a wiring constant and
// not a preference -- swapping two Dupont wires does the same job.
const bool INVERT_DIRECTION = false;

// ISR state. Touched from both an interrupt and the main loop, so every access
// is inside the spinlock -- `volatile` alone would not make the accumulator's
// read-and-clear atomic against an interrupt landing between the two halves.
portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t encState_ = R_START;
volatile uint8_t encPins_ = 3;
volatile int16_t encPending_ = 0;
volatile uint32_t encEdges_ = 0;
volatile uint32_t encAborted_ = 0;

int32_t position_ = 0;

// --- the stored position ----------------------------------------------------
// A press writes the knob's current position to NVS and begin() restores it, so
// the encoder is a SETTING that survives a power cycle rather than a counter
// that resets whenever somebody unplugs the unit.
//
// ITS OWN NAMESPACE, following what the rest of the firmware already does: WiFi
// keeps credentials in "bowlstack", the cells keep calibration in "bowlscale".
// Each subsystem owns one, so clearing a feature's storage cannot disturb
// another's and a key name cannot collide across two features that know nothing
// about each other.
//
// A PRIVATE Preferences OBJECT, not a shared one. scale.cpp records why: the
// object is not reentrant and it is opened from a different task there. NVS
// itself is safe across distinct handles, so a private handle opened and closed
// around each access is the shape that cannot go wrong.
const char *NVS_NS = "bowlinput";
const char *KEY_ENCPOS = "encpos";
Preferences prefs_;

// What is actually on flash, so a press that changes nothing writes nothing.
// NVS is flash with a finite endurance, and the switch is a button somebody may
// well press twice to be sure -- storing the same number again would spend a
// write cycle recording that nothing happened.
int32_t savedPos_ = 0;
bool haveSaved_ = false;

// --- TRIAL HARNESS: the manual fill estimate --------------------------------
// See inputs.h. One point of fill per detent, clamped hard at both ends.
//
// CLAMPED AS STATE, NOT AS DISPLAY, and that distinction is the whole feel of
// the control. If the raw count were allowed to run past 100 and only the
// rendering clamped, then overshooting by twelve clicks would need twelve
// clicks back before the number moved at all -- and the attendant would decide
// the knob was broken. Clamping the stored value means the very next click the
// other way is worth one point.
const char *KEY_FILLPCT = "fillpct";
uint8_t fillPct_ = 0;
uint8_t savedFillPct_ = 0;
bool fillDirty_ = false;
uint32_t fillChangedAtMs_ = 0;

// Written 2 s after the knob STOPS, not on every detent. At one point per click
// a sweep from empty to full is a hundred writes; NVS endurance is finite and
// the intermediate values are not estimates anybody made, they are the knob
// passing through. The settle window turns a sweep into one write.
const uint32_t FILL_SETTLE_MS = 2000;

// How long the attendant is left alone before the LED starts asking. The
// reminder is the experiment's clock: an estimate nobody refreshes is what a
// knob-based system actually degrades into, so the interval is part of what is
// being measured rather than a convenience.
const uint32_t FILL_REMIND_MS = 600000;  // 10 minutes
uint32_t fillUpdatedAtMs_ = 0;

void storeFill() {
  if (fillPct_ == savedFillPct_) { fillDirty_ = false; return; }
  if (!prefs_.begin(NVS_NS, false)) {
    // println, not printf -- so a literal percent sign, not an escaped one.
    Serial.println("input: NVS unavailable -- fill % will not persist");
    fillDirty_ = false;
    return;
  }
  prefs_.putUChar(KEY_FILLPCT, fillPct_);
  prefs_.end();
  savedFillPct_ = fillPct_;
  fillDirty_ = false;
  Serial.printf("input: fill %u%% stored\n", (unsigned)fillPct_);
}

void storePosition() {
  if (haveSaved_ && position_ == savedPos_) {
    Serial.printf("input: position %ld already stored\n", (long)position_);
    return;
  }
  if (!prefs_.begin(NVS_NS, false)) {
    Serial.println("input: NVS unavailable -- knob position will not persist");
    return;
  }
  prefs_.putInt(KEY_ENCPOS, position_);
  prefs_.end();
  savedPos_ = position_;
  haveSaved_ = true;
  Serial.printf("input: position %ld stored -- next boot starts here\n",
                (long)position_);
}

void IRAM_ATTR encIsr() {
  // digitalRead is IRAM_ATTR in this core, so it is safe from an ISR. Both pins
  // are read here rather than one being inferred: inferring the other is the
  // same mistake as counting edges, one layer down.
  const uint8_t pins = (uint8_t)((digitalRead(board::PIN_ENC_CLK) << 1) |
                                 digitalRead(board::PIN_ENC_DT));

  portENTER_CRITICAL_ISR(&mux_);
  if (pins != encPins_) {
    encPins_ = pins;
    encEdges_++;

    const uint8_t was = (uint8_t)(encState_ & STATE_MASK);
    const uint8_t next = QTABLE[was][pins];
    encState_ = next;

    switch (next & DIR_MASK) {
      case DIR_CW:  encPending_++; break;
      case DIR_CCW: encPending_--; break;
      default:
        // A sequence that started and fell back to rest without completing --
        // bounce, or a hand that changed its mind mid-detent. Counted so the
        // bring-up dump can tell a dirty encoder from a disconnected one, and
        // deliberately NOT treated as movement, which is the entire point.
        if (was != R_START && (next & STATE_MASK) == R_START) encAborted_++;
        break;
    }
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

// --- the indicator, on PWM ---------------------------------------------------
// FOUR STATES A PERSON CAN TELL APART WITHOUT BEING TAUGHT THEM, which is the
// whole reason this pin moved off digitalWrite:
//
//     steady            on battery, running down the cell
//     smooth fade       on mains
//     fast blink 5 Hz   the fill estimate is stale and wants updating
//     dark              switched off
//
// Two square waves at 1 Hz and 5 Hz were distinguishable in principle and not
// in practice -- both read as "the light is flashing", and somebody glancing
// across a counter has to count to tell them apart. A fade is a different KIND
// of signal rather than a different rate, so it separates at a glance, and it
// carries the meaning most phones have already taught everybody: breathing
// means charging.
//
// CHANNEL 0, AND THAT IS NOT ARBITRARY. LovyanGFX drives the backlight on
// channel 7, and arduino-esp32 2.0.17 maps a channel to timer (chan >> 1) & 3 --
// so channel 7 sits on timer 3 and CHANNEL 6 SHARES IT. Calling ledcSetup on 6
// with a different frequency silently re-times the display's brightness; on 7
// it steals it outright. Channel 0 lands on timer 0 and touches neither.
const uint8_t LED_PWM_CH = 0;
const uint32_t LED_PWM_HZ = 5000;  // far above anything an eye or a camera sees
const uint8_t LED_PWM_BITS = 8;
const uint8_t LED_DUTY_MAX = 255;

// Full cycle of the fade, dark to bright to dark. 0.5 Hz.
const uint32_t FADE_PERIOD_MS = 2000;

uint8_t ledDuty_ = 0;

void driveLed(uint8_t duty) {
  ledDuty_ = duty;
  ledOn_ = (duty > 0);
  // The board's LED is active HIGH (board_waveshare_s3.h section 9); inverting
  // here rather than at every call site keeps the polarity in one place, and it
  // is a duty inversion rather than a level one because this is PWM now.
  ledcWrite(LED_PWM_CH,
            board::STATUS_LED_ACTIVE_HIGH ? duty : (LED_DUTY_MAX - duty));
}

// GAMMA, because a linear duty ramp does not look like a linear fade.
//
// Perceived brightness goes roughly as the square root of luminous output, so a
// duty sweeping evenly from 0 to 255 appears to rush through the dim half and
// crawl through the bright one -- it reads as a flicker followed by a plateau,
// which is exactly the "blink" this state exists to NOT look like. Squaring the
// ramp (gamma 2.0) inverts that and gives an even-looking breath. It is one
// multiply, against the cube-root-accurate 2.2 that would need a table.
uint8_t fadeDuty(uint32_t nowMs) {
  const uint32_t ph = nowMs % FADE_PERIOD_MS;
  const uint32_t half = FADE_PERIOD_MS / 2;
  // Triangle: up over the first half, down over the second.
  const uint32_t tri = (ph < half) ? (ph * LED_DUTY_MAX / half)
                                   : ((FADE_PERIOD_MS - ph) * LED_DUTY_MAX / half);
  return (uint8_t)((tri * tri) / LED_DUTY_MAX);
}

// FREE-RUNNING off millis() rather than a toggle with its own timer. A stored
// phase would have to be reset whenever the mode changed, and getting that
// wrong gives a blink that stalls on one edge -- which reads as a failed LED.
// Derived from the clock, it cannot stall; it only glitches once per millis()
// wrap, every 49.7 days, by at most one half-period.
void applyLedPolicy(uint32_t nowMs) {
  switch (ledMode_) {
    case LedMode::ForceOn:  driveLed(LED_DUTY_MAX); return;
    case LedMode::ForceOff: driveLed(0);             return;
    case LedMode::Auto:
    default:
      // TRIAL: THE REMINDER OUTRANKS BOTH POWER STATES, because it is the only
      // one addressed to a person who is expected to do something. 5 Hz is
      // deliberately faster than anything else this LED does -- the 1 Hz mains
      // blink and the steady battery light are ambient status, and an alert
      // that ticked at a similar rate would be read as more of the same.
      // THE REMINDER OUTRANKS BOTH POWER STATES, because it is the only one
      // addressed to a person who is expected to do something. A hard 5 Hz
      // square -- no fade -- so it reads as an alarm rather than as more
      // ambient status.
      if (fillReminderDue()) {
        driveLed(((nowMs / 100u) & 1u) == 0u ? LED_DUTY_MAX : 0);
        return;
      }
      // On mains it breathes; on battery it is steady. The steady state is the
      // one that costs current, and it is deliberately the battery one: a light
      // left on a device that looks switched off is the message.
      driveLed(vbus_.level ? fadeDuty(nowMs) : LED_DUTY_MAX);
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

  ledcSetup(LED_PWM_CH, LED_PWM_HZ, LED_PWM_BITS);
  ledcAttachPin(board::PIN_STATUS_LED, LED_PWM_CH);
  driveLed(0);

  // Seed from the pins as they are RIGHT NOW, so the first edge is judged
  // against where the shaft actually sits rather than an assumption. If the
  // knob happens to rest between detents at boot the machine simply waits at
  // R_START for a clean sequence -- one click may be needed to synchronise, and
  // that is honest behaviour rather than a fabricated first step.
  encPins_ = (uint8_t)((digitalRead(board::PIN_ENC_CLK) << 1) |
                       digitalRead(board::PIN_ENC_DT));
  encState_ = R_START;

  attachInterrupt(digitalPinToInterrupt(board::PIN_ENC_CLK), encIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(board::PIN_ENC_DT), encIsr, CHANGE);

  // Restored before the first loop() runs, so nothing ever renders the zero
  // this started at.
  //
  // Opened READ-WRITE even though only a read happens here. A namespace that
  // has never been written does not exist, and opening it read-only fails and
  // logs an error -- which would make the first boot after flashing look
  // identical to a corrupt store. Opening read-write creates it, and no flash
  // is written unless something is put. bringup_wifi.cpp learned this the same
  // way.
  if (prefs_.begin(NVS_NS, false)) {
    position_ = prefs_.getInt(KEY_ENCPOS, 0);
    prefs_.end();
    savedPos_ = position_;
    haveSaved_ = true;
    // TRIAL: the fill estimate survives a power cycle too. A station rebooted
    // mid-service must not come back claiming an empty vessel -- that is a
    // reading somebody would act on.
    fillPct_ = prefs_.getUChar(KEY_FILLPCT, 0);
    if (fillPct_ > 100) fillPct_ = 100;
    savedFillPct_ = fillPct_;
  } else {
    Serial.println("input: NVS unavailable -- knob starts at 0, will not persist");
  }

  Serial.println("\n--- panel controls ---");
  Serial.printf("  encoder  CLK GPIO%d (P1-8)  DT GPIO%d (P1-9)  SW GPIO%d (P1-6)\n",
                board::PIN_ENC_CLK, board::PIN_ENC_DT, board::PIN_ENC_SW);
  Serial.println("           the cell bus moved to P2-9/P2-10, so P1 is all panel now");
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
  Serial.printf("  TRIAL    manual fill %u%%, restored from NVS. 1%% per detent,\n"
                "           stored 2 s after the knob stops, and the LED blinks\n"
                "           at 5 Hz once the estimate is 10 min old.\n",
                (unsigned)fillPct_);
  Serial.printf("  position %ld restored from NVS -- a press stores the "
                "current value\n", (long)position_);
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

    // --- TRIAL HARNESS ------------------------------------------------------
    // Saturating rather than wrapping. A vessel cannot be 105% full, and
    // wrapping to 0 at the top would turn one click of overshoot into a report
    // that the food had run out.
    const int32_t want = (int32_t)fillPct_ + e.turned;
    const uint8_t next = (uint8_t)(want < 0 ? 0 : (want > 100 ? 100 : want));
    if (next != fillPct_) {
      fillPct_ = next;
      fillDirty_ = true;
      fillChangedAtMs_ = nowMs;
      // The reminder clock restarts on any movement, including one that lands
      // back where it started -- the attendant has just looked at the vessel,
      // which is the thing the reminder exists to provoke.
    }
    fillUpdatedAtMs_ = nowMs;
  }

  // Commit once the knob has been still for a moment. Deliberately not on the
  // press: pressing is a separate gesture the attendant has no reason to learn,
  // and an estimate that only persists if you remember a second action is an
  // estimate that will be lost.
  if (fillDirty_ && (uint32_t)(nowMs - fillChangedAtMs_) >= FILL_SETTLE_MS) {
    storeFill();
  }

  const bool swRaw = (digitalRead(board::PIN_ENC_SW) == LOW);  // pull-up: LOW = pressed
  if (settle(sw_, swRaw, nowMs, SWITCH_DWELL_MS)) {
    if (sw_.level) {
      e.pressed = true;
      swDownAtMs_ = nowMs;
      longFired_ = false;
      // ON THE PRESS, not the release, because the press is the moment the
      // operator feels as committing. It costs an NVS commit -- a few
      // milliseconds inside the render loop -- which is one dropped frame at
      // worst, and only on a deliberate button push.
      storePosition();
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
int32_t storedPosition() { return savedPos_; }

uint8_t fillPercent() { return fillPct_; }
uint32_t fillAgeMs() { return millis() - fillUpdatedAtMs_; }
bool fillReminderDue() { return fillAgeMs() >= FILL_REMIND_MS; }
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
    default:
      return "auto -- steady on battery, fading on mains, 5 Hz when stale";
  }
}
bool statusLed() { return ledOn_; }

uint32_t transitions() { return encEdges_; }
uint32_t invalidTransitions() { return encAborted_; }
int8_t subPosition() { return (int8_t)(encState_ & STATE_MASK); }

void traceLine(char *out, size_t n) {
  // RAW levels, deliberately bypassing every filter above. This is the line
  // that says whether a wire is on the pin at all, and a debounced view would
  // hide exactly the fault it is meant to find.
  snprintf(out, n,
           "CLK=%d DT=%d SW=%d | STAT=%d VBUS=%d | pos=%ld st=%d edges=%lu part=%lu",
           digitalRead(board::PIN_ENC_CLK), digitalRead(board::PIN_ENC_DT),
           digitalRead(board::PIN_ENC_SW), digitalRead(board::PIN_CHARGE_STAT),
           digitalRead(board::PIN_VBUS_SENSE), (long)position_, (int)(encState_ & STATE_MASK),
           (unsigned long)encEdges_, (unsigned long)encAborted_);
}

void dumpState() {
  char line[160];
  traceLine(line, sizeof(line));
  Serial.println("\n--- panel controls ---");
  Serial.printf("  raw      %s\n", line);
  Serial.printf("  encoder  position %ld detents, %lu edges, %lu partial sequences abandoned\n",
                (long)position_, (unsigned long)encEdges_,
                (unsigned long)encAborted_);
  Serial.printf("  stored   %ld%s\n", (long)savedPos_,
                position_ == savedPos_
                    ? " (matches current)"
                    : "  <- press the knob to store the current value");
  Serial.printf("  fill     %u%%  (%lu s since last update%s)\n",
                (unsigned)fillPct_,
                (unsigned long)(fillAgeMs() / 1000),
                fillReminderDue() ? ", REMINDER DUE" : "");
  Serial.printf("  switch   %s\n", sw_.level ? "DOWN" : "up");
  Serial.printf("  charging %s%s\n", stat_.level ? "YES" : "no",
                board::CHARGER_STATUS_READABLE ? "" : "   (mod not declared fitted)");
  Serial.printf("  external %s\n", vbus_.level ? "5 V PRESENT" : "on battery");
  Serial.printf("  LED      duty %u/255, %s\n", (unsigned)ledDuty_, ledModeName());

  // THE TWO FAILURES THAT LOOK THE SAME FROM THE POSITION FIGURE ALONE, named
  // so the console answers the question rather than just posing it.
  if (encEdges_ == 0) {
    Serial.println("  !! no quadrature edges at all -- CLK/DT are not moving.");
    Serial.println("     Check the encoder's ground at P1-13 and that CLK/DT");
    Serial.println("     are on P1-9 and P1-8. The cell bus is on P2 now, so a");
    Serial.println("     panel wire can no longer take the load cells down with it.");
  } else if (encAborted_ > encEdges_ / 2) {
    Serial.println("  !! most sequences abandoned before completing.");
    Serial.println("     The state machine discards these rather than guessing,");
    Serial.println("     so POSITION stays right and clicks are merely missed.");
    Serial.println("     100nF from CLK and DT to GND is the fix if it is bad");
    Serial.println("     enough to feel.");
  }
}

}  // namespace inputs
