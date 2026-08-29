# Cutting boot time on an ESP32-S3 + LVGL + LovyanGFX touch device

Written after taking this project's boot from **9.9 s to 2.1 s**. It is
deliberately written to be **portable to any project on the same stack** — a
Waveshare-style ESP32-S3 panel, LovyanGFX for the display and touch, LVGL for
the UI — so it names the causes and the method rather than this repo's files.

Copy it into the next project. The numbers in it are real measurements from a
240×320 ST7789 panel on an ESP32-S3R8 at 240 MHz.

---

## The one rule

**Measure first. Every single time.**

Boot lag has a small number of causes and they are not the ones that look
expensive. On this project the two things a reasonable engineer would suspect —
PNG decoding and I²C bus speed — were together worth under 50 ms out of 9.9 s,
and I wasted a change on one of them by inferring instead of measuring. The four
things that actually cost the time were all invisible from the outside.

A boot log that names its phases but does not time them cannot answer "why is
this slow", and that is the state most firmware ships in.

---

## Step 1 — make the boot self-measuring

Two mechanisms, both about ten minutes of work. Do both before changing anything.

### Host-side timestamps

In `platformio.ini`, for the environment you flash:

```ini
monitor_filters = time
```

Every console line now arrives with a wall-clock stamp. This alone localises the
problem to a phase.

### Firmware-side phase marks

Timestamps on the host tell you when a line was *printed*, which is not quite
when work happened, and they cannot see inside a phase. Add this:

```c
uint32_t bootPhaseMs_ = 0;
uint32_t bootStartMs_ = 0;

void bootMark(const char *what) {
  const uint32_t now = millis();
  Serial.printf("  [+%4lu ms, %5lu total] %s\n",
                (unsigned long)(now - bootPhaseMs_),
                (unsigned long)(now - bootStartMs_), what);
  bootPhaseMs_ = millis();
}

void setup() {
  bootStartMs_ = bootPhaseMs_ = millis();
  ...
}
```

Both figures matter and neither substitutes for the other: **the delta is what
you optimise, the total is what the person watching the splash experiences.**

Then put a `bootMark()` after each phase — console wait, display init, backlight
ramp, splash, sensors, LVGL init, page construction. Keep them in. They cost
nothing and the question recurs.

### Measuring inside LVGL page construction

`build*()` functions usually live in UI code that must not include board headers
(so a desktop simulator can compile it). `LV_LOG_WARN` works there, prints at the
default `LV_LOG_LEVEL_WARN`, carries its own millisecond timestamp, and reaches
your console through the callback you registered with
`lv_log_register_print_cb()`. Drop marks between page builders, measure, remove.

---

## Step 2 — the four usual culprits, in the order they usually rank

### 1. Blocking peripheral bring-up in `setup()`

**Measured here: 4766 ms.** Three ADCs, each doing a reset, a power-up-ready
wait and an internal offset calibration.

Ask: *does anything in `setup()` actually need this finished?* Usually nothing
does. The UI reads sensor state through a snapshot, and the snapshot has (or
should have) a vocabulary for "has not concluded yet".

Move the bring-up into the task that owns the peripheral. It stays a blocking
sequence — just on the other side of the task boundary, so the rule that one
task owns a subsystem still holds from the first register write. It then runs
*concurrently* with page construction instead of before it.

> **The trap.** A zero-initialised snapshot puts every sensor in whatever your
> enum's value 0 is — usually `Offline`. Extending the pre-first-publish window
> from milliseconds to a second means the dashboard now renders a red fault flag
> on **every boot**. Seed the published snapshot with `Warming` (or your
> equivalent) in the init function. If you have already probed the bus and know
> the device answers, `Warming` is the accurate word; `Offline` is a false claim.

**Cost after: 22 ms on the boot path.**

### 2. Eager LVGL page construction

**Measured here: 3268 ms, of which 2136 ms was screens nobody had opened.**

| screen | build cost |
| --- | --- |
| WiFi (passphrase keyboard) | **1343 ms** |
| numeric keypad | 343 ms |
| battery detail | 230 ms |
| scope / chart | 134 ms |
| diagnostics | 86 ms |

LVGL widget construction is far more expensive than it looks — flex layouts
recalculate as children are added, so a keyboard's ~40 buttons is not 40 units of
work. **A page with a keyboard or keypad is the one to check first.**

