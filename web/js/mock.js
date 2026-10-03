// ====================================================================
//  Demo mode — the dashboard with no Supabase and no hardware behind it.
//
//  Open index.html?mock=1 and the app runs against this module instead of
//  PostgREST. It exists because the weight work has to be reviewed and
//  demonstrated before twenty load cells are mounted, and because a screen
//  whose whole point is a number counting down cannot be judged from a
//  static screenshot.
//
//  ONE SOURCE OF TRUTH, THREE VIEWS
//  --------------------------------
//  Everything below is derived from `devices` and `menu` — the two mutable
//  stores — by functions that mirror device_overview, slot_overview and
//  slot_quantity from supabase/schema.sql. Hand-writing three fixtures
//  instead would have let them disagree with each other, and a demo whose
//  Stock tab contradicts its Master tab teaches the reviewer nothing.
//
//  That mirroring is also why the menu store is WRITABLE: edit a dish on the
//  Menu tab in demo mode, press Save, and Master's dish names move.
//
//  THE FLEET AFTER THE CUT-OVER
//  ----------------------------
//  Every BWL-xxx is a BUFFER platform here (supabase/cutover_buffers.sql has
//  run): a load cell under the bowls waiting behind the line, reporting food
//  in kilograms and the bowls it is carrying. No device is a bowl stack any
//  more, so the per-bowl weight on the Menu tab feeds nothing and is hidden.
//  The stack rendering is covered by the smoke suite's kind-less fixtures,
//  which model a database from before the cut-over.
//
//  IT DRIFTS
//  ---------
//  Every figure is a function of the wall clock: buffers are served down a
//  bowl at a time and reloaded, counters are served down and topped up, each
//  device at its own pace. So a poll visibly changes the screen, and the
//  history a device page draws ends exactly where its live figure is.
//
//  ALWAYS IN SERVICE
//  -----------------
//  Devices are dark ~16 h a day by design, so a demo opened at 3pm would
//  correctly render "outside service hours", freeze, and drop the poll to
//  its 10-minute idle cadence — an accurate screen that demonstrates
//  nothing. Demo mode therefore forces in_service, and the banner at the
//  top of the app says the data is not real.
// ====================================================================

import { SERVICE_WINDOWS, DEFAULT_TZ } from './domain.js';

/** Demo mode is opt-in per URL and never sticky. It must not be reachable by
 *  accident on a kitchen tablet: a screen showing invented stock that
 *  someone believes is real is worse than a screen showing nothing. */
export function isMockMode() {
  try {
    return new URLSearchParams(location.search).get('mock') === '1';
  } catch { return false; }
}

const START = Date.now();
const TZ = DEFAULT_TZ;
const iso = ms => new Date(ms).toISOString();

// --- the fleet, exactly as supabase/assign_devices.sql states it -------
//
// Twenty deployed buffers over four dish positions: Darshanarthi runs three
// platforms per position, Mahatma and Tiffin one each. BWL-021..024 are the
// backups — assigned to slot 5 but never powered on, which is what makes
// them the test case for slot_quantity's "has ever reported" gate. They must
// stay invisible on Master while remaining visible on Health as awaiting
// deployment.
const ASSIGN = [
  ['BWL-001', 'D', 1], ['BWL-002', 'D', 1], ['BWL-003', 'D', 1],
  ['BWL-004', 'D', 2], ['BWL-005', 'D', 2], ['BWL-006', 'D', 2],
  ['BWL-007', 'D', 3], ['BWL-008', 'D', 3], ['BWL-009', 'D', 3],
  ['BWL-010', 'D', 4], ['BWL-011', 'D', 4], ['BWL-012', 'D', 4],
  ['BWL-013', 'M', 1], ['BWL-014', 'M', 2], ['BWL-015', 'M', 3], ['BWL-016', 'M', 4],
  ['BWL-017', 'T', 1], ['BWL-018', 'T', 2], ['BWL-019', 'T', 3], ['BWL-020', 'T', 4],
  // Backups. Assigned, registered, never heard from.
  ['BWL-021', 'D', 5], ['BWL-022', 'D', 5], ['BWL-023', 'M', 5], ['BWL-024', 'T', 5],
];
const BACKUPS = new Set(['BWL-021', 'BWL-022', 'BWL-023', 'BWL-024']);

// One buffer seeded into each state the UI has a rule for, so demo mode
// exercises every branch rather than twenty healthy platforms.
const NO_CELLS = 'BWL-002';     // D/1: the cell is not answering -- slot 1 reads ≥
const UNCONFIRMED = 'BWL-004';  // D/2: a count remembered across a power cycle -- "3?"
const SETTLING = 'BWL-008';     // D/3: first readings still arriving -- slot 3 reads ≥
const OFFLINE = 'BWL-014';      // M/2: died mid-service -- keeps its last value, in red
const EMPTY = 'BWL-016';        // M/4: tared and empty -- 0.0 kg, which means refill me
const NO_BATTERY = 'BWL-019';
const CRITICAL_B = 'BWL-005';
const LOW_B = 'BWL-011';

const AREA_LABEL = { D: 'Darshanarthi', M: 'Mahatma', T: 'Tiffin', R: 'Reserved' };

