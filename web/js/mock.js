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
//  That mirroring is also why the menu store is WRITABLE: type a per-bowl
//  weight on the Menu tab in demo mode, press Save, and the Master total
//  moves. The editing path is the part most worth trying before it is wired
//  to a real database.
//
//  IT DRIFTS
//  ---------
//  Bowl counts fall with the wall clock, each stack at its own rate, and
//  wrap back to full when a stack empties. So a poll visibly changes the
//  screen, which is the one behaviour a static seed cannot show.
//
//  ALWAYS IN SERVICE
//  -----------------
//  Devices are dark ~16 h a day by design, so a demo opened at 3pm would
//  correctly render "outside service hours", freeze, and drop the poll to
//  its 10-minute idle cadence — an accurate screen that demonstrates
//  nothing. Demo mode therefore forces in_service, and the banner at the
//  top of the app says the data is not real.
// ====================================================================

import { MAX_BOWLS, SERVICE_WINDOWS, DEFAULT_TZ } from './domain.js';

/** Demo mode is opt-in per URL and never sticky. It must not be reachable by
 *  accident on a kitchen tablet: a screen showing invented bowl counts that
 *  someone believes is stock is worse than a screen showing nothing. */
export function isMockMode() {
  try {
    return new URLSearchParams(location.search).get('mock') === '1';
  } catch { return false; }
}

const START = Date.now();
const TZ = DEFAULT_TZ;

// --- the fleet, exactly as supabase/assign_devices.sql states it -------
//
// Twenty deployed stacks over four dish positions: Darshanarthi runs three
// counters per position, Mahatma and Tiffin one each. BWL-021..024 are the
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

// One device seeded into each state the UI has a rule for, so demo mode
// exercises every branch rather than twenty healthy stacks.
const FAULT = 'BWL-002';      // discontiguous — slot 1 carries a fault
const DEGRADED = 'BWL-008';   // a dead sensor — slot 3's count is a bound
const OFFLINE = 'BWL-014';    // died mid-service — slot 2 keeps its last value
const NO_BATTERY = 'BWL-019';
const CRITICAL_B = 'BWL-005';
const LOW_B = 'BWL-011';

const AREA_LABEL = { D: 'Darshanarthi', M: 'Mahatma', T: 'Tiffin', R: 'Reserved' };

const devices = ASSIGN.map(([device_id, location, food_slot], i) => ({
  device_id, location, food_slot, i, kind: 'stack',
  label: `${AREA_LABEL[location]} slot ${food_slot}`,
  timezone: TZ,
  firmware: '0.2.0',
  mac: `A0:B1:C2:D3:E4:${String(0x10 + i).toString(16).toUpperCase()}`,
}));
for (let n = 25; n <= 32; n++) {
  devices.push({
    device_id: `BWL-0${n}`, location: 'R', food_slot: null, i: n, kind: 'stack',
    label: 'Reserved', timezone: TZ, firmware: null, mac: null,
  });
}

// --- the load cells, mirroring the stacks -----------------------------
//
// LDC-nnn sits at the same physical position as BWL-nnn -- the rule
// supabase/assign_loadcells.sql states and derives rather than repeats. The
// platform is UNDER the stack, so the two are one installation seen two ways:
// one counts what is buffered, the other weighs what is actually there.
//
// EVERY scale is registered and assigned, and only SOME have reported. That is
// not a shortcut, it is the rollout: slot 3's cells are installed on paper and
// not yet powered, exactly as BWL-021..024 are. It is also what keeps the
// interesting cases on screen -- with every hall weighed, `estimated` and
// `mixed` would never render, and slot 3's "Set weight ›" case, which the
// naive sum-then-multiply formula gets wrong, would disappear from the demo.
const SCALE_SLOTS_LIVE = new Set([1, 2, 4]);

