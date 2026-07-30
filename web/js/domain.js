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
export function deviceStack(dev) {
  if (!dev.reported || dev.stack_count == null) {
    return { kind: 'none', text: '—', note: 'No reading' };
  }
  if (dev.stack_status === 'discontiguous') {
    return { kind: 'fault', text: '!', note: 'Impossible reading — check the sensors' };
  }
  if (dev.stack_status === 'degraded') {
    return { kind: 'bound', value: dev.stack_count, text: `≥${dev.stack_count}`,
             note: 'A sensor is down — this is a lower bound' };
  }
  return { kind: 'count', value: dev.stack_count, text: String(dev.stack_count), note: '' };
}

/**
 * What to render for one DISH POSITION, which may be served by several stacks.
 * The arithmetic is the view's — `bowls_trusted` already sums only the devices
 * reporting `ok`. Nothing is re-summed here.
 */
export function slotStock(slot) {
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
    severity: null,     // filled in by stockSeverity(), which needs the ok-count
    headline: String(trusted),
    note: '',
  };
}

/**
 * Colour band for a dish position.
 *
 * A 0–4 count has very low resolution, so the colour carries more than the
 * digit does. Normalised per stack, because a position with three stacks holds
 * three times as much at the same "fullness" — comparing raw totals across
 * positions would paint Mahatma red while it is as full as Darshanarthi.
 */
export function stockSeverity(trusted, okStacks) {
  if (trusted == null || !okStacks) return 'idle';
  const perStack = trusted / okStacks;
  if (perStack <= 1) return 'critical';
  if (perStack <= 2) return 'warning';
  return 'good';
}

// --- assignment validity ---------------------------------------------

export const SERVING_AREAS = new Set(['D', 'M', 'T']);

/**
 * A device parked in a serving area with no slot number.
 *
 * `slot_overview` groups by (location, food_slot) and drops rows where either
 * is null, so such a device contributes nothing to any dish position: its
 * bowls disappear from the primary screen with no error and no gap. This is
 * how Darshanarthi slot 1 came to report a capacity of 4 with three stacks
 * physically standing on it.
 *
 * Reserved (`R`) with no slot is correct, not half-assigned -- a spare
 * occupies no serving position.
 */
export function isHalfAssigned(dev) {
  return SERVING_AREAS.has(dev.location) && dev.food_slot == null;
}

/**
 * Is this (location, food_slot) pair sane? Returns null when fine, otherwise
 * the reason. Used to gate the Devices form so the state above cannot be
 * created in the first place.
 */
export function assignmentError(location, foodSlot) {
  if (location == null) {
    return foodSlot == null ? null : 'Pick an area, or clear the slot too.';
  }
  if (location === 'R') {
    return foodSlot == null ? null : 'Reserved units occupy no serving position — clear the slot.';
  }
  if (foodSlot == null) {
    return 'Pick a slot. Without one this device is left out of its dish position’s total.';
  }
  return null;
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
export function deviceSeverity(dev) {
  const reasons = [];
  let rank = 0;

  if (dev.awaiting_deployment) {
    return { rank: -1, level: 'idle', reasons: ['Awaiting deployment'] };
  }
  if (dev.offline) { rank = Math.max(rank, 100); reasons.push('Offline during service'); }
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
  // A half-assignment is not cosmetic. slot_overview filters `food_slot is not
  // null`, so this device's bowls are silently absent from its dish position's
  // total -- the number on the primary screen is simply wrong, with nothing on
  // screen to say so. Ranked above a low battery for that reason.
  if (isHalfAssigned(dev)) {
    rank = Math.max(rank, 70);
    reasons.push('Area set but no slot — its bowls are missing from stock totals');
  } else if (dev.location == null) {
    rank = Math.max(rank, 10); reasons.push('Not assigned to a position');
  }

  // Rank is fine-grained because it drives the SORT; `level` is only the colour,
  // and colour gets three tones because the status palette cannot reliably
  // separate four (see the note in app.css). Order is carried by position in
  // the list, which needs no colour at all.
  const level = rank >= 80 ? 'critical'
              : rank >= 20 ? 'warning'
              : 'good';
  return { rank, level, reasons };
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
    batteryWarn: 0, sensorsDown: 0, halfAssigned: 0,
    awaiting: 0, inService: 0, reporting: 0,
  };
  for (const d of devices) {
    // Counted before the awaiting_deployment skip: a misconfigured spare is
    // still misconfigured, and it is cheaper to fix before it is installed.
    if (isHalfAssigned(d)) s.halfAssigned++;
    if (d.awaiting_deployment) { s.awaiting++; continue; }
    if (d.offline) s.offline++;
    if (d.stack_status === 'discontiguous') s.fault++;
    if (d.stack_status === 'degraded') s.degraded++;
    if (d.battery_level === 'low' || d.battery_level === 'critical') s.batteryWarn++;
    if (d.sensors_online != null && d.sensors_online < 4) s.sensorsDown++;
    if (d.in_service) s.inService++;
    if (d.reported) s.reporting++;
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