const devices = ASSIGN.map(([device_id, location, food_slot], i) => ({
  device_id, location, food_slot, i, kind: 'buffer',
  label: `${AREA_LABEL[location]} slot ${food_slot}`,
  timezone: TZ,
  // A buffer posts from the panel it is wired to, so it carries that
  // firmware -- there is no BWL image any more.
  firmware: 'V1.0 250826',
  mac: `A0:B1:C2:D3:E4:${String(0x10 + i).toString(16).toUpperCase()}`,
}));
for (let n = 25; n <= 32; n++) {
  devices.push({
    device_id: `BWL-0${n}`, location: 'R', food_slot: null, i: n, kind: 'buffer',
    label: 'Reserved', timezone: TZ, firmware: null, mac: null,
  });
}

// --- the counter scales ----------------------------------------------
//
// LDC-nnn sits at the same position as BWL-nnn -- the rule
// supabase/assign_loadcells.sql states and derives rather than repeats. The
// two are different food in different places: BWL-nnn weighs the bowls
// waiting behind the line, LDC-nnn the vessel on the serving counter.
//
// EVERY scale is registered and assigned, and only SOME have reported. That is
// not a shortcut, it is the rollout: slot 3's cells are installed on paper and
// not yet powered, exactly as BWL-021..024 are. It also keeps a buffer-only
// position (slot 3) on screen beside buffer-plus-counter ones (1, 2, 4).
const SCALE_SLOTS_LIVE = new Set([1, 2, 4]);

// Kept from the bowl-count era: a cell reading 2.2x heavy. Nothing on screen
// flags it, which is the honest outcome -- a scale reading heavy is
// indistinguishable from a counter holding more food, and catching it needs
// bowls REMOVED from a buffer against grams ADDED to the counter.
const SCALE_MISCALIBRATED = 'LDC-020';   // T slot 4
const SCALE_MISCAL_FACTOR = 2.2;
const SCALE_UNCALIBRATED = 'LDC-017';    // T slot 1
// Slot 2's cells at Mahatma and Tiffin are not fitted.
const SCALE_NOT_FITTED = new Set(['LDC-014', 'LDC-018']);

for (const [bufferId, location, food_slot] of ASSIGN) {
  const n = bufferId.slice(4);
  devices.push({
    device_id: `LDC-${n}`, location, food_slot, i: Number(n), kind: 'scale',
    label: `${AREA_LABEL[location]} slot ${food_slot} scale`,
    timezone: TZ,
    firmware: 'V1.0 250826',
    mac: `28:84:85:47:AB:${(0xB0 + Number(n)).toString(16).toUpperCase()}`,
  });
}
for (let n = 25; n <= 32; n++) {
  devices.push({
    device_id: `LDC-0${n}`, location: 'R', food_slot: null, i: n, kind: 'scale',
    label: 'Reserved (scale)', timezone: TZ, firmware: null, mac: null,
  });
}

// --- the menu, writable ----------------------------------------------
//
// Slot 1, 2 and 4 carry one dish across all three halls — the normal case.
// Slot 3 deliberately diverges, so Master lists three dishes on one card.
// The per-bowl weights are the bowl-count era's and multiply nothing here
// (there is no stack to count); they stay so the stored figures survive.
const SEED = {
  1: { D: ['Rice', 5000], M: ['Rice', 5000], T: ['Rice', 5000] },
  2: { D: ['Dal', 3500], M: ['Dal', 3500], T: ['Dal', 3500] },
  3: { D: ['Curry', 4500], M: ['Sabzi', 4000], T: ['Bhaji', null] },
  4: { D: ['Roti', 2000], M: ['Roti', 2000], T: ['Roti', 2000] },
  5: { D: ['Khichdi', 4000], M: ['Khichdi', 4000], T: ['Khichdi', 4000] },
};
const MEALS = ['Breakfast', 'Lunch', 'Dinner'];

const menu = new Map();            // "loc|date|meal|slot" -> row
const template = new Map();        // "loc|weekday|meal|slot" -> row
const mkey = r => `${r.location}|${r.meal_date}|${r.meal_type}|${r.food_slot}`;
const tkey = r => `${r.location}|${r.weekday}|${r.meal_type}|${r.food_slot}`;

function today() {
  return new Intl.DateTimeFormat('en-CA', {
    year: 'numeric', month: '2-digit', day: '2-digit', timeZone: TZ,
  }).format(new Date());
}

for (const [slot, byLoc] of Object.entries(SEED)) {
  for (const [location, [food_name, bowl_weight_g]] of Object.entries(byLoc)) {
    for (const meal_type of MEALS) {
      const row = {
        location, meal_type, meal_date: today(),
        food_slot: Number(slot), food_name, bowl_weight_g,
      };
      menu.set(mkey(row), row);
      for (let weekday = 0; weekday <= 6; weekday++) {
        const t = { location, weekday, meal_type, food_slot: Number(slot), food_name, bowl_weight_g };
        template.set(tkey(t), t);
      }
    }
  }
}

// --- the drift --------------------------------------------------------

const elapsed = () => (Date.now() - START) / 1000;

/** The meal the demo pretends is running. The real clock picks it so the
 *  screen agrees with the wall, but service is forced on regardless. */
function currentMeal() {
  const hhmm = new Intl.DateTimeFormat('en-GB', {
    hour: '2-digit', minute: '2-digit', hour12: false, timeZone: TZ,
  }).format(new Date());
  // The meal whose window most recently STARTED — the same rule as
  // public.last_served_meal(). Between meals that is the one that just
  // finished, NOT the next one due: the food on the counters at 3pm is
  // lunch's, and naming it "Dinner" put the demo a meal ahead of the
  // database it is standing in for.
  let last = null;
  for (const w of SERVICE_WINDOWS) if (hhmm >= w.start) last = w.meal;
  // Before breakfast opens, the most recent meal is last night's dinner.
  return last || SERVICE_WINDOWS[SERVICE_WINDOWS.length - 1].meal;
}