// Deliberately awkward, one per rule the UI has:
//   D/2 alone is weighed  -> that slot reads `mixed`
//   T/4 reads badly heavy -> the mismatch chip, with a direction and a hall
//   T/1 never calibrated  -> contributes NOTHING, and says why
//
// TIFFIN, NOT DARSHANARTHI, and the reason is a real limit rather than a
// preference. slot_quantity compares measured against estimated PER AREA, which
// is the finest grouping that view has -- so at Darshanarthi, which runs three
// stacks and three scales at every position, one bad cell of three is a third
// of a third and needs to be ~75% out before it clears the 25% band. Tiffin has
// one of each, so per-area IS per-device there and the fault shows at its true
// size. Putting the demo's bad cell at D would have shown a detection this
// design cannot actually make.
//
// 2.2x rather than something subtler because the bowl count drains to zero on a
// sawtooth: at 1.35x the absolute 2000 g band stops clearing once the stack is
// down to a bowl or two, and the chip would blink on and off as the demo runs.
const SCALE_MISCALIBRATED = 'LDC-020';   // T slot 4, one stack, one scale
const SCALE_MISCAL_FACTOR = 2.2;
const SCALE_UNCALIBRATED = 'LDC-017';    // T slot 1
// Slot 2's cells at Mahatma and Tiffin are not fitted, so that position mixes
// one measured hall with two estimated ones.
const SCALE_NOT_FITTED = new Set(['LDC-014', 'LDC-018']);

