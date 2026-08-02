# Bowlstack dashboard

A field-trial front-end for the Bowlstack fleet. Static files, no build step,
no `node_modules` — copy the folder onto any static host and it runs.

Built against the contract in [../docs/FRONTEND_HANDOFF.md](../docs/FRONTEND_HANDOFF.md).
Where that document and this code disagree, the document is right.

---

## Bring-up, in order

### 1. The database

If it is not already applied, run these in the Supabase SQL editor in order —
`schema.sql` drops everything, so `register_devices.sql` is not optional
afterwards, it is part of the same operation:

```
supabase/schema.sql
supabase/register_devices.sql
supabase/assign_devices.sql
supabase/seed_meal_mapping.sql     -- sample menus, so the dashboard shows dish names
supabase/smoke_test.sql            -- 20 assertions; expect ALL PASS
```

On a database that is already live, do NOT re-run `schema.sql` (it drops
everything). Run the additive migration instead:

```
supabase/weekly_menu_and_offline.sql   -- missed-service flag + weekly template
```

It is idempotent, ends with its own PASS/FAIL verification, and `schema.sql`
has been updated to produce the identical state on a fresh rebuild.

### 2. Credentials

```powershell
python tools/make_web_config.py
```

That reads `SUPABASE_URL` and `SUPABASE_ANON_KEY` out of `include/secret.h` —
the same file the firmware builds against, so the two cannot drift onto
different projects — and writes `web/config.js`. Re-run it whenever `secret.h`
changes. Both files are gitignored; the committed template is
[config.example.js](config.example.js).

### 3. No sign-in

The dashboard opens straight onto the stock screen. Nobody types anything.

There is one thing to turn on for that to work. Reads require the
`authenticated` role — `anon` is write-only **by design**, so a device can never
read another installation's telemetry (`schema.sql` §8). Skipping authentication
therefore does not give you a public dashboard, it gives you an empty one, which
on screen is indistinguishable from a dead fleet. So the app signs itself in
silently instead:

> Supabase → **Authentication → Sign In / Providers → Allow anonymous sign-ins → ON**

That mints a real `authenticated` JWT per browser with no prompt and no account.
**Not one policy or grant changes** — it is a way of *getting* the authenticated
role, not of going around it.

> **What this trades away.** With anonymous sign-ins on, anyone who has the site
> URL can read the fleet — bowl counts, device health, menus — and edit the menu
> and assignments. For a 2–3 day trial on a page nobody has been given the link
> to, that is usually fine. It is not a production posture.
>
> If you would rather not open that up, create one account (**Authentication →
> Users → Add user**, Auto Confirm ticked) and run
> `python tools/make_web_config.py --email you@example.com --password …`. The
> app then signs in silently as that account and nothing else can. Still no
> prompt; the password just sits in `config.js` instead.
>
> Either way, also turn **off** "Enable sign ups" under Email — nobody should be
> able to self-register into a fleet console.

### 4. Run it

ES modules need a real origin, so `file://` will not work:

```powershell
cd web
python -m http.server 8080
# then open http://localhost:8080
```

### 5. Publish to GitHub Pages

**Live at https://jagmevada.github.io/bowlstack/**, deployed from the
`frontend` branch by `.github/workflows/pages.yml`.

Three one-time steps, in this order:

1. **Secrets.** `SUPABASE_URL` and `SUPABASE_ANON_KEY`, so the key stays out of
   git history. Pipe them from `secret.h` rather than pasting, and they never
   touch a terminal or a shell history file:

   ```powershell
   gh secret set SUPABASE_URL      # paste when prompted, or pipe from secret.h
   gh secret set SUPABASE_ANON_KEY
   ```

   Add `DASHBOARD_EMAIL` / `DASHBOARD_PASSWORD` too if you chose the account
   route over anonymous sign-in.

2. **Turn Pages on** — repo → **Settings → Pages → Source: GitHub Actions**, or

   ```powershell
   gh api -X PUT repos/OWNER/REPO/pages -f build_type=workflow
   ```

   The workflow cannot do this itself. `configure-pages` has an `enablement`
   input, but *creating* a Pages site needs admin rights `GITHUB_TOKEN` does
   not have, and the failure reads "Resource not accessible by integration".
   Worse, a repo that has never had Pages can auto-enable as a **legacy**
   Jekyll site serving the repo root — the README, not the dashboard. If the
   site shows your README, that is what happened; the `PUT` above fixes it.

3. **Push.** Any push touching `web/**` on `main` or `frontend` redeploys.

> `frontend` is in the trigger list so the trial can deploy before the
> dashboard is merged. Drop it once this lands on `main`.

The anon key is built to live in a browser and row-level security is what
protects the data — but the devices hold the same key, so publishing it widens
who could write telemetry for an arbitrary `device_id`. That is already true of
every flashed board (`docs/supabase.md` §5, "Not yet done"); hosting the
dashboard widens who could, not what they could do. The permanent answer is
per-device JWTs.