const BOWL_DRY_G = 2500;   // a steel bowl's own mass -- the firmware's figure
const BOWLS_LOADED = 4;    // what a reload puts on a platform; 4 is its real max
const BUFFER_CPG = 20.7;   // counts per gram, a 200 kg cell
const COUNTER_CPG = 106.857;

/** One full bowl's food, 14..18 kg, fixed per (device, cycle, bowl) so the
 *  same instant always yields the same figure. */
const fullBowlG = (dev, cycle, j) =>
  14000 + ((dev.i * 7919 + cycle * 104729 + j * 1299709) % 4001);

/** Seconds between full bowls leaving one buffer for its counter: 6..12 min,
 *  which reads like a service on Master's "Empty about". */
const bufferPeriodS = dev => 360 + (dev.i % 7) * 60;

/** What one buffer platform holds at an instant: [bowls, food_g].
 *
 *  WHOLE BOWLS ONLY -- the owner's description of the buffer: full bowls go on
 *  (up to four, the stack's height), and a FULL bowl is carried off to the
 *  counter, where the serving -- the gradual drain -- happens. So a buffer
 *  moves in steps of one bowl (gross and food both drop by it) and is flat in
 *  between. With the last bowl gone the platform sits empty for one period,
 *  reading a real 0.0 kg, then four full bowls go on. */
function bufferAt(dev, at) {
  const period = bufferPeriodS(dev);
  const cycle = (BOWLS_LOADED + 1) * period;
  const t = at / 1000 + dev.i * 97;                 // desynchronise the fleet
  const c = Math.floor(t / cycle);
  const k = Math.floor((t - c * cycle) / period);   // bowls already taken off
  if (k >= BOWLS_LOADED) return [0, 0];
  let food = 0;
  for (let j = k; j < BOWLS_LOADED; j++) food += fullBowlG(dev, c, j);
  return [BOWLS_LOADED - k, food];
}

/** Food in one counter vessel at an instant: served down from a full bowl's
 *  worth to a scrape, then topped up with the next bowl. Independent of the
 *  buffer beside it by design -- deriving one from the other would show an
 *  agreement the hardware never promises. */
function counterAt(dev, at) {
  const period = 300 + (dev.i % 5) * 60;            // 5..9 min a vessel
  const t = at / 1000 + dev.i * 53;
  const c = Math.floor(t / period);
  return Math.round(fullBowlG(dev, c, 0) * (1 - 0.9 * (t - c * period) / period));
}

function batteryOf(dev) {
  if (dev.device_id === NO_BATTERY) return [null, null];
  if (dev.device_id === CRITICAL_B) return [3320, 'critical'];
  if (dev.device_id === LOW_B) return [3560, 'low'];
  return [4020, 'good'];
}

/** The weight half of one device's row at an instant, in the columns its
 *  firmware sends.
 *
 *  THE STATE IS ALWAYS KNOWN AND THE NUMBERS ARE NOT -- weight, gross and
 *  bowls exist exactly when the state is 'ok', mirroring the CHECKs on
 *  device_status so the demo cannot render a row the database would reject.
 *  A scale never carries the buffer's three columns. */
function reading(dev, at) {
  const none = { weight_g: null, gross_g: null, bowls: null, bowls_confirmed: null };
  if (dev.kind === 'scale') {
    if (dev.device_id === SCALE_UNCALIBRATED) {
      return { ...none, weight_state: 'uncalibrated', cells_online: 3,
               counts_per_gram: null, net_counts: 1998226 };
    }
    let g = counterAt(dev, at);
    if (dev.device_id === SCALE_MISCALIBRATED) g = Math.round(g * SCALE_MISCAL_FACTOR);
    return { ...none, weight_state: 'ok', weight_g: g, cells_online: 3,
             counts_per_gram: COUNTER_CPG, net_counts: Math.round(g * COUNTER_CPG) };
  }
  // net_counts is NULL, not 0, while there are no samples: 0 is a reading.
  if (dev.device_id === NO_CELLS) {
    return { ...none, weight_state: 'no_cells', cells_online: 0,
             counts_per_gram: BUFFER_CPG, net_counts: null };
  }
  if (dev.device_id === SETTLING) {
    return { ...none, weight_state: 'settling', cells_online: 1,
             counts_per_gram: BUFFER_CPG, net_counts: null };
  }
  // The offline unit stopped twenty minutes before the demo opened, and its
  // figure stops with it.
  let [bowls, food] = dev.device_id === EMPTY ? [0, 0]
    : bufferAt(dev, dev.device_id === OFFLINE ? Math.min(at, START - 20 * 60_000) : at);
  // The unconfirmed unit's "?" must be on screen whenever the demo is opened, and
  // an empty platform confirms zero -- so it never shows its empty period: it
  // shows the last bowl of the one before (found a minute after midnight, when
  // the cycle put it there and the seeded-state smoke check failed).
  if (dev.device_id === UNCONFIRMED && bowls === 0) {
    [bowls, food] = bufferAt(dev, at - bufferPeriodS(dev) * 1000);
  }
  const gross = food + bowls * BOWL_DRY_G;
  return {
    weight_state: 'ok', weight_g: food, gross_g: gross, bowls,
    // An empty platform confirms a count of zero, so "unconfirmed" needs bowls.
    bowls_confirmed: !(dev.device_id === UNCONFIRMED && bowls > 0),
    cells_online: 1, counts_per_gram: BUFFER_CPG,
    net_counts: Math.round(gross * BUFFER_CPG),
  };
}