for (const [stackId, location, food_slot] of ASSIGN) {
  const n = stackId.slice(4);
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
// Slot 1, 2 and 4 carry one dish across all three halls — the normal case,
// and the one the Master total is built for. Slot 3 deliberately diverges
// AND leaves Tiffin unweighed, because those are the two cases the naive
// formula gets wrong: multiplying a site-wide bowl count by a single weight
// would silently price Tiffin's Bhaji as Darshanarthi's Curry, and treating
// a missing weight as zero would under-report the hall entirely. On screen
// that slot must read as a lower bound with a "Set weight" link.
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

/** Bowls left on one stack. A sawtooth per device, each with its own period,
 *  so the fleet drains unevenly the way a real service does rather than
 *  stepping down in lockstep. */
function liveCount(dev) {
  if (dev.device_id === OFFLINE) return 2;   // frozen: it stopped reporting
  const period = 18 + (dev.i % 7) * 4;       // 18..42 s per bowl
  const phase = Math.floor(elapsed() / period);
  return MAX_BOWLS - (phase % (MAX_BOWLS + 1));
}

function levelsFor(dev, count) {
  if (dev.device_id === FAULT) return ['absent', 'present', 'absent', 'absent'];
  if (dev.device_id === DEGRADED) return ['present', 'present', 'unknown', 'absent'];
  return Array.from({ length: MAX_BOWLS }, (_, i) => (i < count ? 'present' : 'absent'));
}

function statusOf(dev) {
  if (dev.device_id === FAULT) return 'discontiguous';
  if (dev.device_id === DEGRADED) return 'degraded';
  return 'ok';
}

function batteryOf(dev) {
  if (dev.device_id === NO_BATTERY) return [null, null];
  if (dev.device_id === CRITICAL_B) return [3320, 'critical'];
  if (dev.device_id === LOW_B) return [3560, 'low'];
  return [4020, 'good'];
}

/** What one load cell is reporting: [weight_state, weight_g].
 *
 *  THE STATE IS ALWAYS KNOWN AND THE NUMBER IS NOT -- the same inversion the
 *  schema enforces with device_status_weight_agrees_ck, mirrored here so the
 *  demo cannot render a combination the database would reject. Grams exist
 *  exactly when the state is 'ok'.
 *
 *  The weight tracks the bowl count on the same stack, so the two figures move
 *  together the way they would on a real counter -- a demo where the kilograms
 *  and the bowls drift apart would make the mismatch chip meaningless. */
function weightOf(dev, bowlWeightG, bowls) {
  if (dev.device_id === SCALE_UNCALIBRATED) return ['uncalibrated', null];
  if (bowlWeightG == null || bowls == null) {
    // Nothing to derive a plausible mass from. Report a real state rather than
    // a fabricated weight -- an empty platform is 0 g, and that is a claim.
    return ['no_cells', null];
  }
  const truth = bowls * Number(bowlWeightG);
  if (dev.device_id === SCALE_MISCALIBRATED) {
    // Past both bands of weight_mismatch_tolerance(), so the chip fires. A
    // miscalibrated cell is one of exactly two things a disagreement can mean,
    // and the demo should show one of them -- the other, a wrong per-bowl
    // weight on the Menu tab, is reachable by editing one and watching this
    // same chip appear.
    return ['ok', Math.round(truth * SCALE_MISCAL_FACTOR)];
  }
  // A few hundred grams of realistic slop, deterministic per device so the
  // screen is stable between polls. Well inside the tolerance, so an honest
  // scale never trips the chip.
  return ['ok', Math.round(truth + ((dev.i * 137) % 400) - 200)];
}

// --- device_overview --------------------------------------------------

function deviceOverview() {
  const meal = currentMeal();
  const date = today();
  const now = Date.now();
  return devices.map(dev => {
    const deployed = dev.location !== 'R' && dev.food_slot != null;
    const isScale = dev.kind === 'scale';
    const reported = isScale
      ? deployed && SCALE_SLOTS_LIVE.has(dev.food_slot)
                 && !SCALE_NOT_FITTED.has(dev.device_id)
      : deployed && !BACKUPS.has(dev.device_id);
    if (!reported) {
      return {
        device_id: dev.device_id, location: dev.location, food_slot: dev.food_slot,
        label: dev.label, timezone: TZ, kind: dev.kind,
        current_food: null, current_meal: meal,
        reported: false, updated_at: null, stale_for: null,
        in_service: true, offline: false, awaiting_deployment: true,
        data_is_stale: false, missed_last_service: false,
        stack_count: null, stack_status: null, levels: null,
        sensors_online: null, battery_mv: null, battery_level: null,
        charging: null, uptime_s: null, firmware: null, mac: null,
        weight_g: null, weight_state: null, cells_online: null,
        counts_per_gram: null, net_counts: null,
      };
    }

    // A SCALE FILLS THE OTHER HALF OF THE SAME ROW. Every stack_* column stays
    // NULL -- which is what keeps it out of slot_overview's bowl arithmetic --
    // and devices.kind is what tells the dashboard which half to read.
    if (isScale) {
      const dish = menu.get(`${dev.location}|${date}|${meal}|${dev.food_slot}`);
      // The bowls physically on this platform: the paired stack's count.
      const pairedStack = devices.find(d => d.device_id === `BWL-${dev.device_id.slice(4)}`);
      const bowls = pairedStack ? liveCount(pairedStack) : null;
      const [weight_state, weight_g] =
        weightOf(dev, dish ? dish.bowl_weight_g : null, bowls);
      const [battery_mv, battery_level] = batteryOf(dev);
      return {
        device_id: dev.device_id, location: dev.location, food_slot: dev.food_slot,
        label: dev.label, timezone: TZ, kind: 'scale',
        current_food: dish ? dish.food_name : null,
        current_meal: meal,
        reported: true,
        updated_at: new Date(now - 20_000).toISOString(),
        stale_for: '00:00:20',
        in_service: true, offline: false, awaiting_deployment: false,
        data_is_stale: false, missed_last_service: false,
        stack_count: null, stack_status: null, levels: null, sensors_online: null,
        battery_mv, battery_level,
        // Unreadable on this board -- the ETA6098's STAT pin reaches no GPIO --
        // so null, never false. See include/board_waveshare_s3.h section 5.
        charging: null,
        uptime_s: 3120 + Math.floor(elapsed()),
        firmware: dev.firmware, mac: dev.mac,
        weight_g, weight_state,
        cells_online: weight_state === 'no_cells' ? 0 : 3,
        counts_per_gram: weight_state === 'uncalibrated' ? null : 106.857,
        net_counts: weight_state === 'no_cells' ? null
          : Math.round((weight_g == null ? 18700 : weight_g) * 0.106857 * 1000),
      };
    }
    const offline = dev.device_id === OFFLINE;
    const count = liveCount(dev);
    const [battery_mv, battery_level] = batteryOf(dev);
    const dish = menu.get(`${dev.location}|${date}|${meal}|${dev.food_slot}`);
    return {
      device_id: dev.device_id, location: dev.location, food_slot: dev.food_slot,
      label: dev.label, timezone: TZ,
      current_food: dish ? dish.food_name : null,
      current_meal: meal,
      reported: true,
      updated_at: new Date(now - (offline ? 20 * 60_000 : 20_000)).toISOString(),
      stale_for: offline ? '00:20:00' : '00:00:20',
      in_service: true,
      offline,
      awaiting_deployment: false,
      data_is_stale: false,
      missed_last_service: false,
      stack_count: count,
      stack_status: statusOf(dev),
      levels: levelsFor(dev, count),
      sensors_online: dev.device_id === DEGRADED ? 3 : 4,
      battery_mv, battery_level,
      charging: dev.device_id === 'BWL-001',
      uptime_s: 7412 + Math.floor(elapsed()),
      firmware: dev.firmware, mac: dev.mac,
      kind: 'stack',
      // A stack leaves the load-cell half NULL, exactly as a scale leaves the
      // stack half NULL. One row shape, two products.
      weight_g: null, weight_state: null, cells_online: null,
      counts_per_gram: null, net_counts: null,
    };
  });
}

// --- the aggregates, mirroring the SQL --------------------------------

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
        any_fault: false, any_degraded: false, any_battery_warn: false,
        any_offline: false, any_missed_service: false, oldest_update: null,
      });
    }
    const a = out.get(k);
    // STACKS ONLY for the device count and the capacity, matching the `filter
    // (where d.kind = 'stack')` the views carry. A load cell bolted at this
    // position is not a fourth bowl counter, and counting it would claim four
    // more bowls of capacity that nothing is watching -- with the numerator
    // still correct, so the bar would simply read low forever.
    if (r.kind !== 'scale') {
      a.devices += 1;
      a.bowls_capacity += MAX_BOWLS;
      if (r.reported) a.devices_reported += 1;
    } else {
      a.scales += 1;
      if (r.weight_state === 'ok' && r.weight_g != null) {
        a.scales_ok += 1;
        a.measured_weight_g = (a.measured_weight_g || 0) + Number(r.weight_g);
      } else if (r.weight_state != null && !a.scale_issues.includes(r.weight_state)) {
        a.scale_issues.push(r.weight_state);
      }
    }
    // NOT filtered by kind: a scale that has reported means this position is
    // live, which is what the view's `having bool_or(any_reported)` gate asks.
    if (r.reported) a.any_reported = true;
    // Trusted only — a degraded count is a lower bound and a discontiguous
    // one is not a count at all. Same filter as the view.
    if (r.stack_status === 'ok' && r.stack_count != null) {
      a.bowls_trusted = (a.bowls_trusted || 0) + Number(r.stack_count);
    }
    if (r.stack_count != null) a.bowls_reported = (a.bowls_reported || 0) + Number(r.stack_count);
    if (r.stack_status === 'discontiguous') a.any_fault = true;
    if (r.stack_status === 'degraded') a.any_degraded = true;
    if (['low', 'critical'].includes(r.battery_level)) a.any_battery_warn = true;
    if (r.offline) a.any_offline = true;
    if (r.missed_last_service) a.any_missed_service = true;
    if (r.updated_at && (!a.oldest_update || r.updated_at < a.oldest_update)) {
      a.oldest_update = r.updated_at;
    }
  }
  return [...out.values()];
}