Build the **container** eagerly (your show/hide logic needs something to hide);
build the **contents** on first open:

```c
bool built_[N] = {false};

void ensureBuilt(lv_obj_t *page) {
  if (page == detailWifi_ && !built_[1]) {
    built_[1] = true;
    buildWifiPage(detailWifi_);
    wifiOnClose(back);
  } else if (...) { ... }
}

void push(lv_obj_t *page) {
  if (!page) return;
  ensureBuilt(page);      // BEFORE any show/prefill logic that reaches inside
  showOnly(page);
}
```

The cost does not vanish — it moves to the first tap on that settings row, which
can afford a second. A boot cannot, and the person paying at boot is usually not
the person who wanted the page.

> **The trap that bit us.** `lv_tileview_set_tile()` is a *scroll*: it moves
> content to the tile's coordinates. **Until a layout pass has run, every tile is
> still at 0,0**, so it scrolls to the first tile. Our device started booting to
> the menu instead of the home screen.
>
> It was latent, not new — it used to work because pages built *after* that line
> generated enough layout traffic to lay the tileview out in time. Removing that
> traffic removed the accident.
>
> **Fix: `lv_obj_update_layout(scr);` immediately before the tile selection.**
> State the dependency rather than restore the accident. Expect the forced layout
> to show up as ~300 ms that was previously hidden — that is correctness you were
> already paying for, now visible.

### 3. Deliberate holds — `delay()` in the splash

**Measured here: 1200 ms.** A hard-coded `delay(1200)` so the splash stayed up
long enough to read.

That was a real need when it was written and stopped being one as the rest of
boot grew: the splash was already on screen for the whole of sensor bring-up and
page construction, because nothing else draws until the UI is built. **The hold
was not making the splash visible, it was adding to a period the splash already
filled.**

Grep your boot path for `delay(`. Judge each one against what the *rest* of boot
now costs, not against what it cost when the line was written.

### 4. The USB-CDC console wait

**Measured here: up to 2000 ms.**

```c
const uint32_t deadline = millis() + 2000;
while (!Serial && (int32_t)(millis() - deadline) < 0) delay(10);
```

Necessary on a board where the console is native USB — without it the first boot
lines are lost. But on a unit running on battery with no host attached it is
**always paid in full**, to preserve lines nobody will read.

Make it a build flag and lower the default:

```c
#ifndef PROJECT_CONSOLE_WAIT_MS
#define PROJECT_CONSOLE_WAIT_MS 400
#endif
```

400 ms keeps the useful case — a host already attached when the board resets,
which is what `pio run -t upload -t monitor` produces. Raise it if early lines go
missing.

---

## Step 3 — the sleeper: constants that went stale when a rate changed

Worth its own section because it is the hardest to find and it is not really a
performance bug.

Our converter self-test asked for 16 samples with a flat **1500 ms** timeout.
Correct when the firmware ran at 80 SPS — 18 conversions is 225 ms. The sample
rate was later dropped to **10 SPS** deliberately (the rate *is* the
anti-aliasing filter), which made 18 conversions 1800 ms. **Every call then ran
to its timeout and returned a truncated sample set, silently.** Nine timeouts of
1.5 s is 13.5 s of boot spent waiting for samples that were never going to
arrive.

It survived for months because **the function was never called**. A stale
constant inside dead code is invisible until the day the code runs.

Two lessons:

- **Derive timeouts from the rate, never write them down.**
  ```c
  // +2 covers the conversion already in flight and the one discarded after a
  // configuration change.
  uint32_t conversionsMs(uint8_t n) {
    return (uint32_t)(n + 2) * 1000u / RATE_HZ + 250u;
  }
  ```
- **When you enable code that has never run, budget time to re-audit its
  constants.** Search the same file for other numbers justified against the old
  rate. We found a second one immediately: a `delay(60)` commented "two
  conversion periods at 80 SPS", which at 10 SPS is less than *one* period — a
  correctness bug, not a slow one, because the measurement could include a
  conversion taken before the configuration change landed.

---

## Step 4 — expensive diagnostics belong on a key, not on every boot

A boot-time self-test that proves something true on every boot but the one where
hardware has just been rewired is not worth 3.5 s of every power-on.

Do not delete it — **move it to a console key** and print one line at boot saying
it exists:

