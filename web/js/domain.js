// ====================================================================
//  Domain semantics.
//
//  Every rule here traces to docs/FRONTEND_HANDOFF.md §4 and
//  docs/meal_mapping.md §3. They live in one file because getting any of
//  them wrong is the difference between a dashboard that is acted on and
//  one that cries wolf, and they must not be re-decided per screen.
// ====================================================================

export const LOCATION_NAMES = { D: 'Darshanarthi', M: 'Mahatma', T: 'Tiffin', R: 'Reserved' };
export const SERVING_LOCATIONS = ['D', 'M', 'T'];
export const MEAL_TYPES = ['Breakfast', 'Lunch', 'Dinner'];
export const FOOD_SLOTS = [1, 2, 3, 4, 5, 6, 7, 8];
export const DEFAULT_TZ = 'Asia/Kolkata';

/** Index 0 = Sunday, matching Postgres extract(dow), JS Date.getDay(), and the
 *  meal_menu_template.weekday column — one convention end to end, because a
 *  dow/isodow mix-up serves Monday's menu on Sunday and survives testing until
 *  a week boundary. */
export const WEEKDAYS = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];

/** Weekday of a YYYY-MM-DD service date, 0 = Sunday. Parsed at noon UTC the
 *  way addDays() already does, so a browser in any timezone lands on the same
 *  day. (fmtDay parses at local midnight, which is fine for formatting but
 *  must never be the basis for the weekday.) */
export function weekdayOf(dateStr) {
  return new Date(`${dateStr}T12:00:00Z`).getUTCDay();
}

/** Fleet service windows, for the local-clock fallback in the menu editor only.
 *  Liveness is never computed here — the server owns that (see `offline`). */
export const SERVICE_WINDOWS = [
  { meal: 'Breakfast', start: '06:00', end: '09:00' },
  { meal: 'Lunch',     start: '11:30', end: '14:00' },
  { meal: 'Dinner',    start: '18:30', end: '21:00' },
];

// --- battery --------------------------------------------------------
//
// A BAND, not a percentage, and the band edges are hysteretic — so no
// number is derived from it in either direction. `null` means no cell
// was detected, which is not the same as flat and must never render as
// an empty battery.

// POWER STATE FROM THE TWO FACTS THAT CARRY IT, in one place, because doing it
// per-surface is how the Master chips kept a stale tooltip and no bolt for a
// day after the Health page was fixed.
//
//   charging        the ETA6098's STAT pin -- unreadable without the mod, so
//                   usually null, and null is NOT "no"
//   external_power  the VBUS divider -- known on every board that has it
//
// `powered` drives the bolt: it means "this unit is on mains", which is what a
// bolt means on a phone and is still true at 100%. `word` is the tooltip, and
// it never claims a charge it cannot see.
export function powerState(dev, powerRows) {
  // A HUB'S `charging` IS ITS MAINS: true on the charger, false running on
  // the backup battery, null unknown. It has no STAT-pin caveat to work
  // around, so the three states are read straight off it.
  if (isHub(dev)) {
    return dev.charging === true ? { powered: true, word: 'charging' }
         : dev.charging === false ? { powered: false, word: 'on battery' }
         : { powered: false, word: 'charging unknown' };
  }
  const row = (powerRows || []).find(p => p.device_id === dev.device_id);
  const ext = row ? row.external_power : null;
  const chg = dev.charging;

  if (chg === true) return { powered: true, word: 'charging' };
  if (chg === false && ext === true) return { powered: true, word: 'on mains, charge complete' };
  if (chg === false) return { powered: false, word: 'not charging' };
  if (ext === true) return { powered: true, word: 'on mains' };
  if (ext === false) return { powered: false, word: 'on battery' };
  return { powered: false, word: 'charge state unknown' };
}

const BATTERY = {
  good:     { label: 'Battery good',     status: 'good',     glyph: '▮' },
  medium:   { label: 'Battery medium',   status: 'good',     glyph: '▮' },
  low:      { label: 'Battery low',      status: 'warning',  glyph: '▮' },
  critical: { label: 'Battery critical', status: 'critical', glyph: '▮' },
};

export function batteryInfo(level) {
  if (level == null) return { label: 'No battery', status: 'idle', glyph: '—', absent: true };
  return BATTERY[level] || { label: `Battery ${level}`, status: 'idle', glyph: '?' };
}

// WHO HAS A BATTERY. Since migrate_hubs.sql the battery is the HUB's -- one
// ESP32 per area on mains with a small backup cell -- and a weighed platform
// reports none at all. A legacy stack keeps its own. Asking this, rather than
// whether battery_level is null, is what stops every platform reading "No
// battery detected": NULL there is not a missing cell, it is not its column.
export function carriesBattery(dev) {
  return isHub(dev) || isStack(dev);
}

/** Low or critical, on a device that carries a battery. One test, because the
 *  header chip and the Health filter it opens must count the same rows. */
export function isBatteryWarn(dev) {
  return carriesBattery(dev)
    && (dev.battery_level === 'low' || dev.battery_level === 'critical');
}

// --- node health ------------------------------------------------------
//
// What a hub polls from each platform hanging off it. NULL is "not measured"
// -- today's I2C platforms report only some of the four, RS485 nodes all --
// so a NULL ranks nothing and renders as a dash, never as 0.

/** A 5 V feed read below 4.5 V at the node has lost a tenth to cable drop or
 *  a failing supply; brown-outs and dropped cells are the next step down. */