function slotOverview(rows) {
  const meal = currentMeal();
  return perArea(rows).map(a => ({
    location: a.location, food_slot: a.food_slot,
    current_food: a.food_name, current_meal: meal,
    devices: a.devices, devices_reported: a.devices_reported,
    bowls_capacity: a.bowls_capacity,
    bowls_trusted: a.bowls_trusted, bowls_reported: a.bowls_reported,
    any_fault: a.any_fault, any_degraded: a.any_degraded,
    any_battery_warn: a.any_battery_warn, any_offline: a.any_offline,
    any_missed_service: a.any_missed_service, oldest_update: a.oldest_update,
  })).sort((x, y) => x.location.localeCompare(y.location) || x.food_slot - y.food_slot);
}

/** slot_quantity: the second pass, grouped by food_slot across every hall.
 *  Mirrors the view line for line — including the sum-then-multiply, the
 *  "only when every hall agrees" per-bowl weight, and the has-ever-reported
 *  gate that keeps the backups off this screen. */
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

    const est = weighed.length
      ? weighed.reduce((n, a) => n + (a.bowls_trusted || 0) * a.bowl_weight_g, 0)
      : null;
    const capW = weighed.length
      ? weighed.reduce((n, a) => n + a.bowls_capacity * a.bowl_weight_g, 0)
      : null;

    const trustedVals = areas.filter(a => a.bowls_trusted != null);
    const dishes = [...new Set(areas.map(a => a.food_name).filter(Boolean))].sort();

    // THE PRECEDENCE RULE, PER AREA -- mirroring per_area_w in the view. Two
    // estimates, not one, and the difference is not redundancy: the SLOT total
    // treats a silent hall as contributing nothing while the hall's own LINE
    // shows a dash, because "no reading" is not "an empty counter".
    const estTotal = a => a.bowl_weight_g == null
      ? null : (a.bowls_trusted || 0) * a.bowl_weight_g;
    const estLine = a => a.bowl_weight_g == null || a.bowls_trusted == null
      ? null : a.bowls_trusted * a.bowl_weight_g;
    const wTotal = a => a.measured_weight_g != null ? a.measured_weight_g : estTotal(a);
    const wLine = a => a.measured_weight_g != null ? a.measured_weight_g : estLine(a);
    const srcOf = a => a.measured_weight_g != null ? 'measured'
      : a.bowl_weight_g != null ? 'estimated' : null;

    const sources = areas.map(srcOf).filter(Boolean);
    const weight_source = sources.length === 0 ? null
      : sources.every(s => s === 'measured') ? 'measured'
      : sources.every(s => s === 'estimated') ? 'estimated' : 'mixed';

    const withWeight = areas.filter(a => wTotal(a) != null);
    const weight_g = withWeight.length
      ? withWeight.reduce((n, a) => n + wTotal(a), 0) : null;
    const measured = areas.filter(a => a.measured_weight_g != null);
    const measured_weight_g = measured.length
      ? measured.reduce((n, a) => n + a.measured_weight_g, 0) : null;

    // PER AREA, over the halls that have BOTH -- mirroring per_area_m. A
    // miscalibrated cell is one hall's broken instrument, and comparing slot
    // totals averages it into the halls that are fine: a Tiffin scale 35% heavy
    // at a three-hall position moves the total by 7%, inside the 25% band, so
    // the slot reads healthy and the fault is invisible.
    //
    // Outside BOTH bands, never one -- public.weight_mismatch_tolerance().
    const TOL_G = 2000, TOL_FRAC = 0.25;
    const mismatched = areas.filter(a => {
      const e = estTotal(a);
      if (a.measured_weight_g == null || e == null) return false;
      const d = Math.abs(a.measured_weight_g - e);
      return d > TOL_G && d > TOL_FRAC * Math.max(e, 1);
    });
    const weight_mismatch = mismatched.length > 0;
    const weight_mismatch_g = mismatched.length
      ? mismatched.reduce((n, a) => n + (a.measured_weight_g - estTotal(a)), 0) : null;
    const weight_mismatch_areas = mismatched.map(a => a.location);

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
      est_weight_g: est,
      capacity_weight_g: capW,
      // WIDENED with the view: "holds bowls it cannot weigh AT ALL", not
      // "holds bowls with no per-bowl weight". A hall with a load cell has a
      // weight even with no menu figure typed. With no scales anywhere this
      // collapses to the old test exactly.
      est_is_partial: areas.some(a => wTotal(a) == null && (a.bowls_trusted || 0) > 0),
      areas_without_weight: areas
        .filter(a => wTotal(a) == null && (a.bowls_trusted || 0) > 0)
        .map(a => a.location),
      areas: areas.map(a => ({
        location: a.location, food_name: a.food_name,
        bowl_weight_g: a.bowl_weight_g, bowls_trusted: a.bowls_trusted,
        bowls_capacity: a.bowls_capacity, devices: a.devices,
        // The hall's own mass, and it is the precedence answer rather than the
        // estimate it used to be. Still NULL, never 0, when the hall has
        // neither a measurement nor a reading to multiply.
        weight_g: wLine(a),
        est_weight_g: estLine(a),
        measured_weight_g: a.measured_weight_g,
        weight_source: srcOf(a),
        scales: a.scales,
        capacity_weight_g: a.bowl_weight_g == null
          ? null : a.bowls_capacity * a.bowl_weight_g,
      })).sort((x, y) => x.location.localeCompare(y.location)),
      scales: areas.reduce((n, a) => n + a.scales, 0),
      scales_ok: areas.reduce((n, a) => n + a.scales_ok, 0),
      measured_weight_g, weight_g, weight_source,
      weight_mismatch, weight_mismatch_g, weight_mismatch_areas,
      scale_issues: [...new Set(areas.flatMap(a => a.scale_issues))].sort(),
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

