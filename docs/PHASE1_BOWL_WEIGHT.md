# Phase 1 — per-bowl weight and the Master Dashboard

**Status:** built and verified. Dashboard **v1.23**.
**Not included:** the 20 load cells. Phase 1 is deliberately the estimate —
`bowls × weight` — and needs no new hardware. See [What comes next](#what-comes-next).

---

## See it right now

There are two ways, and they answer different questions.

### A. Demo mode — no database, no hardware

```
web/index.html?mock=1
```

`?mock=1` is a **query parameter, before the `#`**. These work:

```
index.html?mock=1
index.html?mock=1#/master
```

This does **not** — the flag is inside the hash, where the app never looks:

```
index.html#/master?mock=1        ← wrong
```

It also has to be served over `http://`, not opened as a `file://` path: the
app is built from ES modules and a browser refuses to load those from `file://`.
Any static server will do:

```powershell
cd web
python -m http.server 8080
# then open  http://localhost:8080/index.html?mock=1#/master
```

You should see a permanent amber **"Demo data"** strip, and Master showing four
slot cards whose figures count down as you watch.

### B. Against your real Supabase

Master reads a view that does not exist yet. **Run this once**, in the Supabase
SQL editor:

```
supabase/migrate_bowl_weight.sql
```

It adds one column to two tables, widens two functions and creates one view.
It drops nothing and rewrites no existing row, so it is safe to run mid-trial.

**It is reversible.** `supabase/rollback_bowl_weight.sql` puts the database
back exactly as it was — verified by fingerprinting every column, view and
function signature before and after, and comparing. The only thing lost is the
per-bowl weights themselves; dish names, devices, telemetry and history are
never touched. Both files can be re-run, and you can migrate again after a
rollback.

Then, on the **Menu** tab, fill in the new *kg / bowl* box beside each dish and
press Save. Master turns bowl counts into kilograms from those figures — until
a dish has a weight, that dish contributes bowls but no mass.

> **If Master is empty, it now tells you which of these you are looking at.**
> An un-migrated database says *"Master needs one migration"* and names the
> file. A migrated database with nothing reporting says the fleet has not
> reported. Those two used to look identical, and sent people to opposite
> places — one to the SQL editor, the other out to the hall to check stations
> that were working fine.

---

## What shipped

| Area | Change |
| --- | --- |
| **Menu** | A second field per slot — the weight of one bowl of that dish, in kg |
| **Master** | A new tab: quantity per dish position in kilograms, itemised by serving hall |
| **Demo mode** | `?mock=1` runs the entire dashboard with no backend at all |
| **Schema** | `bowl_weight_g` on both menu tables; the new `slot_quantity` view |

New files: [`supabase/migrate_bowl_weight.sql`](../supabase/migrate_bowl_weight.sql),
[`web/js/views/master.js`](../web/js/views/master.js),
[`web/js/mock.js`](../web/js/mock.js).

---

## The decision that shaped it

The fleet is **20 stacks over 4 dish positions** — Darshanarthi runs three
counters per position (12), Mahatma and Tiffin one each (4 + 4). A slot number
means the same dish position in every hall, so Master groups by **`food_slot`
alone, across all three areas**: 4 rows, each summing 5 devices.

That is the one place its grouping differs from `slot_overview`, and the reason
it is a separate view rather than a widened one — the Stock screen is the one
people watch through a service, and it carries no risk from this work.

### Sum-then-multiply, not multiply-then-sum

The obvious formula is *(total bowls across the slot) × (one weight)*. It is
wrong whenever the halls do not serve the same dish at that position — and they
do not: slot 3 is Curry at Darshanarthi, Sabzi at Mahatma, Bhaji at Tiffin.
Those bowls do not weigh the same, so there is no single weight to multiply by.

So each hall is weighed against **its own dish** first, and the products are
summed:

```
slot_total = Σ over areas ( bowls(area, slot) × weight(area, slot) )
```

Worked example, verified against a real Postgres:

| Hall | Dish | Bowls | Per bowl | Contribution |
| --- | --- | ---: | ---: | ---: |
| Darshanarthi | Curry | 9 | 4.5 kg | 40.5 kg |
| Mahatma | Sabzi | 2 | 4.0 kg | 8.0 kg |
| Tiffin | Bhaji | 1 | 3.0 kg | 3.0 kg |
| | | **12** | | **51.5 kg** |

The naive formula gives 54.0, 48.0 or 36.0 depending on which hall's weight you
happen to pick — and none of them would *look* wrong on screen.

When the halls agree — the normal case — this reduces exactly to
`total_bowls × W`, so nothing is lost. It also means the view never has to
choose which hall's weight "wins", a decision that would have been arbitrary
and invisible.

---

## Three rules worth knowing

### 1. A missing weight is NULL, never zero

`0` would render a full counter as **"0.0 kg remaining"** the moment someone
typed a dish name and forgot the weight — sending staff to refill something
that is full. `NULL` renders as *"weight not set"* and prompts. Never coalesce
it.

### 2. A partial total is a bound, and says so

If a hall holds bowls that cannot be weighed, the total is rendered
**`≥48.5 kg`**, not a confident `48.5`, and the row carries a *Set weight ›*
link deep-linked to the hall that is missing one.

This reuses the `≥` notation `deviceStack()` already uses for a degraded
stack's count — this codebase already had a vocabulary for "real but
incomplete", and inventing a second one would have been worse. The bound
matters because a slot total that quietly drops an unweighed hall reads as
complete, and a kitchen under-orders against it.

### 3. Weight is stored in grams, as an integer

Master sums products across up to five devices and three halls per slot. Doing
that in binary floating point puts visible drift on a screen showing one
decimal. The UI divides by 1000 exactly once, at render.

The column is bounded to **100 g … 50 000 g**. Those are sanity rails, not
policy: they turn a units mix-up — kilograms typed into a grams field, or a raw
gram figure pasted into the kg box — into a rejection at the edge rather than a
dashboard reading 14 000 kg.

---

## The layout, and why there is no grand total

Each slot is a card: the dish and that slot's own total on top, then **one line
per serving hall** with its bowls and its kilograms.

```
Slot 4   Roti                              40.0 kg
                                       9 of 20 bowls
  Darshanarthi     5 of 12 bowls           10.0 kg
  Mahatma          2 of 4 bowls             4.0 kg
  Tiffin           2 of 4 bowls             4.0 kg
```

The halls are itemised because that is the unit of action: a refill, or a
repair, is dispatched to a hall, never to a slot number. Fault, offline and
degraded markers sit on the hall that owns them, so "slot 3 is compromised"
becomes "Darshanarthi's stack at slot 3".

**There is no site-wide total.** An earlier cut showed one figure at the top
adding rice, dal, curry and roti together. It is arithmetically fine and
operationally meaningless — nobody cooks, orders or refills "212 kg of food" —
and a number nobody can act on at the top of a screen teaches people to skip
the top of the screen. Quantity is only meaningful per dish, so the totals stop
at the slot. The header carries the **meal** instead, which every figure below
genuinely depends on.

A faulted position now states its figure in red rather than refusing to show
one. `bowls_trusted` already excludes the faulted stack, so the number is what
the healthy stacks hold — and once the halls are itemised underneath, hiding a
total while listing every part of it reads as a bug rather than as caution. The
▲ on the offending hall's line carries the warning, exactly as an offline
stack's last known value is already kept in red.

## Between meals, the figure survives

`current_meal_type()` answers NULL outside every service window — correct for
*"is a meal running"*, wrong for *"what is that food"*. The bowls are still
physically on the stations; only the clock has moved on.

An earlier cut of `slot_quantity` joined the menu on the *current* meal, so the
moment dinner closed it lost every dish name and every weight, and Master read
**"12 bowls, no weight set"** for roughly sixteen hours a day.

`public.last_served_meal(tz)` fixes it by resolving the most recently **started**
window rather than the currently open one. During service that *is* the current
meal, so nothing changes; outside service it is the meal that just finished —
which is exactly the food still sitting on the counters.

Two properties are deliberate:

- **It looks backward only.** Tomorrow's menu is usually entered a day ahead,
  and picking it up would print tomorrow's dish over tonight's leftovers — a
  confident wrong answer rather than a missing one.
- **It crosses midnight.** At 05:00 the most recent meal is *last night's*
  dinner; a today-only lookup would answer nothing at all.

The view exposes `menu_meal_type`, `menu_meal_date` and `menu_is_live`, and the
row says **"from Dinner · 19 Aug"** whenever the figure is not from the running
meal. A number from last night must never read as live.

This is the one place Master deliberately diverges from Stock, which still shows
"No current dish" between meals. Master's whole purpose is the kilogram figure,
and a kitchen planning tomorrow's cooking wants last night's remaining stock in
kg — not a blank.

## Which slots appear

A slot earns a row **once at least one of its devices has ever reported**.

That hides `BWL-021`…`BWL-024` — the backups, which are registered and assigned
to slot 5 but have never been powered on. Parking a permanent *"no data"* row
there would train people to ignore a row that means something real. They appear
by themselves the day those four are deployed.

The gate is deliberately *has ever reported*, **not** *is reporting now*. A slot
whose devices all die mid-service **keeps its row**, its last known figure and
its offline flags. Vanishing at the moment something breaks is the one
behaviour a stock screen must not have.

---

## Demo mode, in a bit more detail

[`web/js/mock.js`](../web/js/mock.js) stands in for the Supabase client and
generates the real 20-device deployment plus the four backups and eight
reserved units. Three properties are deliberate:

- **It drifts.** Bowl counts fall with the wall clock, each stack at its own
  rate, so every poll visibly changes the screen. A static seed cannot show
  the one thing this screen is for.
- **It is writable.** Type a weight on the Menu tab, press Save, and the Master
  total moves. The editing path is the part most worth trying before it is
  wired to a real database.
- **It is always in service.** Devices are dark ~16 h a day by design, so a demo
  opened at 3pm would correctly render "outside service hours", freeze, and
  idle its poll to 10 minutes — accurate, and useless as a demo.

Everything is derived from two mutable stores by functions that mirror
`device_overview`, `slot_overview` and `slot_quantity`, so the tabs cannot
contradict each other. Demo mode is opt-in per URL and never sticky: a screen
showing fabricated bowl counts that someone believes is stock is worse than a
screen showing nothing.

The fixture deliberately includes the awkward cases — a faulted stack in slot 1,
an offline stack in slot 2, and slot 3 with three different dishes of which
Tiffin's is unweighed.

---

## Verification

Docker was unavailable and PostgreSQL refuses to run under a Windows admin
token, so the SQL was executed against **PGlite — real PostgreSQL 18 compiled
to WASM**, which compiles and runs plpgsql rather than merely parsing it.

| Check | Result |
| --- | --- |
| `schema.sql` → register → assign → `smoke_test.sql` | 24 PASS, 1 SKIP¹ |
| Pre-change schema from `git show HEAD` → `migrate_bowl_weight.sql` → same suite | 24 PASS, 1 SKIP¹ |
| `slot_quantity` arithmetic (21 assertions, hand-computed answers) | ALL PASS |
| `last_served_meal()` at pinned instants — midnight rollover, the tomorrow's-menu hazard | 15 assertions, ALL PASS |
| Migration run twice, then on an already-migrated database | idempotent; stored weights survive |
| migrate → rollback → migrate again | schema restored byte-identically; 12 assertions, ALL PASS |
| `web/test/smoke.mjs` | 304 assertions, ALL PASS |
| Demo mode booted end-to-end with no Supabase | all five tabs render |

¹ Assertion 24 resolves the dish through `current_meal_type()`, which is NULL
outside service hours by design, so it reports SKIP rather than a false failure
— the same constraint assertion 19 documents. Its arithmetic is covered
separately by the 21-assertion suite, which widens the service window to force
the case.

The second row is the one that matters operationally: it is the path a live
trial database actually takes.

New SQL assertions (20–24) cover the range CHECK, the weight surviving
`meal_mapping_preload()`, cross-area grouping, the has-ever-reported gate, and
the per-area arithmetic. `smoke_test.sql` is now **25 assertions**.

### Two things found along the way

- A first pass at `meal_template_apply()` was reconstructed from memory and
  silently dropped two real guards — the *no template rows for this weekday*
  skip and the *today's already-completed meals are history* skip. Caught by
  diffing against the original; the shipped version differs from it only in the
  INSERT column list.
- A test that backdated `updated_at` failed because `tg_device_status_stamp`
  overwrites it with `now()` on every UPDATE — deliberately, so a device cannot
  pin the column and hide going offline. The test was wrong; the trigger was
  doing exactly the job its comment claims.

---

## What comes next

Nothing in Phase 1 blocks any of this. `slot_quantity` gains a
`measured_weight_g` column and the UI gains a source chip.

| Phase | |
| --- | --- |
| **2** | `devices.kind` discriminator → `LDC-001`…`LDC-020` reusing the whole registry, assignment, service windows and offline logic; nullable weight columns on `device_status` using the same anon column-grant write path; HX711 firmware |
| **3** | `measured_weight_g` in `slot_quantity`, the measured-vs-estimated precedence rule (decided once, in SQL), and a mismatch chip — a disagreement between the two is itself a signal: a miscalibrated cell, or a wrong per-bowl weight in the menu |
| **4** | A `weight_samples` table for the analog history — **not** `status_events`, which is stack-shaped and NOT NULL on stack columns — then burn rate and projected run-out (*"Rice runs out ~13:42"*), the highest-value operational number in the whole design |

Out of scope by decision: a **Tare / Calibrate** button on the dashboard. There
is no browser→device command path anywhere in this architecture today; adding
one means a desired-state table and firmware polling, and is its own project.

---

## Files changed

```
new   supabase/migrate_bowl_weight.sql   the live-database migration
new   supabase/rollback_bowl_weight.sql  undo it, exactly
new   web/js/views/master.js             the Master Dashboard
new   web/js/mock.js                     demo-mode data

      supabase/schema.sql                weight column, both functions, slot_quantity
      supabase/smoke_test.sql            assertions 20-24
      web/index.html                     the Master tab
      web/js/app.js                      route, slot_quantity fetch, demo strip
      web/js/domain.js                   fmtWeight, slotQuantity, siteTotal, areaTotals
      web/js/supa.js                     demo-mode client hook
      web/js/views/menu.js               the weight field, in 4 builders + 7 save paths
      web/app.css                        4-column row-form, Master layout, demo strip
      web/sw.js                          cache v1.23 + the two new files
      web/js/version.js                  1.23
      web/test/smoke.mjs                 fixtures, the slot_quantity stub, new tests
      README.md, web/README.md, docs/*   documentation
```

> `web/sw.js` caches the app shell by filename. The cache key was bumped to
> `bowlstack-shell-v1.23` and the two new modules added to `SHELL` — without
> that, the Master tab 404s offline on a tablet that already has the old shell.