export const NODE_MIN_SUPPLY_MV = 4500;
/** Half a kilo on an EMPTY platform is a moved zero, not creep -- and every
 *  figure the platform sends is off by that much until it is re-zeroed. */
export const NODE_MAX_DRIFT_G = 500;
/** A stray few corrupt frames since power-up is cable noise; a hundred is a
 *  connector, cable or termination fault worth a visit before it gets worse. */
export const NODE_MAX_CHECKSUM = 100;

/** The four facts as display strings, '—' for each one not measured. */
export function nodeParts(dev) {
  const mv = dev.supply_mv, bad = dev.checksum_errors, g = dev.no_load_g;
  return {
    supply: mv == null ? '—' : `${(Number(mv) / 1000).toFixed(2)} V`,
    checksum: bad == null ? '—' : String(bad),
    link: dev.responding == null ? '—' : dev.responding ? 'responding' : 'not responding',
    drift: g == null ? '—' : `${Number(g) > 0 ? '+' : ''}${g} g`,
  };
}

/** The roster's short form: only what was measured, in display order. */
export function nodeHealthList(dev) {
  const p = nodeParts(dev);
  return [
    dev.supply_mv == null ? null : p.supply,
    // "checksum", not "CRC": the RS485 frames carry a plain checksum (owner).
    dev.checksum_errors == null ? null : `${p.checksum} checksum err`,
    dev.responding == null ? null : p.link,
    dev.no_load_g == null ? null : `no-load ${p.drift}`,
  ].filter(Boolean);
}

/** ...as one line, '—' when nothing was measured. */
export function nodeHealthText(dev) {
  return nodeHealthList(dev).join(' · ') || '—';
}

/** The hub a platform hangs off: the one in its area. Null before
 *  migrate_hubs.sql, when there are no hub rows to find. */
export function hubOf(dev, devices) {
  return dev.location == null ? null
    : (devices || []).find(d => isHub(d) && d.location === dev.location) || null;
}

/** A hub's nodes (ATtiny platforms): MAPPED = every counter scale or buffer in its
 *  area with a slot -- a spare has none, so it hangs off nothing -- and ACTIVE =
 *  those reporting, not offline, and not refusing the hub's poll. */
export function hubNodes(hub, devices) {
  const mapped = (devices || []).filter(d =>
    isWeighed(d) && d.location === hub.location && d.food_slot != null);
  const active = mapped.filter(d => d.reported && !d.offline && d.responding !== false);
  return { mapped: mapped.length, active: active.length };
}

// --- stack count trust ----------------------------------------------

/**
 * What to render for ONE device's count.
 *   fault  — discontiguous: a bowl detected above an empty level. Physically
 *            impossible, so there is no count to show. "2 bowls" here would be
 *            worse than an error.
 *   bound  — degraded: a dead sensor leaves the count ambiguous. The number is
 *            a LOWER bound and must be labelled as one.
 *   count  — trustworthy.
 *   none   — never reported.
 */
/** A stack physically holds at most this many bowls. */
export const MAX_BOWLS = 4;

export function deviceStack(dev) {
  if (!dev.reported || dev.stack_count == null) {
    return { kind: 'none', text: '—', note: 'No reading' };
  }
  // Zero working sensors means zero detection: whatever count rode along in
  // the payload is a leftover, not a measurement. Rendering it — even as a
  // bound — would dress a blind device up as data. (Strict === : null means
  // the column is absent, not that the sensors are.)
  if (dev.sensors_online === 0) {
    return { kind: 'none', text: '—', note: 'No working sensors — no reading' };
  }
  if (dev.stack_status === 'discontiguous') {
    return { kind: 'fault', text: '!', note: 'Impossible reading — check the sensors' };
  }
  if (dev.stack_status === 'degraded') {
    // A lower bound at the physical ceiling is not a bound, it is the answer:
    // "≥4" of a maximum 4 says exactly 4, and printing the ≥ reads as
    // nonsense ("more than full?"). The sensor is still down — the badges
    // say so — but the number is exact.
    if (dev.stack_count >= MAX_BOWLS) {
      return { kind: 'count', value: MAX_BOWLS, text: String(MAX_BOWLS),
               note: 'A sensor is down, but a full stack leaves no ambiguity' };
    }
    return { kind: 'bound', value: dev.stack_count, text: `≥${dev.stack_count}`,
             note: 'A sensor is down — this is a lower bound' };
  }
  return { kind: 'count', value: dev.stack_count, text: String(dev.stack_count), note: '' };
}

/**
 * What to render for one DISH POSITION, which may be served by several stacks.
 * The arithmetic is the view's — `bowls_trusted` already sums only the devices
 * reporting `ok`. Nothing is re-summed here.
 *
 * `devs` are the device_overview rows at this position. They decide ONE thing,
 * whether a weight is a lower bound, because the view's scales/buffers counts
 * cannot: they include instruments that have never reported. LDC-002..024 sit
 * assigned at every position and have never spoken, so `scales_ok < scales`
 * would put a ≥ on every card for good -- and a unit awaiting deployment is
 * not missing food, it is not installed.
 */
