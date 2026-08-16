# Working in this repo

Project-wide conventions for Claude Code. For what the system *is*, start at
[README.md](README.md); for the port in progress, [todo.md](todo.md) opens with
a resume section.

---

## Touch-UI development: iterate in the simulator

**`pio run -e sim` is the loop. Flashing is the check, not the loop.**

| | |
| --- | --- |
| Edit `src/ui/*.cpp`, rebuild sim | **~9 s** |
| Edit `include/lv_conf.h`, rebuild sim | ~60 s (every LVGL TU includes it) |
| Build and flash the device | **~2 min** |

Fourteen flashes is half an hour of waiting for what the simulator answers in
two minutes. Use it.

```
pio run -e sim                                          # builds + opens a window
pio run -e ws-s3-bringup -t upload --upload-port COM6   # only when it matters
```

### The rule that makes the simulator worth trusting

**Both targets must compile the same UI source.** `src/ui/` is pure LVGL — no
board, driver or framework headers — and `build_src_filter` includes `+<ui/>` in
both the `sim` and `ws-s3-bringup` environments. A preview that renders its own
copy of the screens is a picture that resembles the product; one that renders
the product's own code is evidence about it.

That is why the seam is drawn at [include/ui_state.h](include/ui_state.h) rather
than at `DeviceStatus`, which reaches for `config.h` → `Arduino.h` → `Wire.h` and
cannot exist on a PC.

**Anything the UI renders comes from a SHARED fixture or from a genuinely
platform-specific source. There is no third category.** Mock data lives in
[src/ui/ui_demo.cpp](src/ui/ui_demo.cpp) and both targets install it. The MAC is
the one legitimate divergence: the device reads its eFuse, the desktop has none.

> This is not hypothetical. The simulator was once told it was connected to a
> network at −58 dBm while the device was told nothing, so the two rendered
> different headers from one source file and it read as a rendering bug. "Make
> the demo look nicer" is how the third category gets invented.

### What the simulator cannot tell you

Trust it for **layout, overlaps, wording, pixel sizing, state logic, widget
behaviour**. Go to the panel for:

- **Legibility.** The monitor is ~96 DPI, the panel ~200. Identical pixels, very
  different apparent size — this is exactly how the type scale got set.
- **Colour and black level.** An IPS LCD's black is backlight through a closed
  shutter; it will always look greyer than the monitor, and no code changes that.
- **Touch accuracy and target sizes.**
- **Viewing angle, backlight, anything physical.**

So: iterate freely in the simulator, and batch a device check whenever a change
touches type sizes, colour or touch targets.

---

## Build

`pio` is **not on PATH** here — use `~/.platformio/penv/Scripts/pio.exe`.

**`pio` exits 0 even when the build FAILED.** The shell exit code proves
nothing; grep the output for the `SUCCESS`/`FAILED` status line. A flash reported
as done on the strength of an exit code has already happened once in this repo.

| Environment | Builds |
| --- | --- |
| `sim` | desktop preview, MSYS2 mingw64 + SDL2 |
| `ws-s3-bringup` | Waveshare ESP32-S3 board bring-up |
| `esp32dev`, `esp32dev-debug` | the discrete board — **need `include/secret.h`, absent here** |

`default_envs` is `esp32dev-debug`, so a bare `pio run` builds the discrete
image. Always pass `-e`.

The simulator holds `program.exe` open while running; stop it before a rebuild
or the link fails with `Access is denied`.

---

## Editing

**Use the `Edit` tool for anything non-trivial, not scripted string replacement.**
A mismatched `Edit` fails loudly; a mismatched `str.replace()` succeeds at
nothing and the next build is confusing rather than red. Several build cycles
have been lost to exactly that, plus one `\n` written as a literal newline into a
C string. Reserve scripting for genuinely mechanical bulk edits.

---

## Conventions this codebase already holds to

These are established in the existing firmware and the UI follows them; they are
worth preserving rather than rediscovering.

**Never claim what cannot be measured.** No cell detected reports `null`, not 0%.
A sensor that has not concluded reports `unknown`, not "no bowl". An impossible
stack reports a fault, not a count. The board cannot read charge state, so
`charging` is unknown rather than false; it has no RTC, so the clock reads
`--:--` rather than `00:00`.

**One task owns a subsystem.** `sensorTask` owns `SensorArray` exclusively; state
crosses task boundaries only as immutable snapshots under a mutex. The UI follows
suit — one task makes every `lv_*` call, which is why `LV_USE_OS` is `LV_OS_NONE`.

**Measurement never waits on the network.** Core 1 carries sensing; WiFi and TLS
live on core 0.

**Comments explain why, not what.** Especially where a value was chosen against a
plausible alternative, or where a bug's symptom pointed somewhere other than its
cause.
