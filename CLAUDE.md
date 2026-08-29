# Working in this repo

Project-wide conventions for Claude Code. For what the system *is*, start at
[README.md](README.md); for the port in progress, [todo.md](todo.md) opens with
a resume section.

---

## What branch you are on

`loadcell`. **The measurement here is WEIGHT, not bowl count.** Same Waveshare
board, same 240×320 panel, same UI framework and the same status bar — three
NAU7802 bridge converters under one platform instead of four VL53L0X up a pipe.

| | |
| --- | --- |
| cells | **GPIO21/16**, hardware I²C port 1, 400 kHz — the camera's SCCB pair, 4.7 k already fitted (R4/R5) |
| mux | **TCA9548A at 0x70**, one channel per cell (0/1/2) |
| port 0 | GPIO47/48 carries the touch chip and the IMU, **and nothing else** |
| why a mux | the NAU7802's address is fixed at 0x2A and cannot be moved |
| why three cells | two leave the platform free to rock about the line joining them; three points define a plane |

> This table described **two** cells on two buses until 3e58f87 — cell A sharing
> the touch bus, cell B bit-banged on GPIO11/12. Both pairs are free now. If you
> find that arrangement described anywhere else in the repo, it is stale;
> `include/board_waveshare_s3.h` section 8 is the authority.

The ToF branch is `touch-ui` and is untouched; the discrete ToF product is
`main`. `src/bringup/bringup_display.cpp` and `bringup_sensors.cpp` were removed
here rather than left half-compiling — see the note in `platformio.ini`.

**A unit reports COUNTS until somebody calibrates it.** That is deliberate, not
unfinished: with no known mass there is no counts-to-gram factor, so there are no
grams. Menu → Settings → Scale → Tare, put the known mass on, then Calibrate.

That rule reaches the database as a constraint rather than a convention:
`check ((weight_state = 'ok') = (weight_g is not null))`. A scale always sends
a *state*; it sends a *number* only when that state is `ok`. Zero, though, is a
real weight — a tared empty platform means *refill me*, where NULL means nobody
knows, and those send staff to opposite places.

### It reports upstream

`LDC-001` publishes to the same Supabase project as the bowl counters and shows
up on the same dashboard. `src/uplink.cpp` is the transport, shared verbatim
with the discrete product; `src/loadcell/scale_telemetry.cpp` is the only part
that differs. Current state PATCHes `device_status` every 20 s; history appends
to `weight_samples` on a 250 g move, on a state change, and otherwise every two
minutes.

**Not `status_events`** — five of that table's NOT NULL columns are bowl-shaped,
so a scale could append to it only by fabricating a bowl count.

The reason any of this exists is `slot_burn_rate`: which dish is going fastest
and whether it lasts the meal. Consumption is measured on **buffer plus
counter**, so carrying bowls from a stack onto the scale reads as flat rather
than as a serving.

See [docs/supabase.md](docs/supabase.md) for the schema and
[docs/FRONTEND_HANDOFF.md](docs/FRONTEND_HANDOFF.md) for what the dashboard
reads. `supabase/whats_installed.sql` says what a given database already has.

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
pio run -e ws-s3-loadcell -t upload --upload-port COM6   # only when it matters
```

### The rule that makes the simulator worth trusting

**Both targets must compile the same UI source.** `src/ui/` is pure LVGL — no
board, driver or framework headers — and `build_src_filter` includes `+<ui/>` in
both the `sim` and `ws-s3-loadcell` environments. A preview that renders its own
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
| `ws-s3-loadcell` | Waveshare ESP32-S3 + 2x NAU7802 -- the image this branch ships |
| `esp32dev`, `esp32dev-debug` | the discrete board — need `include/secret.h`, which is gitignored but **is** present in this checkout, so both build |

`default_envs` is `esp32dev-debug`, so a bare `pio run` builds the discrete
image. Always pass `-e`.

**A bare `pio run` therefore compiles the DISCRETE image against whatever is new
under `src/`.** That is how this branch's load-cell files first reached a build
that links no LVGL: `[env]`'s filter is a deny-list behind a `+<*>`, so anything
added to `src/` joins the discrete images until it is explicitly excluded. If you
add a file that belongs only to the panel build, exclude it there — the comment
above that filter lists what each entry is preventing.

The simulator holds `program.exe` open while running; stop it before a rebuild
or the link fails with `Access is denied`.

**Launch it with mingw64 on PATH**, or it dies at load with no message:

```
PATH="/c/msys64/mingw64/bin:$PATH" ./.pio/build/sim/program.exe
```

`SDL2.dll`, `libstdc++-6.dll` and `libwinpthread-1.dll` live there. Started
without it the process exits instantly, which is easy to misread as a crash in
the UI -- it has been misread that way once already. Backgrounding it from a
shell that then exits produces the same false alarm, reported as a segfault.

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
