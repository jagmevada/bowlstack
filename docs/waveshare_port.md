# Waveshare touch port

Moving Bowlstack from the discrete ESP32 board (four sensors, five LEDs, a
hand-built divider) onto a **Waveshare ESP32-S3-Touch-LCD-2**, where the local
UI becomes a 2" touchscreen.

For the measurement behaviour, which does not change at all, see
[sensor_logic.md](sensor_logic.md). For the module layout this plugs into see
[firmware.md](firmware.md).

Every pin number here was read from the manufacturer's schematic netlist
(`ESP32-S3-Touch-LCD-2-SchDoc.pdf`) and is recorded with its evidence in
[include/board_waveshare_s3.h](../include/board_waveshare_s3.h). Nothing came
from a pinout image or a forum post.

---

## 1. What the board already spends

| Function | GPIO | Note |
| --- | --- | --- |
| Octal PSRAM (8 MB, in package) | 33–37 | permanently gone |
| Quad flash (16 MB) | 26–32 | permanently gone |
| LCD — SCLK / MOSI / MISO / DC / CS | 39 / 38 / 40 / 42 / 45 | ST7789T3, SPI |
| TF card — SCLK / MOSI / MISO / CS | 39 / 38 / 40 / 41 | **shares the LCD bus** |
| Backlight | 1 | active **high**, via SS8050; PWM-able |
| Touch + IMU I2C — SDA / SCL | 48 / 47 | CST816D `0x15`, QMI8658 |
| Touch INT | 46 | |
| Battery ADC | 5 | ADC1_CH4, **divider already fitted** |
| USB D− / D+ | 19 / 20 | |
| UART0 console | 43 / 44 | freed by USB-CDC |
| BOOT key | 0 | |

### Three findings that change the plan

**The battery divider already exists.** `R19` 200 k from `VBAT` to `GPIO5`,
`R20` 100 k to ground, `C31` 100 nF across it — ratio **3.0**, not the discrete
build's 2.0. Adding a second divider in parallel would load this one and skew
every reading. What changes is one constant, `BATTERY_DIVIDER`; the measured
SoC curve in `battery_soc.h` is untouched, because a 4.2 V cell presents 1.40 V
at the pin and a 2.75 V cell presents 0.92 V — the whole span still sits inside
the ADC's accurate window.

> **But calibrate per unit, more carefully than before.** Source impedance is
> 200 k ‖ 100 k = **66.7 kΩ**, against the ~10 kΩ the SAR ADC wants. `C31`
> saves the *dynamic* case — it is a charge reservoir four orders of magnitude
> larger than the sampling capacitor, so an individual conversion barely
> droops. What it cannot fix is the static offset from ADC input leakage across
> 66.7 kΩ, which does not average away and differs per chip. The 16-sample mean
> is still worth taking; `BOWLSTACK_BATTERY_CAL` matters *more* here, not less.

**Charge state is not readable.** The ETA6098's `STAT` output drives the
charge LED's cathode directly. That net has exactly two nodes — charger and LED
— and no module pin on it. The honest firmware behaviour is to publish
`charging` as **unknown**, not `false`: the codebase already refuses to state
what it cannot measure, and a hard `false` is a claim rather than an absence.

> If it turns out to be worth having, the mod is one resistor and the config
> already anticipates its polarity. `STAT` is open-drain and pulls **low** while
> charging — which is exactly the TP4056-style sense that
> `CHARGING_ACTIVE_LOW = true` was written for. Note this **inverts** the
> discrete board's convention, where the pin is high while charging.

**Neither the panel nor the touch chip has a reset line.** Their `RESET` pins
share one node held up by `R14`; `R16`, the 0 Ω link to GPIO0, is unpopulated.
So both drivers must be given `-1` — handing them GPIO0 would be actively
harmful, it is the BOOT strap. The practical consequence: a wedged touch
controller cannot be recovered without a power cycle. If that ever shows up in
the field, populating `R16` is the fix, at the cost of GPIO0.

---

## 2. The constraint that reshapes the sensor wiring

**The ESP32-S3 has two I2C controllers. This design wants three buses.**

The touch controller and IMU occupy one port on GPIO 47/48. That leaves exactly
**one** hardware I2C port for sensors — but the discrete design deliberately
splits four sensors across *two* buses, "so a slave that locks one bus cannot
take down all four".

That split cannot be reproduced here in hardware. Three ways out:

| Option | Verdict |
| --- | --- |
| All four sensors on the one free port | works, but drops the blast-radius protection entirely |
| Bit-bang a second bus in software | adds a dependency and jitter to buy back a weaker version of what the mux gives |
| **TCA9548A mux on the one free port** | **recommended** |