function isReported(dev) {
  const deployed = dev.location !== 'R' && dev.food_slot != null;
  return dev.kind === 'scale'
    ? deployed && SCALE_SLOTS_LIVE.has(dev.food_slot) && !SCALE_NOT_FITTED.has(dev.device_id)
    : deployed && !BACKUPS.has(dev.device_id);
}

// --- device_overview --------------------------------------------------

function deviceOverview(at = Date.now()) {
  const meal = currentMeal();
  const date = today();
  return devices.map(dev => {
    const base = {
      device_id: dev.device_id, location: dev.location, food_slot: dev.food_slot,
      label: dev.label, timezone: TZ, kind: dev.kind, current_meal: meal,
      in_service: true, data_is_stale: false, missed_last_service: false,
      // Neither product writes the stack half of the row any more.
      stack_count: null, stack_status: null, levels: null, sensors_online: null,
      // Unreadable on the panel -- the ETA6098's STAT pin reaches no GPIO --
      // so null, never false. See include/board_waveshare_s3.h section 5.
      charging: null,
    };
    if (!isReported(dev)) {
      return {
        ...base, current_food: null,
        reported: false, updated_at: null, stale_for: null,
        offline: false, awaiting_deployment: true,
        battery_mv: null, battery_level: null,
        uptime_s: null, firmware: null, mac: null,
        weight_g: null, weight_state: null, cells_online: null,
        counts_per_gram: null, net_counts: null,
        bowls: null, bowls_confirmed: null, gross_g: null,
      };
    }
    const offline = dev.device_id === OFFLINE;
    const dish = menu.get(`${dev.location}|${date}|${meal}|${dev.food_slot}`);
    const [battery_mv, battery_level] = batteryOf(dev);
    return {
      ...base,
      current_food: dish ? dish.food_name : null,
      reported: true,
      updated_at: iso(at - (offline ? 20 * 60_000 : 20_000)),
      stale_for: offline ? '00:20:00' : '00:00:20',
      offline, awaiting_deployment: false,
      battery_mv, battery_level,
      uptime_s: 3120 + Math.floor(elapsed()),
      firmware: dev.firmware, mac: dev.mac,
      ...reading(dev, at),
    };
  });
}

// --- the aggregates, mirroring the SQL --------------------------------

/** NULL only when both are; otherwise each term coalesced -- the rule every
 *  view applies to buffer + counter, because 0 and unknown are not the same. */
const sum2 = (a, b) => (a == null && b == null) ? null : (a || 0) + (b || 0);
const sumOf = xs => {
  const v = xs.filter(x => x != null);
  return v.length ? v.reduce((n, x) => n + x, 0) : null;
};

/** One bucket per (location, food_slot), the shape slot_overview groups on
 *  and the shape slot_quantity's `per_area` CTE builds before its second
 *  pass. Both views below are computed from this, so they cannot diverge. */
function perArea(rows) {
  const out = new Map();
  const date = today();
  const meal = currentMeal();
  for (const r of rows) {
    if (r.location == null || r.food_slot == null) continue;
    if (!['D', 'M', 'T'].includes(r.location)) continue;
    const k = `${r.location}|${r.food_slot}`;
    if (!out.has(k)) {
      const dish = menu.get(`${r.location}|${date}|${meal}|${r.food_slot}`);
      out.set(k, {
        location: r.location, food_slot: r.food_slot,
        food_name: dish ? dish.food_name : null,
        bowl_weight_g: dish && dish.bowl_weight_g != null ? Number(dish.bowl_weight_g) : null,
        devices: 0, devices_reported: 0, any_reported: false,
        bowls_capacity: 0, bowls_trusted: null, bowls_reported: null,
        scales: 0, scales_ok: 0, measured_weight_g: null, scale_issues: [],
        buffers: 0, buffers_ok: 0, buffer_measured_g: null, buffer_bowls: null,
        buffer_unconfirmed: null, buffer_issues: 0, weighed_down: 0,
        any_fault: false, any_degraded: false, any_battery_warn: false,
        any_offline: false, any_missed_service: false, oldest_update: null,
      });
    }
    const a = out.get(k);
    // STACKS ONLY for the device count and the capacity, matching the `filter
    // (where d.kind = 'stack')` the views carry. A scale or a buffer platform
    // is not a bowl counter, and counting it would claim four bowls of
    // capacity that nothing is watching.
    if (r.kind === 'stack') {
      a.devices += 1;
      a.bowls_capacity += 4;
      if (r.reported) a.devices_reported += 1;
      // Trusted only — a degraded count is a lower bound and a discontiguous
      // one is not a count at all. Same filter as the view.
      if (r.stack_status === 'ok' && r.stack_count != null) {
        a.bowls_trusted = (a.bowls_trusted || 0) + Number(r.stack_count);
      }
      if (r.stack_count != null) a.bowls_reported = (a.bowls_reported || 0) + Number(r.stack_count);
      if (r.stack_status === 'discontiguous') a.any_fault = true;
      if (r.stack_status === 'degraded') a.any_degraded = true;
    } else if (r.kind === 'scale') {
      a.scales += 1;
      if (r.weight_state === 'ok' && r.weight_g != null) {
        a.scales_ok += 1;
        a.measured_weight_g = (a.measured_weight_g || 0) + Number(r.weight_g);
      } else if (r.weight_state != null && !a.scale_issues.includes(r.weight_state)) {
        a.scale_issues.push(r.weight_state);
      }
    } else if (r.kind === 'buffer') {
      a.buffers += 1;
      if (r.weight_state === 'ok' && r.weight_g != null) {
        a.buffers_ok += 1;
        a.buffer_measured_g = (a.buffer_measured_g || 0) + Number(r.weight_g);
        a.buffer_bowls = (a.buffer_bowls || 0) + Number(r.bowls);
        a.buffer_unconfirmed = !!a.buffer_unconfirmed || r.bowls_confirmed === false;
      } else {
        a.buffer_issues += 1;
      }
    }
    // REPORTING AND NOT WEIGHING is what makes a figure a lower bound. A unit
    // that has never reported is awaiting deployment, not missing -- see
    // slotStock() in domain.js for why that distinction is load-bearing.
    if (r.kind !== 'stack' && r.reported && r.weight_state !== 'ok') a.weighed_down += 1;
    // NOT filtered by kind: any device that has reported means this position
    // is live, which is what the view's `having bool_or(any_reported)` asks.
    if (r.reported) a.any_reported = true;
    if (['low', 'critical'].includes(r.battery_level)) a.any_battery_warn = true;
    if (r.offline) a.any_offline = true;
    if (r.missed_last_service) a.any_missed_service = true;
    if (r.updated_at && (!a.oldest_update || r.updated_at < a.oldest_update)) {
      a.oldest_update = r.updated_at;
    }
  }
  return [...out.values()];
}