```
self-test skipped at boot -- press 's' on the console to run it
```

This is strictly better than the build flag it replaces: the moment you need the
diagnostic is when hardware has gone quiet in the field, and reflashing is the
last thing you want to do then. On a key it can also run *deeper* than boot ever
did, because somebody is watching.

> Route it through the task that owns the peripheral (raise a flag, service it in
> the task loop) rather than calling it from the console handler. Diagnostics
> that reconfigure a device — shorting PGA inputs, re-running a calibration —
> racing the task that polls it produces a wrong reading, not a crash.

---

## What is almost never the problem

Check these off with numbers so you stop suspecting them.

| suspect | measured | verdict |
| --- | --- | --- |
| Touch controller / panel init | **264 ms** for `gfx.init()` + clear | never a candidate |
| I²C bus speed | full 0x08–0x77 scan of **two** ports: **17 ms** | irrelevant |
| PNG decode of a 128×128 splash logo | whole splash — clear, decode, 3 lines of text — **44 ms** | looks slow, isn't |
| Backlight fade-in | 41 steps × 4 ms = **164 ms** | real but small; shorten only at the end |
| LVGL init + draw buffer alloc | **4 ms** | free |

**On I²C specifically:** if your sensor bring-up is slow, the cost is almost
certainly *conversion* time, not bus time. At 10 SPS every sample is 100 ms of
the ADC integrating. Raising the bus from 400 kHz to 1 MHz shaves microseconds
off a phase measured in seconds. And a low sample rate is usually a deliberate
choice — decimation-filter nulls land on multiples of the output rate, which is
how 10 SPS rejects 50/60 Hz mains outright. Do not trade that away for boot time.

**On the splash logo:** we converted a PNG to a raw RGB565 array on the theory
that decoding was the cost. It wasn't — a `delay()` on the next line was. That
change bought ~5 ms for 23 KB of flash and was reverted. Measure before you
optimise something that merely *reads* as expensive.

---

## Worked example — the full before and after

| phase | before | after |
| --- | ---: | ---: |
| USB-CDC console wait | ≤2000 | ~100 |
| `gfx.init()` + clear | 264 | 264 |
| backlight ramp | 164 | 164 |
| splash draw + `delay(1200)` hold | 1249 | **49** |
| sensor bring-up, blocking `setup()` | **4766** | **22** |
| LVGL init + draw buffers | 4 | 8 |
| network + time tasks started | 53 | 52 |
| `buildPages()` | **3268** | 1461 |
| **total** | **9882 ms** | **2123 ms** |

`buildPages()` fell by less than the 2.1 s of deferred screens, because two
things moved *into* it: the forced layout pass that fixed the tileview bug, and
the sensor bring-up now running concurrently on a higher-priority task pinned to
the same core. That is the trade working as intended — the work overlaps instead
of serialising.

---

## Checklist

For a new project on this stack, in order:

- [ ] `monitor_filters = time` in `platformio.ini`
- [ ] `bootMark()` after every phase in `setup()`
- [ ] Flash, read the numbers, **rank the phases** — do not start fixing yet
- [ ] Grep the boot path for `delay(` and justify each against today's boot
- [ ] Bound or lower the USB-CDC console wait; make it a build flag
- [ ] Move peripheral bring-up onto its owning task; seed the published snapshot
      with `Warming`, not `Offline`
- [ ] Build detail/settings screens on first open; keep the container eager
- [ ] `lv_obj_update_layout()` before any `lv_tileview_set_tile()` /
      `lv_obj_scroll_to()` during construction
- [ ] Move expensive self-tests to a console key; print one line saying so
- [ ] If a sample rate ever changed, audit every timeout and `delay()` derived
      from the old one
- [ ] Re-measure. Keep the marks in the shipping image.

---

## Two habits worth carrying

**A boot log that names phases but does not time them is half a diagnostic.**
Keeping `bootMark()` in the shipping image costs a handful of `printf`s and means
the next person never has to reconstruct timings from host-side stamps.

**"It works" and "it works for the reason I think" are different claims.** The
tileview bug had been passing for months on layout traffic that had nothing to do
with it. Removing unrelated work exposed it instantly. When a change breaks
something that looks unconnected, suspect that the old behaviour was an accident
— and fix it by stating the dependency, not by restoring the accident.
