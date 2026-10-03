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
| cells | **GPIO12/11** (SDA=12 on P2-10, SCL=11 on P2-9), hardware I²C port 1, 400 kHz — **no on-board pull-ups**, the mux breakout supplies the trunk pair |
| mux | **TCA9548A at 0x70**, one channel per cell (0/1/2) |
| port 0 | GPIO47/48 carries the touch chip and the IMU, **and nothing else** |
| why a mux | the NAU7802's address is fixed at 0x2A and cannot be moved |
| why three cells | two leave the platform free to rock about the line joining them; three points define a plane |

> This table described **two** cells on two buses until 3e58f87 — cell A sharing
> the touch bus, cell B bit-banged on GPIO11/12. If you find that arrangement
> described anywhere else in the repo, it is stale;
> `include/board_waveshare_s3.h` section 8 is the authority.
>
> It then said **GPIO21/16** here for two commits after the bus had already
> moved, and claimed the 4.7 k pull-ups were fitted — both wrong, and wrong in
> the direction that costs somebody an afternoon with a scope. What is true at
> HEAD: GPIO47/48 carries the touch chip and the IMU, GPIO11/12 is the cell
> trunk and needs its pull-ups from the mux breakout, and GPIO21/16 — the SCCB
> pair with R4/R5 fitted — carries the **buffer-stock bus**: its own TCA9548A at
> 0x70 with up to three 200 kg buffer platforms (B1..B3, one NAU7802 per mux
> channel), bit-banged (both hardware ports are spent), SDA on P1-7 and SCL on
> P1-4. Each buffer platform is its own unit (BWL-001..003) and a separate
> instrument from the counter — `src/loadcell/buffer_bank.cpp` over the
> Arduino-free core `src/loadcell/load_scale.cpp` (host tests:
> `bash tools/host_test/run.sh`); console `:buf` (and `z`/`g`/`k` for B1).
> `include/board_waveshare_s3.h` section 10 is the authority. Upload and the
> per-platform dashboard are in progress — see the resume section of todo.md.

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

## Every flash bumps the version. Mandatory, no exceptions.

**Before any build that is going to be flashed, bump `BOWLSTACK_FW_VERSION`.** It
is the last line under the logo on the splash, it is printed at boot
(`device LDC-001, fw …`), and it is published in telemetry. It is the only thing
that tells anyone which code is on a unit without reading its flash back.

> This rule exists because it was not followed. A unit came back from the field
> with `V1.2 260901` on its logo page while running code from 4 September, two
> commits later. Nothing on the board could say so; proving what it ran took an
> esptool flash digest compared against a rebuild.

**Format `V<major>.<minor> <YYMMDD>`**, e.g. `V1.3 261003`. Every flash: minor
goes up by one and the date becomes the flash date. Several flashes in a day are
exactly what the minor number is for, so a string is never reused. The major
number moves only when the user says so.

**The same string, character for character, goes in four places:**

| Where | Why |
| --- | --- |
| `platformio.ini`, `[env:ws-s3-loadcell]`, `-DBOWLSTACK_FW_VERSION` | the source of truth — the splash, the console and telemetry all read this flag |
| `platformio.ini`, `[env:sim]`, same flag | the simulator defines it separately; if the two differ, one commit shows two versions |
| `src/loadcell/loadcell_main.cpp`, a `// Firmware: …` line in the header comment | this image's entry point names its version in the source (`src/main.cpp` is the discrete ToF image — leave it, and `version.h`'s fallback, alone) |
| the commit message, a `Firmware: …` line in the body, above the Co-Authored-By | so `git log --grep "Firmware: V1.3"` finds the code behind a version seen on a unit |

**The bump is committed with the code that was flashed**, not in a later commit.
If the flash came first, commit straight after, with the same string.

Before flashing:

```
grep -n "BOWLSTACK_FW_VERSION" platformio.ini      # [env:ws-s3-loadcell] and [env:sim] lines must be identical
grep -n "Firmware:" src/loadcell/loadcell_main.cpp # must show the same string (add the line on the first bump if absent)
```

After flashing, **look at the device**: the logo page or the boot line on the
console must show the new string. If it shows the old one, the flash did not
take, whatever `pio` reported. Say the version in the done message.

---

## Editing

**Use the `Edit` tool for anything non-trivial, not scripted string replacement.**
A mismatched `Edit` fails loudly; a mismatched `str.replace()` succeeds at
nothing and the next build is confusing rather than red. Several build cycles
have been lost to exactly that, plus one `\n` written as a literal newline into a
C string. Reserve scripting for genuinely mechanical bulk edits.

---

## Every change gets a regression pass. No exceptions for "small" ones.

**A request names a symptom. Your job is the whole chain it sits in.** Before
calling a change done, trace what depends on the thing you touched — forwards to
what reads it, and outwards to the panel, the uplink, the database and the
dashboard. Then say plainly what you checked.

This rule exists because of a one-line change that read as trivial.

> *"remove auto tare at start up"* → `-DBOWLSTACK_AUTOTARE=0`. Done in one line,
> flashed, reported as complete. But the chain was:
>
> ```
> no auto-tare  ->  tared = false        (session tare never taken)
>               ->  weight_state 'untared'
>               ->  weight_g NULL        (the schema's own CHECK enforces it)
>               ->  the dashboard shows no mass at all
> ```
>
> Three hops from a build flag to a blank dashboard mid-service, and a red
> warning on a station that was measuring perfectly well. None of it was hard to
> see; nobody looked.

**What a regression pass actually is.** Not ceremony — four questions:

1. **Who reads what I changed?** `grep` the symbol. Follow it to every consumer,
   including SQL views and web code. A firmware field usually has three or four.
2. **Does it cross a task boundary?** The UI is `loop()`; the scale task
   preempts it. Anything written from a menu handler that the scale task also
   touches must be queued, not applied — see `wantWindow_`, and the comment
   above it explaining that a shared `Preferences` handle corrupts rather than
   merely races.
3. **Does the simulator still render what the device does?** Two `ui::State`
   fields added and not set in `ui_demo.cpp` means the preview shows the OLD
   behaviour and the divergence is invisible until it is flashed.
4. **What does the number mean now?** If a displayed figure changes, check the
   thing that is derived from it — a share denominator, a burn rate, a
   plausibility rail. A total that is now net of something breaks anything that
   assumed it was gross.

**Spawn a reviewer for anything beyond a comment fix.** An `Agent` given the
diff and told to hunt regressions has repeatedly found what the author missed —
including, on the vessel-offset change, an NVS write from the wrong task, a
zero-clamp that would have reported a counter holding food as EMPTY, and a menu
hint that lied after Clear calibration. All three would have shipped.

**Report what you verified and what you did not.** "Builds on all five, panel
checked, cell path NOT verified because the load cells are off the unit" is
worth more than a confident summary. Several hours went into chasing a stall
that turned out to be a damaged charger IC, because a plausible software story
was offered where "I have not measured this" was the honest answer.

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
