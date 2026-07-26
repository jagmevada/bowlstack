# Front-end

Planning notes and project-side context. The **implementation contract** lives
in [FRONTEND_HANDOFF.md](FRONTEND_HANDOFF.md), which is deliberately
self-contained so the front-end can be built without reading the firmware repo.

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

Live bowl counts grouped by **area** (`D` Darshanarthi, `T` Tiffin,
`M` Mahtma), ordered by `item_slot` 1–5.

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

Assigns `area`, `item_slot`, `label` per device, and — the part not yet
designed — maps each slot to a food per meal.

---

## 3. The slot → food mapping (not yet modelled)

`item_slot` is a **fixed physical position**, 1–5, painted on the station. What
food occupies it **changes with the meal**: breakfast, lunch and dinner rotate
through Dal/Kadhi, Rice, Curry, Roti and others.

This mapping is deliberately **not in the database yet**. Modelling it as a
column on `devices` would have been wrong within a day, and the shape depends on
questions only the front-end design answers:

- Is the mapping per-area or fleet-wide?
- Does it vary by day as well as by meal?
- Is it edited live during service, or set in advance?
- Does history need to record what a slot *contained* at the time, so past bowl
  counts can be attributed to a dish?

That last one matters most: if yes, the mapping must be **temporal** — a table
keyed by `(area, item_slot, meal, effective_from)` — and `status_events` becomes
joinable to it. If no, a single current-mapping table is enough. Deciding this
after the fact would mean losing the attribution for everything already
recorded.

---

## 4. Things that will surprise a front-end developer

Each of these is documented in the handoff; they are listed here because they
are the ones most likely to be got wrong by reasonable assumption.

| Assumption | Reality |
| --- | --- |
| "No data for hours = broken" | Devices are dark ~16 h/day **by design**. Use `offline`, which is service-hour aware. |
| "there is a battery percentage" | There is **`battery_level`** — a band. `null` means no cell detected, not flat. |
| "History is regularly sampled" | Rows exist only on **change**. Gaps are steady state. |
| "`recorded_at` ≈ `received_at`" | Offline events are **backdated** from a device-reported age. A large gap is correct. |
| "A device is a board" | A device is an **installation**. A replaced board keeps the `device_id`; only `mac` changes. |
| "`item_slot` is a dish" | It is a **physical position**. The dish changes per meal. |

---

## 5. Access model

| Role | Access |
| --- | --- |
| anonymous | **nothing** |
| `authenticated` | SELECT everything; UPDATE `area`, `item_slot`, `label`, `location`, `timezone` on `devices` |
| device (`anon` key) | write-only: UPDATE its own `device_status`, INSERT `status_events` |

`device_id` is not updatable from the UI — it is the installation's identity and
the key every history row hangs off.

Devices cannot read **any** telemetry column, including their own. That is
enforced by column-level grants rather than policies, so it holds even for
queries the policies would otherwise permit.