/** The three weight terms of one hall's position. The buffer is what its
 *  platforms weigh PLUS the old bowls x kg/bowl estimate from any stack still
 *  standing (none here -- the term is dormant after the cut-over). NULL, never
 *  0, for a term nobody measured. */
function terms(a) {
  const stack = a.bowls_trusted == null || a.bowl_weight_g == null
    ? null : a.bowls_trusted * a.bowl_weight_g;
  const buffer_g = sum2(a.buffer_measured_g, stack);
  return { buffer_g, counter_g: a.measured_weight_g,
           weight_g: sum2(buffer_g, a.measured_weight_g) };
}

function slotOverview(rows) {
  const meal = currentMeal();
  return perArea(rows).map(a => {
    const t = terms(a);
    return {
      location: a.location, food_slot: a.food_slot,
      current_food: a.food_name, current_meal: meal,
      devices: a.devices, devices_reported: a.devices_reported,
      bowls_capacity: a.bowls_capacity,
      bowls_trusted: a.bowls_trusted, bowls_reported: a.bowls_reported,
      any_fault: a.any_fault, any_degraded: a.any_degraded,
      any_battery_warn: a.any_battery_warn, any_offline: a.any_offline,
      any_missed_service: a.any_missed_service, oldest_update: a.oldest_update,
      scales: a.scales, scales_ok: a.scales_ok,
      measured_weight_g: a.measured_weight_g, scale_issues: a.scale_issues,
      bowl_weight_g: a.bowl_weight_g, buffer_g: t.buffer_g, weight_g: t.weight_g,
      buffers: a.buffers, buffers_ok: a.buffers_ok,
      buffer_measured_g: a.buffer_measured_g, buffer_bowls: a.buffer_bowls,
      buffer_unconfirmed: a.buffer_unconfirmed, buffer_issues: a.buffer_issues,
    };
  }).sort((x, y) => x.location.localeCompare(y.location) || x.food_slot - y.food_slot);
}

/** slot_quantity: the second pass, grouped by food_slot across every hall.
 *  Mirrors the view line for line — including the sum-then-multiply for any
 *  stack still standing, the "only when every hall agrees" per-bowl weight,
 *  and the has-ever-reported gate that keeps the backups off this screen. */