export function slotStock(slot, devs = []) {
  const capacity = Number(slot.bowls_capacity) || 0;
  const trusted = slot.bowls_trusted == null ? null : Number(slot.bowls_trusted);

  if (slot.any_fault) {
    return {
      kind: 'fault',
      capacity, trusted,
      severity: 'critical',
      headline: 'Fault',
      note: 'A stack is reporting an impossible level pattern.',
    };
  }
  // A SCALE AT THIS POSITION IS DATA, even when no bowl stack has reported.
  //
  // This tested bowls_trusted alone, so a position with a load cell reporting
  // weight_state='ok' and no trustworthy stack beside it printed "No data" as
  // its headline -- while the very same card carried the weight on its device
  // line and Master had it right. Throwing away a measurement to announce its
  // absence is the one thing this dashboard is written not to do.
  //
  // GATED ON measured_weight_g, NEVER ON slot.weight_g: the view coalesces
  // weight_g to 0 when the buffer is unknown, so testing that would read an
  // unknown buffer as a real zero -- the same NULL-versus-zero confusion in the
  // other direction.
  const measured = slot.measured_weight_g == null ? null : Number(slot.measured_weight_g);
  // A BUFFER PLATFORM MEANS THE VIEW IS POST-migrate_buffer.sql, and only then
  // is slot.weight_g NULL-not-0 and safe to read. Without one, the measured
  // counter figure is the only weight a stack-less position has.
  const buffered = devs.some(isBuffer);
  if (trusted == null && (buffered || measured != null)) {
    const grams = buffered
      ? (slot.weight_g == null ? null : Number(slot.weight_g))
      : measured;
    if (grams == null) {
      return { kind: 'nodata', capacity, trusted: null, severity: 'idle',
               headline: 'No data',
               note: 'No buffer or scale at this position has a weight right now.' };
    }
    const partial = devs.some(d => isWeighed(d) && d.reported && d.weight_state !== 'ok');
    const bufG = slot.buffer_g == null ? null : Number(slot.buffer_g);
    const bowls = slot.buffer_bowls == null ? null : Number(slot.buffer_bowls);
    return {
      kind: 'weight',
      capacity, trusted: null, measured, grams, partial,
      severity: null,
      headline: fmtWeight(grams, partial),
      // The line under the figure: the buffer's bowls when there are any, which
      // is what somebody walks over and counts by eye.
      sub: bowls != null ? `${bowlsText(bowls, slot.buffer_unconfirmed)} buffered`
         : measured != null && bufG == null ? 'weighed at the counter' : 'weighed',
      note: [bufG != null ? `${fmtWeight(bufG)} in the buffer` : null,
             measured != null ? `${fmtWeight(measured)} on the counter` : null]
              .filter(Boolean).join(' + ') + '.'
        + (partial ? ' A lower bound — a buffer or scale here has no weight right now.' : ''),
    };
  }
  if (trusted == null) {
    // NULL is not zero. One sends someone to refill, the other to investigate.
    return {
      kind: 'nodata',
      capacity, trusted: null,
      severity: 'idle',
      headline: 'No data',
      note: 'No stack at this position has reported.',
    };
  }
  return {
    kind: 'count',
    capacity, trusted,
    severity: null,
    headline: String(trusted),
    note: '',
  };
}

// --- weight ----------------------------------------------------------
//
// The Master Dashboard's currency. Everything crossing this boundary is in
// GRAMS as an integer, exactly as the database stores it (schema.sql,
// meal_food_mapping.bowl_weight_g), and is divided by 1000 exactly once —
// here, at render. Carrying kilograms around as floats instead is what makes
// fourteen bowls of 6.5 kg add up to 90.99999999999999 on screen.

/** Grams → "6.5 kg". `partial` prefixes the ≥ that marks a lower bound — the
 *  same notation deviceStack() already uses for a degraded stack's count.
 *  One vocabulary for "real but incomplete", not two. */
export function fmtWeight(grams, partial = false) {
  if (grams == null) return '—';
  const kg = Number(grams) / 1000;
  if (!Number.isFinite(kg)) return '—';
  return `${partial ? '≥' : ''}${kg.toFixed(1)} kg`;
}

/** "3 bowls", or "3? bowls" when a buffer remembers the count from before a
 *  power cycle and nothing has confirmed it since -- the firmware's own word
 *  for that is "unconfirmed", and the ? is the same doubt in one character. */
export function bowlsText(n, unconfirmed = false) {
  return `${n}${unconfirmed ? '?' : ''} bowl${Number(n) === 1 ? '' : 's'}`;
}

/**
 * What to render for one SLOT NUMBER across every serving area — the Master
 * Dashboard's row. The arithmetic is slot_quantity's and nothing is re-summed
 * here, for exactly the reason slotStock() re-sums nothing: two screens doing
 * the same sum in two places is two screens that will eventually disagree.
 *
 * The branch order is the order an operator actually asks the questions: is
 * the number broken, is there a number at all, can it be turned into
 * kilograms, and only then — how much.
 */