The mux is the right answer, and not as a workaround — **it is the design this
project already planned.** `sensor_logic.md` §6 specifies a TCA9548A for
dual-sensor redundancy, and §"The code seam" already describes the exact hook
it needs: a `select(sensorIndex)` call before each transaction group, a no-op in
direct-bus mode. The S3's port scarcity does not force a compromise; it removes
the reason to keep deferring a change that was going in anyway.

A mux is also *better* than two buses at the job two buses were doing: it
isolates **per sensor**, not per pair, so one locked clone cannot take down even
its neighbour.

> **The mux isolates I2C only.** A sensor on a deselected channel keeps ranging
> — continuous mode runs inside the sensor, not over the bus. Optical exclusion
> must still be driven by `stopContinuous`/`startContinuous`. This is already
> written down in `sensor_logic.md`; it is repeated here because "the mux
> handles it" is the natural and wrong assumption.

**Until the mux is in hand**, wire all four directly to the free port and accept
the reduced protection. The XSHUT addressing walk is unchanged, and a locked bus
is still *detected* — `0xFFFF` from the range register and the
`SENSOR_STALE_MS` timeout both still fire. What is lost is containment, not
visibility.

### Proposed allocation

Headers, as broken out:

```
P1:  IO2  IO4  IO6  IO16 IO17 IO18 IO21 IO8  IO7  IO10 IO20 IO19 GND  5V
P2:  3V3  GND  IO43 IO44 IO47 IO48 IO15 IO13 IO11 IO12 IO14 IO9  GND  VBAT
```

> **P2 positions 7-10 were listed wrongly here** as `IO13 IO12 IO15 IO11`. The
> board is `IO15 IO13 IO11 IO12` — verified against the netlist in
> `ESP32-S3-Touch-LCD-2-SchDoc.pdf` p1 (`PIP207`→IO15, `PIP208`→IO13,
> `PIP209`→IO11, `PIP2010`→IO12). The *set* is unchanged, so no firmware was
> ever affected; only somebody counting header positions with a wire in hand,
> for whom a lead meant for IO13 lands on IO15 and the board still boots.

| Signal | GPIO | Header | Why this pin |
| --- | --- | --- | --- |
| Sensor I2C SDA | 21 | P1-7 | camera SCCB pair — **4.7 kΩ pull-ups already fitted** (`R4`, `R5`) |
| Sensor I2C SCL | 16 | P1-4 | same pair |
| XSHUT `f1` | 2 | P1-1 | |
| XSHUT `f2` | 4 | P1-2 | |
| XSHUT `f3` | 13 | P2-8 | position corrected — this table said P2-7 |
| XSHUT `f4` | 12 | P2-10 | position corrected — this table said P2-8 |
| Sensor 3V3 / GND | — | P2-1 / P2-2 | **P1 has only GND and 5 V** — sensor power must come off P2 |

The camera's SCCB bus is the find worth using: a complete I2C pair, pull-ups
fitted, broken out, and idle unless a camera is plugged into `J1` — which this
project has no use for. That is one bus for free, with no added components.