function slotQuantityRows(rows) {
  const meal = currentMeal();
  const bySlot = new Map();
  for (const a of perArea(rows)) {
    if (!bySlot.has(a.food_slot)) bySlot.set(a.food_slot, []);
    bySlot.get(a.food_slot).push(a);
  }

  const out = [];
  for (const [food_slot, areas] of [...bySlot.entries()].sort((x, y) => x[0] - y[0])) {
    // The gate. A slot earns a row once one of its devices has ever spoken.
    if (!areas.some(a => a.any_reported)) continue;

    const weighed = areas.filter(a => a.bowl_weight_g != null);
    const distinctW = [...new Set(weighed.map(a => a.bowl_weight_g))];
    const trustedVals = areas.filter(a => a.bowls_trusted != null);
    const dishes = [...new Set(areas.map(a => a.food_name).filter(Boolean))].sort();

    // TWO estimates per hall, not one, and the difference is not redundancy:
    // the SLOT total treats a silent stack hall as contributing nothing while
    // the hall's own LINE shows a dash, because "no reading" is not "an empty
    // counter". Only halls that still HAVE a stack estimate anything -- with
    // a per-bowl weight typed and no stack left, the estimate is not 0, it
    // does not exist.
    const estTotal = a => a.devices === 0 || a.bowl_weight_g == null
      ? null : (a.bowls_trusted || 0) * a.bowl_weight_g;
    const estLine = a => a.bowl_weight_g == null || a.bowls_trusted == null
      ? null : a.bowls_trusted * a.bowl_weight_g;
    // BUFFER PLUS COUNTER. Food waiting behind the line and food already out
    // on the counter are different food in different places, so a hall's mass
    // is the SUM of the two, and NULL only when it has neither.
    const bufTotal = a => sum2(a.buffer_measured_g, estTotal(a));
    const bufLine = a => sum2(a.buffer_measured_g, estLine(a));
    const wTotal = a => sum2(bufTotal(a), a.measured_weight_g);

    const buffer_g = sumOf(areas.map(bufTotal));
    const counter_g = sumOf(areas.map(a => a.measured_weight_g));
    const weight_g = sum2(buffer_g, counter_g);

    // THREE conditions, matching the view. The third -- a dish is actually
    // set here -- is the one that was missing: breakfast runs at Darshanarthi
    // only, so Mahatma held a bowl against no dish and the slot claimed
    // ">=36.4 kg" with a Set weight link pointing at a hall with nothing to
    // set a weight for.
    const unweighed = areas.filter(a => wTotal(a) == null
      && (a.bowls_trusted || 0) > 0 && a.food_name != null);
    const est_is_partial = unweighed.length > 0;

    out.push({
      food_slot,
      current_meal: meal,
      // Demo mode forces service on, so the resolved menu is always the
      // running meal. The between-meals path is exercised by the smoke suite.
      menu_meal_type: meal, menu_meal_date: today(), menu_is_live: true,
      dishes: dishes.length ? dishes : null,
      bowl_weight_g: distinctW.length === 1 ? distinctW[0] : null,
      devices: areas.reduce((n, a) => n + a.devices, 0),
      bowls_capacity: areas.reduce((n, a) => n + a.bowls_capacity, 0),
      bowls_trusted: trustedVals.length
        ? trustedVals.reduce((n, a) => n + a.bowls_trusted, 0) : null,
      est_weight_g: sumOf(areas.map(estTotal)),
      capacity_weight_g: weighed.length
        ? weighed.reduce((n, a) => n + a.bowls_capacity * a.bowl_weight_g, 0) : null,
      est_is_partial,
      areas_without_weight: unweighed.map(a => a.location),
      areas: areas.map(a => ({
        location: a.location, food_name: a.food_name,
        bowl_weight_g: a.bowl_weight_g, bowls_trusted: a.bowls_trusted,
        bowls_capacity: a.bowls_capacity, devices: a.devices,
        // The hall's own mass: its buffer PLUS what its counter weighs.
        // Still NULL, never 0, when it has neither -- 0 beside a dash is a
        // contradiction that sends somebody to refill a full station.
        weight_g: sum2(bufLine(a), a.measured_weight_g),
        est_weight_g: estLine(a),
        measured_weight_g: a.measured_weight_g,
        scales: a.scales,
        capacity_weight_g: a.bowl_weight_g == null
          ? null : a.bowls_capacity * a.bowl_weight_g,
        buffers: a.buffers, buffers_ok: a.buffers_ok, buffer_g: bufLine(a),
        buffer_bowls: a.buffer_bowls, buffer_unconfirmed: a.buffer_unconfirmed,
      })).sort((x, y) => x.location.localeCompare(y.location)),
      scales: areas.reduce((n, a) => n + a.scales, 0),
      scales_ok: areas.reduce((n, a) => n + a.scales_ok, 0),
      measured_weight_g: counter_g, weight_g, buffer_g, counter_g,
      scale_issues: [...new Set(areas.flatMap(a => a.scale_issues))].sort(),
      buffers: areas.reduce((n, a) => n + a.buffers, 0),
      buffers_ok: areas.reduce((n, a) => n + a.buffers_ok, 0),
      buffer_measured_g: sumOf(areas.map(a => a.buffer_measured_g)),
      buffer_bowls: sumOf(areas.map(a => a.buffer_bowls)),
      buffer_unconfirmed: areas.some(a => a.buffer_unconfirmed)
        || (areas.some(a => a.buffer_unconfirmed === false) ? false : null),
      // A figure with a buffer or scale down beside it is a lower bound -- and
      // there is no bound without a figure.
      weight_is_partial: weight_g != null
        && (areas.some(a => a.weighed_down > 0) || est_is_partial),
      any_fault: areas.some(a => a.any_fault),
      any_degraded: areas.some(a => a.any_degraded),
      any_battery_warn: areas.some(a => a.any_battery_warn),
      any_offline: areas.some(a => a.any_offline),
      any_missed_service: areas.some(a => a.any_missed_service),
      oldest_update: areas.map(a => a.oldest_update).filter(Boolean).sort()[0] || null,
    });
  }
  return out;
}

// --- history, for the device page and Master's curve ------------------

/** weight_samples for one scale or buffer, two minutes apart and newest
 *  first -- the firmware's cadence when nothing moves -- from the same model
 *  as the live row, so the curve ends where the card's figure is. */
function weightSamples(deviceId) {
  const dev = devices.find(d => d.device_id === deviceId);
  if (!dev || !isReported(dev)) return [];
  const [battery_mv, battery_level] = batteryOf(dev);
  // An offline unit's history stops where it went quiet.
  const last = Date.now() - (deviceId === OFFLINE ? 20 * 60_000 : 0);
  const out = [];
  for (let k = 0; k < 1000; k++) {
    const at = last - k * 120_000;
    out.push({
      device_id: deviceId, boot_id: 7, seq: 1000 - k,
      recorded_at: iso(at), received_at: iso(at + 400),
      reason: k === 999 ? 'boot' : 'periodic',
      battery_mv, battery_level, firmware: dev.firmware, manual_fill_pct: null,
      ...reading(dev, at),
    });
  }
  return out;
}

/** slot_stock_series: each hall's level per slot every five minutes over the
 *  last hour, from the model at each instant -- so the newest reading counts
 *  in whatever state it was in, and a weight only when that state was ok. */