On a kitchen tablet, open the site and **Add to Home Screen**. It installs as a
standalone app, opens straight onto the stock screen, and keeps working through
short WiFi dropouts (the shell is cached; data never is — a stale bowl count is
worse than none).

---

## The screens

| Tab | Source | What it is for |
| --- | --- | --- |
| **Stock** | `slot_overview` | Bowls left per dish position, per area. The screen watched during service. |
| **Health** | `device_overview` | Every device, **sorted by severity**. Answers "which station needs someone". |
| Device detail | `device_overview` + `status_events` | One device: levels, battery, history, and the reliability numbers a trial exists to collect. |
| **Menu** | `meal_mapping_preload` + `meal_food_mapping` | What each slot serves, per meal per day. |
| **Devices** | `devices` | Assign `location`, `food_slot`, `label`. Rare — hardware moves only. |

Data refreshes every **15 s** while the tab is visible, and polls rather than
subscribing: at the firmware's 20 s heartbeat, 32 devices would push ~140k
realtime messages a day at a screen glanced at every few minutes.

The poll interval is sized against the server's alarm, not picked round.
`offline` fires at 40 s without a report, so worst-case time-to-notice is
40 s + one poll. At a 20 s poll that is 60 s — meeting "within a minute" with
no margin, and missing it outright if one request is slow. At 15 s it is
40–55 s: still inside the minute, with slack for a slow round trip, and
without a 10 s poll's request volume.

---

## The rules this UI follows

Each of these is a way a reasonable implementation gets it wrong. They live in
[js/domain.js](js/domain.js) so no screen re-decides them.

**Offline keeps the last value — in red.** When a device is not reporting while
it should be, its last count stays on screen (blanking it would send someone to
a station the screen just went silent about) but turns red with a dashed
underline — dashed because red on these numerals already means "critically
low", so hue alone could not tell a full-but-stale position from an empty live
one. The meter keeps its severity hue and gains a hatch; a banner on Stock
names the silent devices. Two server flags drive all of it: `offline` (died
mid-window) and `missed_last_service` (slept through the most recently
completed service window — the flag that survives the dark hours, added
because a device dead for six days used to look identical to a healthy one
between meals). Requires `supabase/weekly_menu_and_offline.sql`.

**Dark devices are not broken.** Devices are powered only during meal service
and are dark ~16 h/day. The alarm is `offline` — which the database computes,
service-hour aware. Nothing here derives staleness from `updated_at`; at 16 dark
hours a day that would false-alarm on every healthy unit and bury the one that
actually failed.

**Three quiet states, three treatments.** `awaiting_deployment` is greyed into
its own section; `data_is_stale` is shown with an "as of" time; `offline` is the
only one that alarms.

**`discontiguous` is never rendered as a count.** A bowl detected above an empty
level is physically impossible, so it is a failed sensor or a bad mount — not
two bowls. The slot shows a fault; the count is withheld. Where some stacks at a
position are still fine, their total appears as a qualified subline, never as
the position's stock.

**`degraded` is a lower bound**, shown as `≥n`.

**`bowls_trusted: null` is "no data", not zero.** One sends someone to refill,
the other to investigate.

**Capacity comes from the view** (`devices × 4`), never a hardcoded 4 or 12. Add
a fourth stack to Darshanarthi slot 1 and the ceiling rises on its own.

**Battery is a band with no percentage.** The bands are hysteretic, so no number
is inferred from them in either direction, and `null` renders as "no battery" —
never a flat-battery icon.

**Colour is normalised per stack.** A 0–4 count has too little resolution for
the digit to carry the message, so the card is banded by bowls-per-stack
(≤1 critical, ≤2 warning). Comparing raw totals would paint a one-stack position
red beside a three-stack one at the same fullness.

**A filter that hides devices says so.** The Health list is sorted by severity,
so a healthy device sits mid-list and every problem filter hides it outright —
which reads as "the device is missing" rather than "the device is fine". A
banner names the count being withheld and offers one click back, and the search
box deliberately **overrides** the filters, because someone holding a board
wants to know whether it is reporting, not whether it is broken.

**Status colour never travels alone.** There are three tones, not four: the
status palette cannot separate four levels on hue (warning against serious
measures below the threshold at which two colours are reliably distinguished,
and good against critical is the classic red/green pair under deuteranopia).
So colour says fine / watch / act, severity *order* is carried by position in
the list, and every badge pairs its colour with a distinct glyph and a word.

**History is not a time series.** `status_events` has one row per real change,
so the chart is a step (the value between rows is genuinely constant) and gaps
are steady state. It is ordered by `recorded_at`, never `id` or `received_at`:
writes batch at most every 5 s, so rows sharing an arrival can describe moments
5 s apart.