> **Avoid GPIO17 for anything I2C.** It carries `R6`, a 10 kΩ pull-*down* (it is
> the camera's PWDN line), which an I2C pull-up would have to fight. It is fine
> as a plain output.

> **GPIO19/20 are not really free.** They are native USB. Reclaiming them as
> GPIO means giving up USB-CDC and USB-JTAG — and once GPIO43/44 are freed for
> other use, USB-CDC *is* the console. Treat them as spoken for.

---

## 3. The GUI stack

**LVGL 9.5 for widgets, LovyanGFX 1.2 for the panel and touch.** Both current,
both PlatformIO libraries, no vendored code.

**Why LovyanGFX and not TFT_eSPI.** TFT_eSPI configures itself through
build-time `#define`s in a *library-owned* header, so two boards cannot coexist
in one tree without editing files inside `.pio`. This project already builds two
images from one source tree and is about to build three. LovyanGFX's config is
an ordinary C++ class in an ordinary header —
[include/lgfx_waveshare_s3.h](../include/lgfx_waveshare_s3.h) — which is what
survives that.

**Why LVGL on top rather than drawing directly.** The screen needs more than one
view: stock, health, and a setup page for the captive portal and calibration.
Screens, touch hit-testing and scrolling are exactly what LVGL is for. For a
single static readout it would be overkill; for three touchable views it is less
code, not more.

**LVGL 9 makes the binding small.** Three callbacks, no compile-time tick hook
the way v8 needed:

| Hook | Does |
| --- | --- |
| `lv_display_set_flush_cb` | `setAddrWindow` + `writePixels` into LovyanGFX |
| `lv_indev_set_read_cb` | `gfx.getTouch()` → `lv_indev_data_t` |
| `lv_tick_set_cb(millis)` | set at runtime |

> Cast the flush buffer to `lgfx::rgb565_t`, not `uint16_t*`. That is what tells
> LovyanGFX the source pixel format so it handles byte order itself. Pushed as
> raw `uint16_t` the image renders in convincing but wrong colours — a bug that
> looks like panel misconfiguration and is not.

### Memory

| | |
| --- | --- |
| Draw buffers | 2 × 240×32×2 B = **30 KB**, internal SRAM, DMA-capable |
| LVGL pool | 64 KB, internal SRAM (`LV_MEM_SIZE`) |
| PSRAM | 8 MB, used for **neither** |

Partial rendering, not full-frame: a 240×320×2 framebuffer is 150 KB, and two
would be 300 KB of the S3's 512 KB spent on a screen that changes a digit every
few seconds.

Both buffers stay in internal RAM deliberately. SPI DMA out of PSRAM on the S3
goes through the cache and is measurably slower — the wrong trade for the one
buffer touched every frame. The LVGL pool stays internal for the same reason in
reverse: every allocation LVGL walks during a redraw would otherwise go out over
the octal bus, showing up as a UI that is mysteriously slower under load.

---

## 4. How it joins the task fabric

**The UI takes the LED task's slot, and its rules.** `indicators` compiles out
on this board; a `ui` module replaces it, consuming the same state through the
same seam.

| Task | Core | Prio | Stack | Change |
| --- | --- | --- | --- | --- |
| `sensor` | 1 | 3 | 4 KB | unchanged |
| `net` | 0 | 2 | 8 KB | unchanged |
| `telemetry` | 0 | 2 | 12 KB | unchanged |
| `ui` | **1** | **1** | **12 KB** | replaces `leds` (2.5 KB) |

Core 1 at priority 1 is the same placement `indicatorTask` has, and for the same
reason: **below** `sensorTask`'s priority 3, so it cannot preempt ranging, while
staying off core 0 where WiFi and TLS block for seconds at a time.

`uiTask` reads `tasks::snapshot()` and `tasks::plotFrame()` exactly as
`indicatorTask` does — immutable copies taken under the existing mutex. No new
shared state, no new locks.

> **LVGL is not thread-safe, and the answer is the existing rule, not a new
> one.** `sensorTask` owns `SensorArray` exclusively; `uiTask` owns LVGL
> exclusively. Every `lv_*` call is made from that one task. This is why
> `lv_conf.h` sets `LV_USE_OS LV_OS_NONE` rather than `LV_OS_FREERTOS` —
> `lv_lock()`/`lv_unlock()` would let several tasks draw safely, which is a
> strictly larger thing to get right and buys nothing, because there is no
> second task that wants to.

12 KB is a starting estimate, not a measurement. `tasks::printStackHeadroom()`
already reports high-water marks every 60 s and must be extended to cover `ui`
before the number is trusted — stack exhaustion on ESP32 surfaces as a
corrupt-looking crash far from its cause.

### What the screen shows

The rules are already written, in [FRONTEND_HANDOFF.md](FRONTEND_HANDOFF.md) §4,
and the local UI must not contradict the web one:

- `stack_status = discontiguous` → **do not render a count at all**, render a
  fault. Showing "2 bowls" there is worse than showing an error.
- `stack_status = degraded` → show the number, marked as a lower bound.
- Battery is a **band**, never a percentage. `null` is "no cell", never a
  flat-battery icon.
- `levels[0]` is `f1`, the **bottom** bowl. Drawn as a vertical column, it must
  read the same way up as the physical pipe.

---

## 5. Bring-up

`src/bringup/bringup_display.cpp`, built as its own environment. It links no
part of Bowlstack — no sensors, no WiFi, no telemetry.

```
pio run -e ws-s3-bringup -t upload
```

Six stages, deliberately separated, because bringing a panel and a GUI toolkit
up in one step means a blank screen has a dozen possible causes:

| Stage | Proves | Expected |
| --- | --- | --- |
| 1 | on-board I2C bus | `0x15` CST816D, `0x6A`/`0x6B` QMI8658 |
| 2 | camera SCCB bus usable | nothing acks, **both lines high** |
| 3 | panel, raw LovyanGFX | four colour bars, right way up |
| 4 | LVGL flush path | mock stock view renders |
| 5 | touch → widget | tap counter increments |
| 6 | battery ADC | plausible cell mV vs a multimeter |

Stage 3 runs before LVGL exists on purpose. If the bars are inverted, mirrored
or off-edge, LVGL cannot fix it and adding LVGL only obscures it:

| Symptom | Fix, in `lgfx_waveshare_s3.h` |
| --- | --- |
| black-on-white where white-on-black expected | `cfg.invert` |
| red and blue swapped | `cfg.rgb_order` |
| content shifted or rotated | `setRotation`, `cfg.offset_x/y` |

Stage 2's "both lines high" is the load-bearing observation, not the empty scan.
An empty bus and a bus stuck low look identical to a scan alone, so the harness
reads the idle levels directly first.

Stage 6 is where `BOWLSTACK_BATTERY_CAL` comes from: read the `pin` figure,
measure the cell at the MX1.25 header, and set the flag to `cell / pin`.

> `bus_shared = true` in the panel config is not optional. The LCD and the TF
> card are on one SPI bus with only their chip selects differing; left false, a
> card transaction and a panel flush interleave and paint the screen with
> filesystem bytes.

---

## 6. Build

```
pio run -e ws-s3-bringup -t upload      # board bring-up
pio run -e esp32dev-debug -t upload     # discrete board, unchanged
```

`default_envs` is still `esp32dev-debug`, so a bare `pio run` builds the
discrete image. **Pass `-e` explicitly** on this board.

Three settings in the `[waveshare_s3]` section are load-bearing, and each fails
in a way that does not point at its cause:

| Setting | Without it |
| --- | --- |
| `memory_type = qio_opi` | boots fine, reports **zero PSRAM**; surfaces later as a null allocation |
| `flash_size = 16MB` | half the flash unaddressable, 16 MB partition table rejected |
| `ARDUINO_USB_CDC_ON_BOOT=1` | no console once GPIO43/44 are repurposed |

### The LVGL config trap

`lv_conf.h` needs **two** build flags, and the second is the non-obvious half:

```ini
-DLV_CONF_INCLUDE_SIMPLE
-I "${platformio.include_dir}"
```

PlatformIO puts `include/` on the search path for the *project's* sources but
not for libraries it builds under `.pio/libdeps`. With only the first flag the
effect is quietly asymmetric — this project's translation units see `lv_conf.h`
and compile against it, while LVGL's own compile with built-in defaults. It does
not fail at compile time. It fails at **link** time:

```
undefined reference to `lv_font_montserrat_48'
undefined reference to `lv_log_register_print_cb'
```

which reads as a missing library rather than a configuration that never
arrived. Anything enabled in `lv_conf.h` and referenced from project code will
show up in that list; anything merely *tuned* there will silently be the
default.

> `LV_CONF_PATH` with an absolute path is the other documented route and is
> worse here. It is substituted into `#include LV_CONF_PATH` as a C string
> literal, and a Windows path puts backslashes inside it — where `\U` in
> `\Users\` is an invalid escape sequence.

Fixing that immediately exposes a second trap, because `lv_conf.h` now actually
reaches LVGL's build: **the file must contain nothing an assembler cannot
parse — macros only, no `#include`.**

LVGL ships hand-written assembly for some targets (`lv_blend_helium.S`). Those
are `.S` files, so the C preprocessor runs over them, and they pull in
`lv_conf_internal.h` → `lv_conf.h`. Whatever is in it is fed to the assembler:

```
.../sys-include/stdint.h:22: Error: unknown opcode or format name 'typedef'
*** [.../lv_blend_helium.S.o] Error 1
```

LVGL's own `lv_conf_template.h` opens with `#include <stdint.h>`, so copying
the template verbatim reproduces this. The assembly bodies are guarded by
`LV_USE_DRAW_SW_ASM` and compile to nothing on Xtensa — only the preamble has
to stay clean. `include/lv_conf.h` writes its sizes as plain integer literals
for exactly this reason.

`build_src_filter` in `[env]` excludes `bringup/` from every other environment —
each harness has its own `setup()`/`loop()`, and without the exclusion they
collide with `main.cpp` on duplicate symbols. That filter is what lets a
bring-up sketch live in the repo instead of in a scratch project that rots.

---

## 7. Open

- **Mux, or one flat bus?** Recommended above, but it is hardware not yet in
  hand. The interim wiring works and is detectable-but-uncontained.
- **`charging` as unknown.** Needs a `device_status` / schema decision:
  the column is already nullable, so publishing `null` may need nothing but a
  firmware change — worth confirming against `supabase/schema.sql`.
- **Does the touch UI want a keypad?** Commissioning WiFi through the captive
  portal still needs a phone. An on-screen SSID picker would remove that, and is
  the one screen that would justify a text-entry widget.
- **Enclosure and viewing angle.** The 2" panel replaces indicators readable
  across a kitchen. A screen that has to be approached to be read is a different
  affordance from an LED that is visible from the door.