function stockSeries() {
  const now = Date.now();
  const out = [];
  for (let k = 12; k >= 0; k--) {
    const at = now - k * 300_000;
    for (const a of perArea(deviceOverview(at))) {
      if (!a.any_reported) continue;
      const t = terms(a);
      out.push({ location: a.location, food_slot: a.food_slot, at_ts: iso(at),
                 buffer_g: t.buffer_g, counter_g: t.counter_g, total_g: t.weight_g });
    }
  }
  return out;
}

/** slot_burn_rate, one row per hall per slot, from the series above.
 *  ponytail: sums the falls between points, which the real view abandoned as
 *  biased (docs/FRONTEND_HANDOFF.md §8) -- it is a demo of the card, not of
 *  the estimator. Reloads are rises, so they drop out rather than read as a
 *  negative rate. */
function burnRate() {
  const series = stockSeries();
  const now = Date.now();
  const latest = new Map(perArea(deviceOverview()).map(a => [`${a.location}|${a.food_slot}`, a]));
  const out = [];
  for (const [key, a] of latest) {
    const pts = series.filter(p => `${p.location}|${p.food_slot}` === key && p.total_g != null);
    if (pts.length < 2) continue;
    let fell = 0;
    for (let i = 1; i < pts.length; i++) fell += Math.max(0, pts[i - 1].total_g - pts[i].total_g);
    const last = pts[pts.length - 1];
    const hours = (Date.parse(last.at_ts) - Date.parse(pts[0].at_ts)) / 3600_000;
    const g_per_hour = Math.round(fell / hours);
    out.push({
      location: a.location, food_slot: a.food_slot,
      total_g: last.total_g, buffer_g: last.buffer_g, counter_g: last.counter_g,
      g_per_hour, covered: '01:00:00',
      runs_out_at: g_per_hour > 0 ? iso(now + (last.total_g / g_per_hour) * 3600_000) : null,
      is_partial: a.weighed_down > 0,
      short_before_close: null,
    });
  }
  return out.sort((x, y) => x.food_slot - y.food_slot);
}

// --- the PostgREST surface --------------------------------------------

function applyFilters(data, filters) {
  let out = data;
  for (const [op, col, val] of filters) {
    if (op === 'eq') out = out.filter(r => String(r[col]) === String(val));
    else if (op === 'in') out = out.filter(r => (val || []).map(String).includes(String(r[col])));
    else if (op === 'gte') out = out.filter(r => r[col] >= val);
    else if (op === 'lte') out = out.filter(r => r[col] <= val);
    else if (op === 'limit') out = out.slice(0, col);
  }
  return out;
}

// TRIAL HARNESS. Demo mode gets the manual estimate too, or the card that
// compares it against the scale is code the preview can never render -- and a
// dashboard feature nobody can see without live hardware is one that quietly
// stops working. The estimate is deliberately WRONG by a couple of kilograms,
// because a demo showing perfect agreement would misrepresent the very thing
// the experiment exists to measure.
const trialCap = new Map([['Breakfast', 18000], ['Lunch', 18000], ['Dinner', 18000]]);

function table(name) {
  const rec = { filters: [], writes: [] };
  const api = {};
  for (const m of ['select', 'order', 'limit']) {
    api[m] = (...args) => { rec.filters.push([m, ...args]); return api; };
  }
  for (const m of ['eq', 'gte', 'lte', 'in']) {
    api[m] = (...args) => { rec.filters.push([m, ...args]); return api; };
  }
  api.upsert = (payload) => { rec.writes.push(['upsert', payload]); return api; };
  api.update = (payload) => { rec.writes.push(['update', payload]); return api; };
  api.delete = () => { rec.writes.push(['delete', null]); return api; };

  api.then = (res, rej) => {
    let data = [];
    try {
      // Writes first: a delete or upsert carries filters that describe the
      // rows it touches, not rows to return.
      for (const [op, payload] of rec.writes) {
        if (name === 'meal_food_mapping') {
          if (op === 'upsert') for (const r of [].concat(payload)) menu.set(mkey(r), { ...r });
          if (op === 'delete') {
            for (const [k, r] of [...menu]) {
              if (applyFilters([r], rec.filters).length) menu.delete(k);
            }
          }
        } else if (name === 'meal_menu_template') {
          if (op === 'upsert') for (const r of [].concat(payload)) template.set(tkey(r), { ...r });
          if (op === 'delete') {
            for (const [k, r] of [...template]) {
              if (applyFilters([r], rec.filters).length) template.delete(k);
            }
          }
        } else if (name === 'devices' && op === 'update') {
          for (const d of applyFilters(devices, rec.filters)) Object.assign(d, payload);
        }
      }
      if (rec.writes.length) return Promise.resolve({ data: [], error: null }).then(res, rej);

      const overview = deviceOverview();
      // TRIAL HARNESS -- writes first, same as the tables above.
      for (const [op, payload] of rec.writes) {
        if (name === 'trial_vessel_capacity' && op === 'upsert') {
          for (const r of [].concat(payload)) trialCap.set(r.meal_type, r.capacity_g);
        }
      }
      // The overview rows carry no `i` (the view has no such column), so the
      // alternation keys on the id's number; on `d.i` it was NaN and every
      // unit read "on battery".
      const nOf = d => Number(d.device_id.slice(4));
      if (name === 'device_power') {
        // Alternating, so the preview shows both "on mains" and "on battery"
        // rather than only whichever the fixture happened to pick.
        data = overview.map(d => ({ device_id: d.device_id,
                                    external_power: (nOf(d) % 2) === 0 }));
      } else if (name === 'trial_vessel_capacity') {
        data = [...trialCap].map(([meal_type, capacity_g]) => ({ meal_type, capacity_g }));
      } else if (name === 'trial_manual_fill') {
        data = overview.filter(d => d.kind === 'scale' && d.weight_state === 'ok')
          .map(d => ({
            device_id: d.device_id, location: d.location, food_slot: d.food_slot,
            // Off by roughly a tenth, alternating sign per device, so the card's
            // signed error shows both directions in the preview.
            manual_fill_pct: Math.max(0, Math.min(100, Math.round(
              (d.weight_g / 18000) * 100 + (nOf(d) % 2 ? 8 : -6)))),
            manual_fill_age_s: (nOf(d) % 3) * 420,
            weight_state: d.weight_state, weight_g: d.weight_g,
            updated_at: d.updated_at,
          }));
      }
      else if (name === 'device_overview') data = overview;
      else if (name === 'slot_overview') data = slotOverview(overview);
      else if (name === 'slot_quantity') data = slotQuantityRows(overview);
      else if (name === 'slot_stock_series') data = stockSeries();
      else if (name === 'slot_burn_rate') data = burnRate();
      else if (name === 'meal_menu_template') data = [...template.values()];
      else if (name === 'meal_food_mapping') data = [...menu.values()];
      else if (name === 'devices') data = devices;
      else if (name === 'weight_samples') {
        const idf = rec.filters.find(f => f[0] === 'eq' && f[1] === 'device_id');
        data = idf ? weightSamples(idf[2]) : [];
      }
      // status_events: no stack is left to have any.
      data = applyFilters(data, rec.filters);
    } catch (err) {
      return Promise.resolve({ data: null, error: { message: err.message } }).then(res, rej);
    }
    return Promise.resolve({ data, error: null }).then(res, rej);
  };
  return api;
}