// --- status_events, for the device page -------------------------------

function statusEvents(deviceId) {
  const dev = devices.find(d => d.device_id === deviceId);
  if (!dev || BACKUPS.has(deviceId) || dev.location === 'R') return [];
  const now = Date.now();
  const out = [];
  // One row per real CHANGE, never per report — the property the step-after
  // chart on the device page depends on.
  for (let k = 0; k < 24; k++) {
    const at = now - k * 6 * 60_000;
    out.push({
      id: 10_000 - k, device_id: deviceId, boot_id: 1, seq: 200 - k,
      recorded_at: new Date(at).toISOString(),
      received_at: new Date(at).toISOString(),
      reason: k === 23 ? 'boot' : 'change',
      stack_count: MAX_BOWLS - (k % (MAX_BOWLS + 1)),
      stack_status: statusOf(dev),
      levels: levelsFor(dev, MAX_BOWLS - (k % (MAX_BOWLS + 1))),
      sensors_ok: [true, true, deviceId !== DEGRADED, true],
      sensors_online: deviceId === DEGRADED ? 3 : 4,
      battery_level: batteryOf(dev)[1],
      charging: false,
      firmware: dev.firmware,
    });
  }
  return out;
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
      if (name === 'device_overview') data = overview;
      else if (name === 'slot_overview') data = slotOverview(overview);
      else if (name === 'slot_quantity') data = slotQuantityRows(overview);
      else if (name === 'meal_menu_template') data = [...template.values()];
      else if (name === 'meal_food_mapping') data = [...menu.values()];
      else if (name === 'devices') data = devices;
      else if (name === 'status_events') {
        const idf = rec.filters.find(f => f[0] === 'eq' && f[1] === 'device_id');
        data = idf ? statusEvents(idf[2]) : [];
      }
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
