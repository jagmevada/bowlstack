# Front-end

Planning notes and project-side context. The **implementation contract** lives
in [FRONTEND_HANDOFF.md](FRONTEND_HANDOFF.md), which is deliberately
self-contained so the front-end can be built without reading the firmware repo.

A working prototype of every screen below is in [../web/](../web/) — static
files, no build step, deployed to GitHub Pages for the field trial. Bring-up and
the trial instrumentation: [../web/README.md](../web/README.md).

---

## 1. Who uses it

The **kitchen in-charge**, during meal service. They need to know how much food
is left across three serving areas, and which stations need attention — then
tell the **service counter in-charge** to act.

That framing decides the design: this is an operational tool watched during a
busy service, not an analytics dashboard. Glanceability beats completeness.

---

## 2. Screens

### Stock view — the primary screen

Live bowl counts grouped by **location** (`D` Darshanarthi, `M` Mahatma,
`T` Tiffin), ordered by `food_slot`. Read `slot_overview`: several stacks serve one
dish position, so the number is their sum.

Design implications worth deciding early:

- **A count of 0–4 has very low resolution.** Colour-coding thresholds (e.g.
  ≤1 = red) probably communicates more than the digit.
- **`stack_status` gates trust in the number.** `discontiguous` must not render
  as a count at all — see the handoff for why.
- **Outside service hours the numbers are last-known, not live.** `data_is_stale`
  says so; the UI must, too, or a coordinator will act on a stale count.

### Health view

Battery, charging, `sensors_online`, firmware, `offline`,
`awaiting_deployment`. Sort by severity rather than device ID — the point is to
surface the one station that needs attention, not to enumerate 32 healthy ones.

### Configuration page

Assigns `location`, `food_slot`, `label` per device. The slot → food mapping is
its own screen, since it changes three times a day where an assignment changes
once a year.

---

## 3. The slot → food mapping — now modelled

`food_slot` is a **fixed physical position**, 1–8, painted on the station. What
food occupies it **changes with the meal**: breakfast, lunch and dinner rotate
through Dal/Kadhi, Rice, Curry, Roti and others.

This is now in the database as `meal_food_mapping`, keyed
`(location, meal_date, meal_type, food_slot)`. Full contract, TypeScript
interfaces and queries: **[meal_mapping.md](meal_mapping.md)**.

The design question that governed the shape has been settled, and it was the one
that could not be deferred:

> Does history need to record what a slot *contained* at the time, so past bowl
> counts can be attributed to a dish?

**Yes** — so the mapping is temporal, keyed by `meal_date`. `status_events` joins
to it through the device's location and slot, which makes *"how much dal did we get
through last Tuesday"* answerable. Had it stored only the current menu, every
historical count would have become unattributable the moment the menu rotated, and
that is not recoverable retrospectively.

Both open questions are answered by the trial prototype in [../web/](../web/):

- **Edited live or set in advance?** Either. The editor is date-driven rather
  than pinned to today, so a menu can be entered days ahead; opening it during
  service edits the live meal. The preload flag is what makes both safe — an
  inherited menu is visibly a draft until saved.
- **Copy a whole day?** Yes, added as "Copy this day" — all three meals for one
  location written to another date. Preload only reaches backwards, so setting up
  a week still needed a forward operation.

The **admin UI (Part 3) is built** in `web/js/views/menu.js`, including the
preload behaviour that inherits the previous same-meal menu so an admin edits
differences instead of retyping.

---

## 4. Things that will surprise a front-end developer

Each of these is documented in the handoff; they are listed here because they
are the ones most likely to be got wrong by reasonable assumption.

| Assumption | Reality |
| --- | --- |
| "No data for hours = broken" | Devices are dark ~16 h/day **by design**. Use `offline`, which is service-hour aware. |
| "there is a battery percentage" | There is **`battery_level`** — a band. `null` means no cell detected, not flat. |
| "the band edges are fixed" | They are **hysteretic**. A cell leaves `medium` at 35% but re-enters at 40%. Do not infer a percentage from a band. |
| "History is regularly sampled" | Rows exist only on **change**. Gaps are steady state. |
| "one change = one arrival" | Writes are batched at most every **5 s**. Rows sharing an arrival instant can describe moments up to 5 s apart — order by `recorded_at`. |
| "`recorded_at` ≈ `received_at`" | Offline events are **backdated** from a device-reported age. A large gap is correct. |
| "A device is a board" | A device is an **installation**. A replaced board keeps the `device_id`; only `mac` changes. |
| "`food_slot` is a dish" | It is a **physical position**. The dish changes per meal — join `meal_food_mapping`. |
| "one device per slot" | Darshanarthi runs **three** counters per position. Stock is the **sum** — read `slot_overview`. |

---

## 5. Access model

| Role | Access |
| --- | --- |
| anonymous | **nothing** |
| `authenticated` | SELECT everything; UPDATE `location`, `food_slot`, `label`, `timezone` on `devices`; full CRUD on `meal_food_mapping` |
| device (`anon` key) | write-only: UPDATE its own `device_status`, INSERT `status_events` |

`device_id` is not updatable from the UI — it is the installation's identity and
the key every history row hangs off.

Devices cannot read **any** telemetry column, including their own. That is
enforced by column-level grants rather than policies, so it holds even for
queries the policies would otherwise permit.