function rpc(name, args = {}) {
  if (name === 'meal_mapping_preload') {
    const { p_location, p_meal_type, p_meal_date } = args;
    const exact = [...menu.values()].filter(r => r.location === p_location
      && r.meal_type === p_meal_type && r.meal_date === p_meal_date);
    if (exact.length) {
      return Promise.resolve({
        data: exact.map(r => ({
          food_slot: r.food_slot, food_name: r.food_name,
          bowl_weight_g: r.bowl_weight_g ?? null,
          source_date: r.meal_date, is_saved: true,
        })).sort((a, b) => a.food_slot - b.food_slot),
        error: null,
      });
    }
    // Nothing saved for that date: fall back to the weekly template, flagged
    // as a DRAFT. The flag is the whole point — a preloaded form is
    // pixel-identical to a saved one.
    const weekday = new Date(`${p_meal_date}T12:00:00Z`).getUTCDay();
    const tpl = [...template.values()].filter(r => r.location === p_location
      && r.meal_type === p_meal_type && r.weekday === weekday);
    return Promise.resolve({
      data: tpl.map(r => ({
        food_slot: r.food_slot, food_name: r.food_name,
        bowl_weight_g: r.bowl_weight_g ?? null,
        source_date: p_meal_date, is_saved: false,
      })).sort((a, b) => a.food_slot - b.food_slot),
      error: null,
    });
  }

  if (name === 'meal_template_apply') {
    const { p_location, p_from, p_to, p_overwrite } = args;
    const out = [];
    for (let d = p_from; d <= p_to; d = addDay(d)) {
      const weekday = new Date(`${d}T12:00:00Z`).getUTCDay();
      for (const meal_type of MEALS) {
        const tpl = [...template.values()].filter(r => r.location === p_location
          && r.meal_type === meal_type && r.weekday === weekday);
        if (!tpl.length) continue;
        const had = [...menu.values()].some(r => r.location === p_location
          && r.meal_date === d && r.meal_type === meal_type);
        if (had && !p_overwrite) {
          out.push({ meal_date: d, meal_type, written: 0, skipped: true });
          continue;
        }
        for (const r of tpl) {
          const row = {
            location: p_location, meal_type, meal_date: d,
            food_slot: r.food_slot, food_name: r.food_name,
            bowl_weight_g: r.bowl_weight_g ?? null,
          };
          menu.set(mkey(row), row);
        }
        out.push({ meal_date: d, meal_type, written: tpl.length, skipped: false });
      }
    }
    return Promise.resolve({ data: out, error: null });
  }

  return Promise.resolve({ data: [], error: null });
}

function addDay(dateStr) {
  const d = new Date(`${dateStr}T12:00:00Z`);
  d.setUTCDate(d.getUTCDate() + 1);
  return d.toISOString().slice(0, 10);
}

/** A stand-in for the Supabase client, covering only what this app calls.
 *  Auth always succeeds: demo mode has no gate to get past. */
export function createMockClient() {
  const user = { id: 'demo', email: null, is_anonymous: true };
  const session = { user, access_token: 'demo' };
  return {
    from: table,
    rpc,
    auth: {
      getSession: () => Promise.resolve({ data: { session }, error: null }),
      getUser: () => Promise.resolve({ data: { user }, error: null }),
      signInAnonymously: () => Promise.resolve({ data: { session }, error: null }),
      signInWithPassword: () => Promise.resolve({ data: { session }, error: null }),
      signOut: () => Promise.resolve({ error: null }),
      onAuthStateChange: () => ({ data: { subscription: { unsubscribe() {} } } }),
    },
  };
}