export function slotQuantity(row) {
  const trusted  = row.bowls_trusted == null ? null : Number(row.bowls_trusted);
  const capacity = Number(row.bowls_capacity) || 0;

  // THE AUTHORITATIVE FIGURE. slot_quantity.weight_g is the SUM of the two
  // pools per area -- buffered bowls plus what the counter is weighing --
  // because they are different food in different places, not rival estimates
  // of the same food. est_weight_g and
  // measured_weight_g are its two inputs, carried so the row can say which it
  // is showing rather than leaving a reader to guess.
  //
  // Falls back to est_weight_g when weight_g is absent, which is a real state
  // rather than defensiveness: a database that has had migrate_bowl_weight.sql
  // but not migrate_loadcell.sql has the estimate and no precedence column, and
  // Master should go on working there exactly as it did.
  //
  // ABSENT, NOT NULL. Once migrate_buffer.sql is in, weight_g is NULL when
  // nothing at the slot has a weight -- and est_weight_g can then be a real 0
  // (a per-bowl weight is set, no stack is left to count), so falling back on
  // a NULL would print "0.0 kg" for a slot nobody can see. Only a database
  // that has no weight_g column at all takes the fallback.
  const estGrams = row.est_weight_g == null ? null : Number(row.est_weight_g);
  const measGrams =
    row.measured_weight_g == null ? null : Number(row.measured_weight_g);
  const grams = !('weight_g' in row) ? estGrams
              : row.weight_g == null ? null : Number(row.weight_g);

  const capacityGrams =
    row.capacity_weight_g == null ? null : Number(row.capacity_weight_g);
  // Two reasons for a lower bound, and only the first has a fix on the Menu
  // tab: a hall holds bowls with no per-bowl weight (est_is_partial), or a
  // buffer or scale here is not reporting a weight (weight_is_partial, absent
  // before migrate_buffer.sql). Never a ≥ on a NULL -- there is no bound to
  // state when there is no figure.
  const estPartial = !!row.est_is_partial;
  const partial  = grams != null && (estPartial || !!row.weight_is_partial);
  const bufMeas = row.buffer_measured_g == null ? null : Number(row.buffer_measured_g);
  const bowls = row.buffer_bowls == null ? null : Number(row.buffer_bowls);

  // NO weight_source AND NO MISMATCH, and both were removed for the same
  // reason: they framed the buffer and the counter as rival answers to one
  // question. They are not. The stack counts bowls held in reserve and the
  // platform weighs the serving counter -- different food, in different
  // places, which slot_quantity now ADDS. A "source" is meaningless when both
  // contribute, and a "disagreement" between them was two correct instruments
  // being accused of a fault.
  const base = { trusted, capacity, grams, estGrams, measGrams, capacityGrams,
                 partial, estPartial, bowls,
                 bowlsUnconfirmed: !!row.buffer_unconfirmed };
  const lowerBound = partial
    ? ' A lower bound — a buffer or scale here has no weight right now.' : '';

  if (trusted == null && grams == null) {
    // NULL is not zero — the distinction the whole schema is built around.
    // One sends someone to refill, the other to investigate.
    return { ...base, kind: 'nodata', severity: 'idle', headline: 'No data',
      note: 'Nothing at this position has a weight or a count.' };
  }
  if (trusted == null) {
    // NO BOWL COUNT, BUT A WEIGHT — the load-cell-only position, and the case
    // that made this branch necessary. It is also what a position looks like
    // when every stack has gone dark mid-service and the scale is the only
    // thing still measuring: the old code returned 'No data' here and threw the
    // measurement away, which is the one moment it matters most.
    //
    // Since migrate_buffer.sql it is ALSO every buffer-platform position, which
    // weighs its bowls rather than counting them -- so the note names whichever
    // instruments produced the figure.
    return { ...base, kind: 'count', severity: null,
      headline: fmtWeight(grams, partial),
      note: (bufMeas != null && measGrams != null
              ? `${fmtWeight(bufMeas)} in the buffer + ${fmtWeight(measGrams)} on the counter.`
            : bufMeas != null ? 'Weighed in the buffer. No counter scale here has a weight.'
            : 'Weighed at the counter.' + (Number(row.buffers) > 0 ? ''
                : ' No bowl stack here has reported a count.'))
        + lowerBound };
  }
  if (grams == null) {
    // Bowls are known; kilograms are not. "0.0 kg" here would be a
    // measurement nobody made, so the row falls back to the count it does
    // have and says what is missing.
    return { ...base, kind: 'noweight', severity: 'idle',
      headline: `${trusted} ${trusted === 1 ? 'bowl' : 'bowls'}`,
      note: 'No per-bowl weight set for this dish — add one on the Menu tab.' };
  }
  // A FAULT NO LONGER SUPPRESSES THE FIGURE.
  //
  // It used to return "Fault" and no number, borrowed from slotStock(), where
  // that is right: Stock shows one position's raw count and an impossible
  // level pattern means there is no count to show. Here the number is
  // different in kind — bowls_trusted already EXCLUDES the faulted stack, so
  // this is what the healthy stacks hold, and Master itemises the halls
  // underneath. Hiding a total while listing every part of it reads as a bug
  // rather than as caution.
  //
  // So the figure stands, `kind` still says a fault is present, and the
  // caller paints it red — the same treatment an offline stack's last known
  // value already gets.
  return { ...base,
    kind: row.any_fault ? 'fault' : 'count',
    severity: row.any_fault ? 'critical' : null,
    headline: fmtWeight(grams, partial),
    note: row.any_fault
      ? 'A stack here is reporting an impossible level pattern — this figure '
        + 'is what the remaining healthy stacks hold.'
      : estPartial ? `A lower bound — ${areaList(row.areas_without_weight)} `
                     + 'holds bowls with no per-bowl weight set.'
      : lowerBound.trim() };
}

/** "Darshanarthi and Tiffin" from ['D','T']. Only slotQuantity() needs it,
 *  so it is not exported. */
function areaList(locs) {
  const names = (locs || []).map(l => LOCATION_NAMES[l] || l);
  if (names.length <= 1) return names[0] || 'an area';
  return `${names.slice(0, -1).join(', ')} and ${names[names.length - 1]}`;
}