**An area without a slot is allowed.** `slot_overview` groups by
`(location, food_slot)` and drops rows where either is null, so such a device
contributes to no dish position total. That is a legitimate configuration —
not every unit is tied to a serving position — so the form saves it as given.
It is never blocked, reddened or alarmed; it sits at the bottom of the Health
ranking as "Not assigned to a position" and nothing more.

**A refresh never rebuilds the page.** Replacing the view on every poll made
the screen blink and threw away scroll position, focus and any open history
table. Renders are now patched into the live DOM node by node, and the
"refetching" dim only appears if a request takes over 1.2 s. One consequence
worth knowing: a view that fills asynchronously must write to its slot **by
id** (`fillSlot`), never to a node it captured — under patching the live node
is kept and the freshly built one discarded, so a captured reference can be
detached by the time the data lands.

**The weekly template is configuration, not menu.** The Menu tab's "Weekly
template" mode edits `meal_menu_template`, keyed by weekday (0 = Sunday,
matching both Postgres `extract(dow)` and JS `getDay()`). The dashboard never
reads it: a weekday has no date, so resolving dishes from it directly would
let a service pass with a dish name on screen and no dated row behind it —
permanently breaking the "what was served last Tuesday" join. It reaches the
dashboard by materialisation only: the daily editor preloads from it (drafts
say "From the weekly template" until saved), and "Apply to dates" writes real
rows for a range — skipping any meal already entered, so deliberate one-off
changes survive, and refusing past dates outright. A saved dish that differs
from the template wears a quiet ◆ override badge.

**Clearing a dish is a DELETE.** "No dish here" and "a dish with no name" are
different, and a CHECK rejects the second — so blanks are filtered before the
upsert rather than submitted.

**A shared position is not a conflict.** `(location, food_slot)` is deliberately
not unique; the Devices tab shows "3 stacks here" as information.

**`device_id` is never written.** It is the installation's identity and the key
every history row hangs off. A replaced board keeps it; only `mac` changes.

---

## What to collect during the trial

The device page turns firmware telemetry into the numbers that decide whether
this is production-ready. None of it needs new schema:

- **Boots** — a device rebooting repeatedly is the single loudest signal.
- **Dropped events** — `seq` increments when an event is *enqueued*, so a gap in
  `seq` means events were genuinely lost, not merely never sent.
- **Buffered offline** — events backdated from a device-reported age. Their
  count and worst delay measure the WiFi, not the sensors.
- **Fault / degraded transitions** — how often a stack becomes unreadable. This
  is the number that justifies (or does not) the planned dual-sensor redundancy.
- **Battery band changes** — many per day means a cell resting on a threshold,
  and every flip is a row in `status_events`.

**Copy diagnostics** on the device page copies that device plus its recent
events as JSON. **Copy fleet diagnostics** in the ⋯ menu copies the whole fleet
snapshot. Both are meant to be pasted straight into a message from the floor.

---

## Layout

```
web/
  index.html          shell + the three top-level states
  app.css             design tokens, light and dark
  config.example.js   template; the real config.js is generated + gitignored
  manifest.webmanifest, sw.js, icon.svg
  vendor/supabase.js  supabase-js 2.110.8, vendored so a CDN cannot be the
                      thing that breaks during service
  js/
    version.js        the version shown in the header — bump on every deploy
    app.js            gates, router, polling, fleet chips
    supa.js           connection + client
    domain.js         every semantic rule, in one place
    ui.js             DOM helpers
    chart.js          step chart + status timeline
    views/            stock, health, device, menu, assign
  test/smoke.mjs      renders every screen against fixtures
```

### Versioning

The header shows the dashboard version, from [js/version.js](js/version.js).
Bump it on any deploy the trial should be able to tell apart — it is stamped
into both diagnostics copies, so a screenshot or a pasted JSON blob always
identifies the build it came from. The service-worker cache name is keyed to
it too, so a deploy cannot leave half an old shell behind.

### Tests

```powershell
cd web/test
npm install      # jsdom, only for the test — the app itself has no dependencies
node smoke.mjs
```

129 assertions. It loads the real `index.html`, stubs PostgREST with rows shaped
like `device_overview` / `slot_overview` / `status_events`, and drives every
screen. It exists to protect the rules in the section above — each is one
plausible edit away from breaking with nothing visibly wrong on screen.

## Known limits

- **No access control.** With anonymous sign-in on, anyone with the URL can read
  the fleet and edit menus and assignments. Deliberate for a trial; not a
  production posture.
- **No realtime.** Worst-case staleness is one poll, 15 s.
- **Session drops recover silently.** A token-refresh hiccup mid-session
  re-signs-in underneath the page: no gate, no redraw, nothing typed is lost.
  The full-screen connecting gate appears only on cold boot or when sign-in
  actually fails.
- **History is capped at 1000 events** per device per window.
- **Menu editing has no conflict detection.** Two admins on the same meal, last
  write wins.
- **`stale_for` is ignored.** Relative times are computed from `updated_at` for
  display only; every alarm still comes from the server's `offline`.