// There is deliberately NO quantity colour band any more. An earlier cut
// tinted the count by bowls-per-stack (red when nearly empty); field feedback
// killed it — a colour per meaning is all a human can register, and red now
// means exactly one thing everywhere: the figure is compromised (offline /
// degraded / fault). Quantity is the number's own job, and data confidence
// is the capsule bar's.

// --- liveness ---------------------------------------------------------
//
// Both flags are the SERVER's. Nothing here ever derives its own staleness
// from updated_at — devices are dark ~16 h a day by design and that
// arithmetic would false-alarm on every healthy unit.
//
//   offline              died mid-window: should be reporting NOW and is not.
//                        Clears when the window closes.
//   missed_last_service  slept through the most recently completed service
//                        window. Survives the dark hours, which is what
//                        `offline` alone could not do — a device dead since
//                        Tuesday used to look identical to a healthy one
//                        between meals.
//
// The UI treats either as "offline" (red, last value kept) because the
// operator's question is the same: this station is not talking.

/** Is this device not reporting when it should be? */
export function deviceOffline(dev) {
  return !!(dev.offline || dev.missed_last_service);
}

/** Is some stack at this dish position not reporting when it should be? */
export function slotOffline(slot) {
  return !!(slot.any_offline || slot.any_missed_service);
}

// --- device severity -------------------------------------------------

/**
 * Rank for the health view. The point of that screen is to surface the one
 * station needing attention, not to enumerate 30 healthy ones — so it sorts by
 * this, never by device_id.
 *
 * `offline` comes from the view and is already service-hour aware. Staleness is
 * NOT recomputed from updated_at: devices are dark ~16 h a day by design, so
 * that would false-alarm on every healthy unit and bury the real failure.
 */
/** One state glyph per device, shared by Stock's strip and Health's roster.
 *  Priority mirrors severity: an impossible reading outranks silence
 *  outranks a dead sensor outranks no reading. Each state owns a distinct
 *  SHAPE as well as a colour (classes st-*), so the pairing survives
 *  colour-blindness; the words appear once per page, in the legend. */
/** Is this installation a load cell? One place, because `kind` is absent on a
 *  database that predates migrate_loadcell.sql and every caller would
 *  otherwise have to remember that a missing kind means 'stack'. */
// WHAT COUNTS AS A FAULT, AND AS DEGRADED, FOR EITHER PRODUCT -- in one place.
//
// There were three copies of this rule: fleetSummary() below, the Stock alert
// strip, and health.js's Faults/Degraded filters. Only health.js had been
// taught about load cells, so the other two counted `stack_status` alone -- and
// a scale never writes stack_status. Every weight fault a station can report
// (no_cells, over_range, cells_partial, uncalibrated, untared) was therefore
// invisible to both fleet roll-ups: the header said "0 faults" and the default
// Stock page showed no alert strip while a station sat there with not one cell
// answering. Health, one click away, ranked that same station critical.
//
// The split follows deviceSeverity(): a FAULT is a reading that cannot be
// trusted at all, DEGRADED is one that is a lower bound or unproven.
export function isFault(dev) {
  return dev.stack_status === 'discontiguous'
      || dev.weight_state === 'no_cells'
      || dev.weight_state === 'over_range'
      // A platform its hub cannot reach: whatever weight the row still holds
      // is not being refreshed. Strict === false -- NULL is "not measured".
      || dev.responding === false;
}

export function isDegraded(dev) {
  return dev.stack_status === 'degraded'
      || dev.weight_state === 'cells_partial'
      || dev.weight_state === 'uncalibrated'
      || dev.weight_state === 'untared';
}

export function isScale(dev) {
  return !!dev && dev.kind === 'scale';
}

// THREE PRODUCTS NOW, and two of them weigh. A BUFFER is a BWL-xxx platform
// under the bowls waiting behind the line -- one 200 kg cell, re-kinded from
// the old ToF stack by supabase/cutover_buffers.sql -- and it reports in
// exactly the scale's columns (weight_state first, weight_g only when 'ok'),
// plus how many bowls it is carrying. So every screen that already knew how
// to draw a scale takes a buffer through the same path, by asking isWeighed().
export function isBuffer(dev) {
  return !!dev && dev.kind === 'buffer';
}

/** Absent kind is a stack: a database from before migrate_loadcell.sql has no
 *  column, and every device on it counts bowls. */
export function isStack(dev) {
  return !!dev && (dev.kind == null || dev.kind === 'stack');
}

export function isWeighed(dev) {
  return isScale(dev) || isBuffer(dev);
}

/** HUB-D / HUB-M / HUB-T: one per area, measures nothing itself. Its
 *  platforms are the scales and buffers sharing its `location`. */
export function isHub(dev) {
  return !!dev && dev.kind === 'hub';
}

/** Cells under one platform: the counter sums three 20 kg cells, a buffer
 *  stands on one 200 kg cell. Per kind rather than a constant, or a buffer
 *  with its only cell answering reads "1 of 3" -- a fault it does not have. */
export function cellsTotal(dev) {
  return isBuffer(dev) ? 1 : 3;
}

/**
 * What to render for a LOAD CELL, and the counterpart to deviceStack().
 *
 * The two are deliberately the same shape -- {kind, text, note} -- because the
 * screens that show them are the same screens, and a caller that has to know
 * which product it is looking at before it can lay out a row is a caller that
 * will get it wrong for one of them.
 *
 * THE STATE IS ALWAYS KNOWN AND THE NUMBER IS NOT, which is the inversion the
 * whole load-cell schema is built around: weight_state always arrives, and
 * weight_g exists only when that state is 'ok'. So this function reads the
 * state first and the number second, never the other way round.
 */
export function deviceWeight(dev) {
  if (!dev.reported) {
    return { kind: 'none', text: '—', note: 'Never reported' };
  }
  const st = dev.weight_state;
  if (st == null) {
    // Registered as a scale, reporting, but sending no weight_state at all --
    // firmware older than the load-cell uplink. Not a fault in the hardware.
    return { kind: 'none', text: '—', note: 'No weight reported — check the firmware' };
  }
  if (st === 'ok') {
    // 0 is a real weight and must render as 0.0 kg, not as a dash. An empty,
    // tared, calibrated platform means "refill me"; a dash means nobody knows.
    //
    // A BUFFER'S weight_g IS FOOD -- the gross load less 2.5 kg per bowl it
    // has counted -- so the headline is what can be served, and the bowls and
    // the gross ride the note. Each part only when the row carries it: bowls
    // NULL says nothing about bowls rather than claiming none.
    const note = isBuffer(dev) ? [
      dev.bowls == null ? null
        : bowlsText(dev.bowls) + (dev.bowls_confirmed === false ? ' (unconfirmed)' : ''),
      dev.gross_g == null ? null : `gross ${fmtWeight(dev.gross_g)}`,
    ].filter(Boolean).join(' · ') : '';
    return { kind: 'weight', value: dev.weight_g,
             text: fmtWeight(dev.weight_g), note };
  }
  const say = {
    no_cells:      ['fault', 'No load cell is answering'],
    cells_partial: ['fault', 'A load cell has dropped out — cells sum, so the total would read LOW'],
    over_range:    ['fault', isBuffer(dev)
      // The buffer firmware also reports this when the food figure falls
      // below -5 kg: more bowls counted than are on it, not a saturated cell.
      ? 'Out of range — the cell is saturated, or more bowls are counted than are on it'
      : 'A converter is saturated — the total is not a weight'],
    // A buffer takes no power-up zero (its empty zero is stored), so settling
    // there is only the first readings arriving.
    settling:      ['idle',  isBuffer(dev) ? 'Waiting for steady readings'
                                           : 'Taking its power-up zero'],
    uncalibrated:  ['idle',  'Never calibrated — it has counts, not kilograms'],
    untared:       ['idle',  'Not tared — the figure would include the platform'],
  }[st] || ['idle', st];
  return { kind: say[0] === 'fault' ? 'fault' : 'nostate',
           text: st === 'no_cells' ? '!' : '—', note: say[1], state: st };
}

export function deviceGlyph(d) {
  // A hub has no reading to judge, so it is only ever talking or not.
  if (isHub(d)) {
    if (deviceOffline(d)) return { cls: 'off', glyph: '✕', word: 'offline' };
    if (!d.reported) return { cls: 'none', glyph: '◌', word: 'never reported' };
    return { cls: 'ok', glyph: '●', word: 'reporting' };
  }
  // A scale has no levels and no stack status, so the bowl-counter ladder
  // below would read every one of them as "no reading" -- a grey ring on a
  // station that is weighing perfectly well.
  if (isWeighed(d)) {
    const w = deviceWeight(d);
    if (deviceOffline(d))
      return { cls: 'off', glyph: '✕', word: 'offline — showing its last value' };
    if (d.responding === false)
      return { cls: 'fault', glyph: '▲', word: 'not answering the hub' };
    if (w.kind === 'fault')
      return { cls: 'fault', glyph: '▲', word: w.note.toLowerCase() };
    if (w.kind === 'nostate')
      return { cls: 'deg', glyph: '◐', word: w.note.toLowerCase() };
    if (w.kind === 'none')
      return { cls: 'none', glyph: '◌', word: 'no reading' };
    return { cls: 'ok', glyph: '●', word: 'weighing' };
  }
  const st = deviceStack(d);
  if (st.kind === 'fault' || d.stack_status === 'discontiguous')
    return { cls: 'fault', glyph: '\u25b2', word: 'impossible reading \u2014 check the sensors' };
  if (deviceOffline(d))
    return { cls: 'off', glyph: '\u2715', word: 'offline \u2014 showing its last value' };
  if (d.stack_status === 'degraded')
    return { cls: 'deg', glyph: '\u25d0', word: 'a sensor is down' };
  if (st.kind === 'none')
    return { cls: 'none', glyph: '\u25cc', word: 'no reading' };
  return { cls: 'ok', glyph: '\u25cf', word: 'reporting' };
}

export function deviceSeverity(dev) {
  const reasons = [];
  let rank = 0;

  if (dev.awaiting_deployment) {
    return { rank: -1, level: 'idle', reasons: ['Awaiting deployment'] };
  }
  if (dev.offline) { rank = Math.max(rank, 100); reasons.push('Offline during service'); }
  if (dev.missed_last_service) { rank = Math.max(rank, 85); reasons.push('Not reporting since before the last service ended'); }

  // Liveness above is true of any device. What follows is product-specific,
  // and running the bowl-counter tests over a scale is how a perfectly
  // healthy load cell acquires "0 of 4 sensors online".
  if (isHub(dev)) {
    // THE AREA'S POWER. A hub on its backup cell has lost mains, and every
    // platform it polls runs on that cell's remaining charge -- ranked into
    // "needs attention", below a low battery that is already running out.
    if (dev.battery_level === 'critical') { rank = Math.max(rank, 80); reasons.push('Battery critical'); }
    if (dev.battery_level === 'low') { rank = Math.max(rank, 60); reasons.push('Battery low'); }
    if (dev.charging === false) { rank = Math.max(rank, 50); reasons.push('On backup battery — mains lost'); }
    if (dev.battery_mv == null && dev.battery_level == null) {
      rank = Math.max(rank, 20); reasons.push('No battery detected');
    }
    // No food_slot is the design, not a gap: a hub serves an AREA.
    if (dev.location == null) { rank = Math.max(rank, 10); reasons.push('Not assigned to an area'); }
    return { rank, level: levelOf(rank), reasons };
  }
  if (isWeighed(dev)) {
    // NO BATTERY RANKS HERE. A platform's battery is its hub's; its columns
    // are NULL by contract, and "No battery detected" on every one of them
    // would bury the hub that actually has a problem.
    if (dev.responding === false) {
      rank = Math.max(rank, 90); reasons.push('Not answering the hub');
    }
    if (dev.supply_mv != null && dev.supply_mv < NODE_MIN_SUPPLY_MV) {
      rank = Math.max(rank, 50);
      reasons.push(`Low supply at the node — ${nodeParts(dev).supply}`);
    }
    if (dev.no_load_g != null && Math.abs(dev.no_load_g) >= NODE_MAX_DRIFT_G) {
      // Ranked with uncalibrated: the figure is wrong by a known amount, not absent.
      rank = Math.max(rank, 45);
      reasons.push(`Zero drifted — reads ${nodeParts(dev).drift} empty`);
    }
    if (dev.checksum_errors != null && dev.checksum_errors >= NODE_MAX_CHECKSUM) {
      // Below the rest: frames that fail the check are dropped, so the
      // readings that do arrive are still good -- there are just fewer.
      rank = Math.max(rank, 30);
      reasons.push(`${dev.checksum_errors} checksum errors since power-up`);
    }
    if (dev.weight_state === 'no_cells') {
      rank = Math.max(rank, 90); reasons.push('No load cell is answering');
    } else if (dev.weight_state === 'cells_partial') {
      // Ranked with a fault, not a warning. Cells under one platform SUM, so a
      // missing cell does not add noise -- it makes the total read LOW, which
      // looks exactly like a lighter bowl and is the harder failure to notice.
      rank = Math.max(rank, 88);
      reasons.push(`${dev.cells_online ?? '?'} of ${cellsTotal(dev)} cells — the total would read low`);
    } else if (dev.weight_state === 'over_range') {
      rank = Math.max(rank, 86); reasons.push(deviceWeight(dev).note);
    } else if (dev.weight_state === 'uncalibrated') {
      rank = Math.max(rank, 45); reasons.push('Never calibrated — counts, not kilograms');
    } else if (dev.weight_state === 'untared') {
      rank = Math.max(rank, 40); reasons.push('Not tared — the figure includes the platform');
    } else if (dev.weight_state === 'settling') {
      rank = Math.max(rank, 15); reasons.push(deviceWeight(dev).note);
    }
    if (dev.location == null || dev.food_slot == null) {
      rank = Math.max(rank, 10); reasons.push('Not assigned to a position');
    }
    return { rank, level: levelOf(rank), reasons };
  }

  if (dev.stack_status === 'discontiguous') { rank = Math.max(rank, 90); reasons.push('Impossible level pattern'); }
  if (dev.battery_level === 'critical') { rank = Math.max(rank, 80); reasons.push('Battery critical'); }
  if (dev.battery_level === 'low') { rank = Math.max(rank, 60); reasons.push('Battery low'); }
  if (dev.stack_status === 'degraded') { rank = Math.max(rank, 55); reasons.push('Sensor down — count is a lower bound'); }
  if (dev.sensors_online != null && dev.sensors_online < 4) {
    rank = Math.max(rank, 50); reasons.push(`${dev.sensors_online} of 4 sensors online`);
  }
  if (dev.battery_mv == null && dev.battery_level == null) {
    rank = Math.max(rank, 20); reasons.push('No battery detected');
  }
  // A device with no slot is a legitimate state, not a fault: plenty of units
  // sit in an area without being tied to a serving position. It is noted at the
  // bottom of the ranking so it never competes with an actual problem.
  if (dev.location == null || dev.food_slot == null) {
    rank = Math.max(rank, 10); reasons.push('Not assigned to a position');
  }

  return { rank, level: levelOf(rank), reasons };
}

// Rank is fine-grained because it drives the SORT; `level` is only the colour,
// and colour gets three tones because the status palette cannot reliably
// separate four (see the note in app.css). Order is carried by position in
// the list, which needs no colour at all.
//
// ONE MAPPING FOR EVERY PRODUCT. An earlier cut gave scales their own --
// 'warn'/'idle'/'ok' against 'warning'/'good' -- and the CSS keys off these,
// so a scale and a stack with identical severity drew different colours on
// the same list. The vocabulary is shared because the ROSTER is shared.
function levelOf(rank) {
  return rank >= 80 ? 'critical' : rank >= 20 ? 'warning' : 'good';
}

export function compareDevices(a, b) {
  const sa = deviceSeverity(a), sb = deviceSeverity(b);
  if (sb.rank !== sa.rank) return sb.rank - sa.rank;
  return String(a.device_id).localeCompare(String(b.device_id));
}

// --- fleet roll-up ----------------------------------------------------

export function fleetSummary(devices) {
  const s = {
    total: devices.length,
    offline: 0, fault: 0, degraded: 0,
    batteryWarn: 0, sensorsDown: 0,
    awaiting: 0, inService: 0, reporting: 0,
  };
  for (const d of devices) {
    if (d.awaiting_deployment) { s.awaiting++; continue; }
    // The chip counts everything not talking when it should be — the acute
    // in-window flag and the slept-through-a-window flag alike.
    if (deviceOffline(d)) s.offline++;
    if (isFault(d)) s.fault++;
    if (isDegraded(d)) s.degraded++;
    // Hubs and legacy stacks only -- a platform's battery is its hub's.
    if (isBatteryWarn(d)) s.batteryWarn++;
    // GUARDED ON THE PRODUCT, the way deviceSeverity() already is. sensors_online
    // is the ToF array's four-up count; a scale has three cells and does not
    // populate it, so an unguarded `< 4` would have called every load cell
    // sensor-down the moment the column existed.
    if (isWeighed(d) ? false : (d.sensors_online != null && d.sensors_online < 4)) s.sensorsDown++;
    if (d.in_service) s.inService++;
    // "Reporting" means TALKING: not flagged by either server offline flag.
    // The old count was `d.reported` — has EVER reported — which among
    // non-awaiting devices (awaiting IS never-reported) was a tautology: the
    // chip read "15/15" while 13 of the 15 were offline.
    if (!deviceOffline(d)) s.reporting++;
  }
  return s;
}

/** Site-wide service state, taken from the devices rather than the browser clock. */
export function serviceState(devices) {
  const live = devices.filter(d => !d.awaiting_deployment);
  const inService = live.some(d => d.in_service);
  const meal = live.find(d => d.current_meal)?.current_meal || null;
  const tz = live.find(d => d.timezone)?.timezone || DEFAULT_TZ;
  return { inService, meal, tz };
}

// --- time -------------------------------------------------------------

export function fmtRelative(iso, now = Date.now()) {
  if (!iso) return 'never';
  const ms = now - new Date(iso).getTime();
  if (!Number.isFinite(ms)) return '—';
  if (ms < 0) return 'just now';
  const s = Math.round(ms / 1000);
  if (s < 45) return `${s}s ago`;
  const m = Math.round(s / 60);
  if (m < 60) return `${m}m ago`;
  const h = Math.floor(m / 60);
  if (h < 24) return `${h}h ${m % 60}m ago`;
  const d = Math.floor(h / 24);
  return `${d}d ${h % 24}h ago`;
}

export function fmtClock(iso, tz = DEFAULT_TZ) {
  if (!iso) return '—';
  return new Intl.DateTimeFormat('en-GB', {
    hour: '2-digit', minute: '2-digit', timeZone: tz,
  }).format(new Date(iso));
}

export function fmtDateTime(iso, tz = DEFAULT_TZ) {
  if (!iso) return '—';
  return new Intl.DateTimeFormat('en-GB', {
    day: '2-digit', month: 'short', hour: '2-digit', minute: '2-digit',
    second: '2-digit', timeZone: tz,
  }).format(new Date(iso));
}

export function fmtDay(dateStr) {
  if (!dateStr) return '—';
  const d = new Date(`${dateStr}T00:00:00`);
  return new Intl.DateTimeFormat('en-GB', { day: '2-digit', month: 'short' }).format(d);
}

export function fmtUptime(seconds) {
  if (seconds == null) return '—';
  const s = Number(seconds);
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
  if (d) return `${d}d ${h}h`;
  if (h) return `${h}h ${m}m`;
  return `${m}m ${s % 60}s`;
}

/** Today's SERVICE date in the site's timezone — not the browser's.
 *  A 21:00 dinner in Asia/Kolkata is already the next UTC day. */
export function serviceDate(tz = DEFAULT_TZ, at = new Date()) {
  return new Intl.DateTimeFormat('en-CA', {
    year: 'numeric', month: '2-digit', day: '2-digit', timeZone: tz,
  }).format(at);
}

export function addDays(dateStr, n) {
  const d = new Date(`${dateStr}T12:00:00Z`);
  d.setUTCDate(d.getUTCDate() + n);
  return d.toISOString().slice(0, 10);
}

/** Which meal the local clock falls in, used only to pick a sensible default in
 *  the menu editor. `current_meal` from the server is authoritative elsewhere. */
export function mealAtLocalClock(tz = DEFAULT_TZ, at = new Date()) {
  const hhmm = new Intl.DateTimeFormat('en-GB', {
    hour: '2-digit', minute: '2-digit', hour12: false, timeZone: tz,
  }).format(at);
  for (const w of SERVICE_WINDOWS) {
    if (hhmm >= w.start && hhmm <= w.end) return w.meal;
  }
  // Between meals: offer the next one up, so an admin sets rather than reviews.
  for (const w of SERVICE_WINDOWS) if (hhmm < w.start) return w.meal;
  return 'Breakfast';
}

export function positionLabel(dev) {
  if (dev.location == null) return 'Unassigned';
  const area = LOCATION_NAMES[dev.location] || dev.location;
  return dev.food_slot == null ? area : `${area} · slot ${dev.food_slot}`;
}
