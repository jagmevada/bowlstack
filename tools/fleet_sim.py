#!/usr/bin/env python3
"""Bowlstack fleet simulator -- load-cell BUFFER platforms, in kilograms.

WHY THIS EXISTS
---------------
The front-end cannot be built against one prototype on a bench: a stock
dashboard is only meaningful with several areas populated, a health view only
with something unhealthy in it, and a history chart only with history. This
produces all three, through the SAME write path the firmware uses.

WHAT IT SIMULATES
-----------------
Every BWL-xxx unit is a 200 kg buffer platform (devices.kind = 'buffer') holding
up to four bowls, each 14-18 kg of food plus the 2.5 kg bowl itself. It reports
what the LDC-001 panel's buffer channel reports: weight_g is FOOD (gross minus
bowls x 2500 g), gross_g is everything on the platform, bowls/bowls_confirmed is
the bowl tracker. It never sends a bowl-stack column and never writes
status_events -- see tools/README.md for the contract.

WHAT IT DELIBERATELY DOES NOT DO
--------------------------------
It does not bypass the schema. Every write goes through PostgREST with the anon
key, against the same policies, grants, CHECK constraints and triggers as a real
device. If this script can write it, a device can; if the schema rejects it, the
device would have been rejected too. A service_role key would prove nothing, so
one is refused (the JWT's role claim is decoded, not guessed at).

It does not write to `devices`, and it never writes an id somebody else owns:
BWL-001..003 belong to the LDC-001 panel's buffer bank, and BWL-025..032 are
reserved (writing one permanently retires it from awaiting_deployment). Those
ids are refused even when passed with --ids.

It must only run AFTER supabase/cutover_buffers.sql: before that the BWL rows are
still kind 'stack' and the database rejects every buffer write to them.

CLOCKS
------
`weight_samples.recorded_at` is computed server-side as `now() - age_ms`, exactly
as for a device with no RTC. Backfill therefore sends an AGE, not a timestamp.
The server clamps age_ms at 7 days, which is the hard limit on backfill.

USAGE
-----
    python tools/fleet_sim.py --once --dry-run      # payloads only, no network
    python tools/fleet_sim.py --once
    python tools/fleet_sim.py --backfill 5
    python tools/fleet_sim.py --live

Credentials come from the environment, or from include/secret.h (gitignored):
    BOWLSTACK_SUPABASE_URL, BOWLSTACK_ANON_KEY
--dry-run reads neither and opens no connection.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import random
import re
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

FIRMWARE = "sim-kg 2.0"  # says "simulated" wherever the dashboard shows firmware

# BWL-004..024: the deployed buffer positions minus the three the panel owns.
DEFAULT_IDS = [f"BWL-{i:03d}" for i in range(4, 25)]

# Never written, even when named with --ids. Two writers on one row interleave two
# boot_ids, which the stale-write guard and the dashboard both read as nonsense;
# and a write to a reserved row sets `reported` permanently.
PANEL_IDS = {"BWL-001", "BWL-002", "BWL-003"}  # buffer_bank.cpp default slots B1..B3
RESERVED_IDS = {f"BWL-{i:03d}" for i in range(25, 33)}

# --- the platform -------------------------------------------------------------
# One full bowl: 14-18 kg of food in a 2.5 kg bowl (owner, 2026-10-03) -- the same
# figures include/load_scale.h's BowlConfig is built on.
BOWL_DRY_G = 2500
BOWL_FOOD_G = (14000.0, 18000.0)
MAX_BOWLS = 4
NOISE_G = 30.0
# ~20.7 counts/g is a 200 kg cell on the NAU7802 at the panel's gain.
CPG = 20.7
# What a saturated converter returns: the NAU7802's positive full scale.
SATURATED_COUNTS = 8_388_607
# One unit is never calibrated, so a permanently partial slot (T3 in
# assign_devices.sql) exists to build the '>= X' path against.
UNCALIBRATED_ID = "BWL-019"

# A buffer moves in WHOLE BOWLS (owner, 2026-10-03): full bowls go on, and a FULL
# bowl is carried to the counter, where the serving -- the gradual drain -- happens.
# Minutes between bowls leaving, per unit; each bowl is a 16.5-20.5 kg step the
# real tracker sees (its threshold is 10 kg).
BOWL_EVERY_MIN = (6.0, 12.0)
# Per-minute probabilities.
# With <= 1 bowl left: ~5 min to a restock, against a bowl leaving every 6-12 min,
# so a platform runs empty now and then (0.0 kg = refill me) but is mostly stocked.
# 0.05 left it empty for 38% of a 7-day backfill once bowls left whole.
P_RESTOCK = 0.2
P_NO_CELLS = 0.002
P_OVER_RANGE = 0.001
P_FAULT_CLEARS = 0.15

# --- the firmware's cadence (src/loadcell/scale_telemetry.cpp) ----------------
STEP_S = 5               # one observation; equal to the 5 s history floor
LIVE_INTERVAL_S = 20     # the heartbeat
OFFLINE_AFTER_S = 40     # public.offline_after(); slower than this flaps offline
SAMPLE_PERIOD_S = 120    # history heartbeat
WEIGHT_EVENT_G = 250     # a move worth a history row

# Server-side clamp in tg_weight_samples_stamp(). Nothing older can be placed.
AGE_MAX_MS = 604_800_000

# Matches `check (battery_mv between 0 and 6000)` and the firmware's
# config::BATTERY_PUBLISH_MAX_MV.
BATTERY_PUBLISH_MAX_MV = 6000

# ponytail: rows per POST, so a backfill window (~4000 rows) is not one 1 MB body.
POST_CHUNK = 500

# Fleet service windows, from schema.sql, in LOCAL wall-clock time. Devices are
# powered ONLY during these, which is why absence of data outside them is normal.
SERVICE_WINDOWS = [
    ("breakfast", 6 * 60, 9 * 60),
    ("lunch", 11 * 60 + 30, 14 * 60),
    ("dinner", 18 * 60 + 30, 21 * 60),
]

# ---------------------------------------------------------------------------
# Battery model -- mirrors include/battery_soc.h and the config.h thresholds.
#
# Duplicated rather than approximated on purpose. The front-end consumes BANDS,
# so simulated data is only useful if the bands move the way real ones do --
# including the hysteresis, which is precisely what stops them oscillating.
# ---------------------------------------------------------------------------
SOC_CURVE = [
    (4159, 100.0), (4095, 94.1), (4064, 88.2), (4018, 82.4),
    (3956, 76.5), (3898, 70.6), (3832, 64.7), (3774, 58.8),
    (3714, 52.9), (3662, 47.1), (3621, 41.2), (3583, 35.3),
    (3522, 29.4), (3471, 23.5), (3380, 17.6), (3250, 11.8),
    (3050, 5.9), (2750, 0.0),
]

BAT_CRITICAL_TO_LOW_UP = 15.0
BAT_LOW_TO_CRITICAL_DOWN = 10.0
BAT_LOW_TO_MEDIUM_UP = 40.0
BAT_MEDIUM_TO_LOW_DOWN = 35.0
BAT_MEDIUM_TO_GOOD_UP = 75.0
BAT_GOOD_TO_MEDIUM_DOWN = 70.0


def soc_from_mv(mv: int) -> float:
    """Piecewise-linear over the measured curve. Clamps rather than extrapolates."""
    if mv >= SOC_CURVE[0][0]:
        return 100.0
    if mv <= SOC_CURVE[-1][0]:
        return 0.0
    for i in range(1, len(SOC_CURVE)):
        if mv >= SOC_CURVE[i][0]:
            hi_mv, hi_soc = SOC_CURVE[i - 1]
            lo_mv, lo_soc = SOC_CURVE[i]
            span = hi_mv - lo_mv
            if span <= 0:
                return lo_soc
            return lo_soc + (hi_soc - lo_soc) * ((mv - lo_mv) / span)
    return 0.0


def _crossed(soc: float, over: bool, up: float, down: float) -> bool:
    """One boundary, two thresholds. `over` is the current side -- the history
    that makes this a Schmitt trigger rather than a comparison."""
    return soc >= down if over else soc >= up


def band_with_hysteresis(soc: float, current: str | None) -> str:
    """Same state machine as battery::Monitor. `current` None means the first
    look, which uses the falling (nominal) edges since there is no history yet."""
    fresh = current is None
    over_crit = fresh or current in ("low", "medium", "good")
    over_low = fresh or current in ("medium", "good")
    over_med = fresh or current == "good"

    over_crit = _crossed(soc, over_crit, BAT_CRITICAL_TO_LOW_UP, BAT_LOW_TO_CRITICAL_DOWN)
    over_low = _crossed(soc, over_low, BAT_LOW_TO_MEDIUM_UP, BAT_MEDIUM_TO_LOW_DOWN)
    over_med = _crossed(soc, over_med, BAT_MEDIUM_TO_GOOD_UP, BAT_GOOD_TO_MEDIUM_DOWN)

    if over_med:
        return "good"
    if over_low:
        return "medium"
    if over_crit:
        return "low"
    return "critical"


# ---------------------------------------------------------------------------
# Credentials
# ---------------------------------------------------------------------------
def key_role(key: str) -> str | None:
    """The role a Supabase key acts as. A legacy key is a JWT whose payload says
    so in its `role` claim -- base64, which is why searching the raw key for the
    text "service_role" never matched anything. New-style keys say it in their
    prefix. None means the format is not one we know."""
    if key.startswith("sb_secret_"):
        return "service_role"
    if key.startswith("sb_publishable_"):
        return "anon"
    try:
        payload = key.split(".")[1]
        claims = json.loads(base64.urlsafe_b64decode(payload + "=" * (-len(payload) % 4)))
        return claims.get("role")
    except (IndexError, ValueError):
        return None


def load_credentials() -> tuple[str, str]:
    url = os.environ.get("BOWLSTACK_SUPABASE_URL")
    key = os.environ.get("BOWLSTACK_ANON_KEY")

    if not (url and key):
        # include/secret.h is gitignored, so reading it leaks nothing that is not
        # already on this machine. It saves re-typing an anon key by hand.
        secret = REPO / "include" / "secret.h"
        if secret.exists():
            text = secret.read_text(encoding="utf-8", errors="replace")
            if not url:
                m = re.search(r'define\s+SUPABASE_URL\s+"([^"]+)"', text)
                url = m.group(1) if m else None
            if not key:
                m = re.search(r'define\s+SUPABASE_ANON_KEY\s+"([^"]+)"', text)
                key = m.group(1) if m else None

    if not (url and key):
        sys.exit(
            "no credentials.\n"
            "  set BOWLSTACK_SUPABASE_URL and BOWLSTACK_ANON_KEY,\n"
            "  or populate include/secret.h"
        )

    # Supabase shows several URLs in its dashboard and only one is the API
    # origin. Normalise the same way the firmware's apiBase() does, so a unit
    # configured with the REST endpoint still works.
    url = url.rstrip("/")
    if url.endswith("/rest/v1"):
        url = url[: -len("/rest/v1")].rstrip("/")

    if key_role(key) == "service_role":
        sys.exit(
            "that is a service_role key. Refusing.\n"
            "service_role carries BYPASSRLS, so a test using it proves nothing\n"
            "about whether a real device could write."
        )
    return url, key


# ---------------------------------------------------------------------------
# The contract every payload must satisfy -- the database enforces most of it,
# but a violation here is a simulator bug, and it should fail on the bench
# (and in --dry-run) rather than as a 400 halfway through a backfill.
# ---------------------------------------------------------------------------
BUFFER_STATES = {"ok", "uncalibrated", "untared", "settling", "no_cells", "over_range"}
STACK_KEYS = {"stack_count", "stack_status", "levels", "sensors_ok", "sensors_online"}


def check_payload(p: dict) -> None:
    ok = p["weight_state"] == "ok"
    assert p["weight_state"] in BUFFER_STATES, p  # a buffer never sends cells_partial
    for k in ("weight_g", "gross_g", "bowls"):
        assert (p[k] is not None) == ok, (k, p)
    assert (p["bowls_confirmed"] is None) == (p["bowls"] is None), p
    assert not (STACK_KEYS & p.keys()), p
    assert p["manual_fill_pct"] is None and p.get("manual_fill_age_s") is None, p
    assert p.get("charging") is None, p
    assert p["cells_online"] in (0, 1), p
    assert 0 <= p["battery_mv"] <= BATTERY_PUBLISH_MAX_MV, p
    if ok:
        assert all(type(p[k]) is int for k in ("weight_g", "gross_g", "bowls")), p
        assert -5000 <= p["weight_g"] <= 250000, p
        assert -5000 <= p["gross_g"] <= 260000, p
        assert 0 <= p["bowls"] <= 8, p
    if "age_ms" in p:
        assert 0 <= p["age_ms"] <= AGE_MAX_MS and p["seq"] >= 0, p


# ---------------------------------------------------------------------------
# Simulated buffer platform
# ---------------------------------------------------------------------------
def new_boot_id() -> int:
    # From the OS, NOT the seeded RNG. A seeded boot_id repeated on every run with
    # the same seed: seq restarted at 0 under an old boot_id, every batch hit 23505,
    # and the stale-write guard silently skipped PATCHes whose uptime went backwards.
    return int.from_bytes(os.urandom(4), "little") & 0x7FFFFFFF or 1


@dataclass
class SimBuffer:
    device_id: str
    rng: random.Random

    t: float = 0.0            # epoch seconds of the last observation
    boot_t: float = 0.0
    boot_id: int = 0
    seq: int = 0
    mac: str = ""

    bowls: list[float] = field(default_factory=list)  # food g in each bowl
    confirmed: bool = False
    fault: str | None = None  # None | 'no_cells' | 'over_range'
    settle_until: float = 0.0
    gross_g: int = 0          # the last reading, noise included

    bowl_every_min: float = 0.0
    on_mains: bool = False
    cell_mv: int = 4100
    band: str | None = None

    queue: list[tuple] = field(default_factory=list)
    last: tuple | None = None  # (state/bowls key, weight_g, t) of the last enqueued sample

    def __post_init__(self) -> None:
        # Seeded, so the same id is the same "board" on every run: one MAC, one
        # serving rate, one starting stack.
        self.mac = ":".join(f"{self.rng.randrange(256):02x}" for _ in range(6))
        self.bowl_every_min = self.rng.uniform(*BOWL_EVERY_MIN)
        self.on_mains = self.rng.random() < 0.25
        self.cell_mv = self.rng.randint(4100, 4159) if self.on_mains else self.rng.randint(3850, 4150)
        self.band = band_with_hysteresis(soc_from_mv(self.cell_mv), None)
        self.bowls = [self.rng.uniform(*BOWL_FOOD_G) for _ in range(self.rng.randint(1, MAX_BOWLS))]

    # -- state evolution ---------------------------------------------------
    def reboot(self, t: float) -> None:
        """A power-up: new boot_id, seq from 0, a few seconds of settling. The
        bowls stay where they are, but the remembered count is UNCONFIRMED until
        the next load/unload or an empty platform -- the firmware cannot know
        what was carried on or off while it was off."""
        self.boot_id = new_boot_id()
        self.seq = 0
        self.t = self.boot_t = t
        self.settle_until = t + self.rng.uniform(3, 10)
        self.fault = None
        self.confirmed = not self.bowls
        self.last = None
        self.queue.clear()
        self._measure()
        self._observe()

    def advance_to(self, t_end: float) -> None:
        while self.t + STEP_S <= t_end:
            self.t += STEP_S
            self._step(STEP_S / 60.0)
            self._observe()

    def _step(self, minutes: float) -> None:
        # A FULL bowl carried to the counter -- the buffer's only way down. A 16.5-20.5
        # kg step, which the real tracker counts as an unload, so the count stays
        # confirmed. ponytail: a Poisson departure per unit; a service-shaped rate if
        # Master's "Empty about" ever needs testing against rush hours.
        if self.bowls and self.rng.random() < minutes / self.bowl_every_min:
            self.bowls.pop()
            self.confirmed = True

        if len(self.bowls) <= 1 and self.rng.random() < P_RESTOCK * minutes:
            n = min(self.rng.randint(1, 3), MAX_BOWLS - len(self.bowls))
            self.bowls += [self.rng.uniform(*BOWL_FOOD_G) for _ in range(n)]
            self.confirmed = True

        if self.fault is None:
            r = self.rng.random()
            if r < P_NO_CELLS * minutes:
                self.fault = "no_cells"
            elif r < (P_NO_CELLS + P_OVER_RANGE) * minutes:
                self.fault = "over_range"
        elif self.rng.random() < P_FAULT_CLEARS * minutes:
            if self.fault == "no_cells":
                self.confirmed = False  # the tracker was blind meanwhile
            self.fault = None

        if not self.bowls:
            self.confirmed = True  # an empty platform IS a count, of zero

        if not self.on_mains:
            # ~8 h of service takes a cell from full to roughly 45%.
            self.cell_mv = max(3000, int(self.cell_mv - self.rng.uniform(0.9, 1.4) * minutes))
            self.band = band_with_hysteresis(soc_from_mv(self.cell_mv), self.band)
        self._measure()

    def _measure(self) -> None:
        true_g = sum(self.bowls) + len(self.bowls) * BOWL_DRY_G
        self.gross_g = round(true_g + self.rng.uniform(-NOISE_G, NOISE_G))

    def charge_overnight(self) -> None:
        self.cell_mv = self.rng.randint(4080, 4159)
        self.band = band_with_hysteresis(soc_from_mv(self.cell_mv), self.band)

    def state(self) -> str:
        """The firmware's ladder (load_scale.cpp): the fault nearest the hardware
        wins, because that is the one somebody can act on."""
        if self.fault == "no_cells":
            return "no_cells"
        if self.t < self.settle_until:
            return "settling"
        if self.fault == "over_range":
            return "over_range"
        if self.device_id == UNCALIBRATED_ID:
            return "uncalibrated"
        return "ok"

    # -- payload construction ---------------------------------------------
    def fields(self) -> dict:
        """The columns device_status and weight_samples share. A NUMBER only when
        the state is ok -- the database's own CHECK -- and gross/bowls follow the
        same rule, so a dead cell cannot keep claiming three confirmed bowls."""
        state = self.state()
        ok = state == "ok"
        n = len(self.bowls)
        if state in ("no_cells", "settling"):
            counts = None  # no window of samples: 0 would be a reading
        elif state == "over_range":
            counts = SATURATED_COUNTS
        else:
            counts = round(self.gross_g * CPG)
        return {
            "weight_state": state,
            "weight_g": self.gross_g - n * BOWL_DRY_G if ok else None,  # FOOD, always
            "gross_g": self.gross_g if ok else None,
            "bowls": n if ok else None,
            "bowls_confirmed": self.confirmed if ok else None,
            "cells_online": 0 if state == "no_cells" else 1,
            "net_counts": counts,
            "counts_per_gram": None if self.device_id == UNCALIBRATED_ID else CPG,
            "battery_mv": min(self.cell_mv, BATTERY_PUBLISH_MAX_MV),
            "battery_level": self.band,
            "manual_fill_pct": None,  # the counter's trial knob; never a buffer's
            "firmware": FIRMWARE,
        }

    def _observe(self) -> None:
        """The firmware's history rule: boot; a state or bowl-count change (never
        throttled); a move of >= 250 g since the last ENQUEUED sample; or 120 s
        without one. STEP_S is the 5 s floor, so the floor holds by construction."""
        f = self.fields()
        key = (f["weight_state"], f["bowls"], f["bowls_confirmed"])
        w = f["weight_g"]
        if self.last is None:
            reason = "boot"
        else:
            last_key, last_w, last_t = self.last
            if key != last_key:
                reason = "change"
            elif w is not None and last_w is not None and abs(w - last_w) >= WEIGHT_EVENT_G:
                reason = "change"
            elif self.t - last_t >= SAMPLE_PERIOD_S:
                reason = "periodic"
            else:
                return
        self.last = (key, w, self.t)
        self.queue.append((self.t, self.seq, reason, f))
        self.seq += 1

    def take_samples(self, now: float) -> list[dict]:
        rows = [{
            "device_id": self.device_id,
            "boot_id": self.boot_id,
            "seq": seq,
            "age_ms": max(0, min(int((now - at) * 1000), AGE_MAX_MS)),
            "reason": reason,
            **f,
        } for at, seq, reason, f in self.queue]
        self.queue.clear()
        return rows

    def status(self) -> dict:
        return {
            "boot_id": self.boot_id,
            "uptime_s": int(self.t - self.boot_t),
            "mac": self.mac,
            **self.fields(),
            "external_power": self.on_mains,
            # NULL, not false: the panel host cannot read its charger.
            "charging": None,
            "manual_fill_age_s": None,
        }


# ---------------------------------------------------------------------------
# Transport -- the same two calls the firmware makes, and nothing else.
# ---------------------------------------------------------------------------
class Uplink:
    def __init__(self, url: str, key: str, dry_run: bool = False):
        self.dry_run = dry_run
        self.samples_sent = 0
        self.patches_sent = 0
        self.errors: list[str] = []
        if dry_run:
            return  # no session at all, so a dry run cannot reach the network
        try:
            import requests
        except ImportError:
            sys.exit("needs `requests`:  python -m pip install requests")
        self.base = url
        self.session = requests.Session()
        self.session.headers.update({
            "apikey": key,
            "Authorization": f"Bearer {key}",
            "Content-Type": "application/json",
        })

    def post_samples(self, rows: list[dict]) -> bool:
        for r in rows:
            check_payload(r)
        if not rows:
            return True
        if self.dry_run:
            print(json.dumps(rows[:2], indent=2))
            print(f"  ... {len(rows)} weight_samples (dry run)")
            self.samples_sent += len(rows)
            return True

        ok = True
        for i in range(0, len(rows), POST_CHUNK):
            chunk = rows[i:i + POST_CHUNK]
            r = self.session.post(
                f"{self.base}/rest/v1/weight_samples",
                headers={"Prefer": "return=minimal"},
                data=json.dumps(chunk),
                timeout=30,
            )
            if 200 <= r.status_code < 300:
                self.samples_sent += len(chunk)
            elif "23505" in r.text:
                # Already stored -- success for an idempotent retry, exactly as
                # the firmware treats it.
                pass
            else:
                self._record("POST weight_samples", r)
                ok = False
        return ok

    def patch_status(self, device_id: str, body: dict) -> bool:
        check_payload(body)
        if self.dry_run:
            print(f"  PATCH {device_id} {json.dumps(body)}")
            self.patches_sent += 1
            return True

        r = self.session.patch(
            f"{self.base}/rest/v1/device_status",
            params={"device_id": f"eq.{device_id}"},
            headers={"Prefer": "return=minimal,count=exact"},
            data=json.dumps(body),
            timeout=30,
        )
        if not (200 <= r.status_code < 300):
            self._record(f"PATCH device_status {device_id}", r)
            return False

        # A PATCH matching no rows is a perfectly successful 204. Without the
        # count there is no way to tell "reported" from "wrote nothing, forever".
        if r.headers.get("Content-Range", "").endswith("/0"):
            self._record(
                f"PATCH device_status {device_id}: matched 0 rows -- not registered "
                f"(supabase/register_devices.sql), or skipped by the stale-write guard",
                r,
            )
            return False
        self.patches_sent += 1
        return True

    def _record(self, what: str, r) -> None:
        msg = f"{what} -> {r.status_code} {r.text[:200]}"
        if "kind" in r.text or "column" in r.text:
            msg += ("\n      (is supabase/migrate_buffer.sql applied, and "
                    "cutover_buffers.sql? see tools/README.md)")
        if msg not in self.errors:
            self.errors.append(msg)
        print(f"  ERROR {msg}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Modes
# ---------------------------------------------------------------------------
def make_fleet(ids: list[str], seed: int) -> list[SimBuffer]:
    # Seeded per device NUMBER, so BWL-010 behaves the same whichever subset is
    # run: a front-end developer comparing two screenshots should not be fighting
    # fresh randomness. boot_id is the one thing that is not seeded.
    return [SimBuffer(i, random.Random(seed + int(i[4:]))) for i in ids]


def run_round(fleet: list[SimBuffer], up: Uplink, now: float, boot: bool, span: float) -> int:
    """One heartbeat: history first (oldest first, as the firmware flushes), then
    current state. `boot` powers every unit on `span` seconds ago."""
    rows = []
    for d in fleet:
        if boot:
            d.reboot(now - span)
        d.advance_to(now)
        rows += d.take_samples(now)
    up.post_samples(rows)
    for d in fleet:
        up.patch_status(d.device_id, d.status())
    return len(rows)


def run_backfill(fleet: list[SimBuffer], up: Uplink, days: int) -> None:
    """Replays `days` of meal service into weight_samples, positioned by AGE --
    the only time reference the write path has."""
    max_days = AGE_MAX_MS // 86_400_000
    if days > max_days:
        print(f"  age_ms is clamped at {max_days} days server-side; using {max_days}")
        days = max_days

    # LOCAL time, because the service windows are local meal times. This used to
    # take UTC and place breakfast at 06:00 UTC.
    now = datetime.now().astimezone()
    total = 0

    for day_offset in range(days, 0, -1):
        day = now - timedelta(days=day_offset)
        for label, start_min, end_min in SERVICE_WINDOWS:
            start = day.replace(hour=start_min // 60, minute=start_min % 60,
                                second=0, microsecond=0)
            if (now - start).total_seconds() * 1000 > AGE_MAX_MS:
                # Older than the server can place: clamping would pile the whole
                # window onto one instant seven days ago.
                continue
            end = start + timedelta(minutes=end_min - start_min)
            rows = []
            for d in fleet:
                # A device is powered on for each service, so each one is a new
                # boot -- which is what makes seq restart from zero.
                d.reboot(start.timestamp())
                d.advance_to(end.timestamp())
                rows += d.take_samples(now.timestamp())
                d.charge_overnight()
            if up.post_samples(rows):
                total += len(rows)
            print(f"  {day.date()} {label:9s} {len(rows):5d} samples")

    # Leave every row's current state consistent with the end of the replay.
    for d in fleet:
        up.patch_status(d.device_id, d.status())
    print(f"  backfilled {total} samples across {days} days")


def in_service_window(at: datetime) -> str | None:
    mins = at.hour * 60 + at.minute
    for label, start, end in SERVICE_WINDOWS:
        if start <= mins < end:
            return label
    return None


def run_live(fleet: list[SimBuffer], up: Uplink, interval: int, respect_window: bool) -> None:
    if interval >= OFFLINE_AFTER_S:
        print(f"  WARNING: {interval}s rounds are slower than offline_after() "
              f"({OFFLINE_AFTER_S}s); rows will flap offline")
    print(f"  live, {interval}s per round, Ctrl-C to stop")
    round_no = 0
    powered = False
    try:
        while True:
            round_no += 1
            started = time.time()
            window = in_service_window(datetime.now())
            if respect_window and window is None:
                powered = False  # switched off between services; next one is a boot
                print(f"  round {round_no}: outside service hours, idle "
                      f"(--always to override)")
                time.sleep(interval)
                continue

            n = run_round(fleet, up, started, boot=not powered, span=interval)
            powered = True
            print(f"  round {round_no} ({window or 'off-hours'}): "
                  f"{n} samples, {len(fleet)} status rows")
            time.sleep(max(0.0, interval - (time.time() - started)))
    except KeyboardInterrupt:
        print("\n  stopped")


def parse_ids(text: str) -> list[str]:
    # Deduplicated: the same id twice would be two writers on one row.
    ids = list(dict.fromkeys(s.strip().upper() for s in text.split(",") if s.strip()))
    if not ids:
        sys.exit("--ids: no ids given")
    for i in ids:
        if not re.fullmatch(r"BWL-\d{3}", i):
            sys.exit(f"{i}: only BWL-NNN buffer ids can be simulated")
        if i in PANEL_IDS:
            sys.exit(f"{i} belongs to the LDC-001 panel's buffer bank. Refusing.")
        if i in RESERVED_IDS:
            sys.exit(f"{i} is reserved; writing it permanently retires it from "
                     f"awaiting_deployment. Refusing.")
    return ids


# ---------------------------------------------------------------------------
def main() -> int:
    p = argparse.ArgumentParser(
        description="Simulate the Bowlstack buffer fleet (kg) against Supabase.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument("--once", action="store_true",
                      help="one heartbeat for all devices, from power-up")
    mode.add_argument("--backfill", type=int, metavar="DAYS",
                      help="replay DAYS of meal service into weight_samples (max 7)")
    mode.add_argument("--live", action="store_true", help="run continuously")

    p.add_argument("--interval", type=int, default=LIVE_INTERVAL_S,
                   help=f"seconds per round in --live (default {LIVE_INTERVAL_S}; "
                        f"keep it under {OFFLINE_AFTER_S})")
    p.add_argument("--always", action="store_true",
                   help="in --live, report outside service hours too")
    p.add_argument("--ids", type=parse_ids, default=DEFAULT_IDS,
                   help="comma-separated BWL ids (default BWL-004..BWL-024)")
    p.add_argument("--seed", type=int, default=20260726,
                   help="RNG seed, for reproducible runs")
    p.add_argument("--dry-run", action="store_true",
                   help="print payloads; read no credentials, open no connection")
    args = p.parse_args()

    if args.dry_run:
        url = key = ""
        print("bowlstack fleet sim -- DRY RUN, nothing will be sent")
    else:
        url, key = load_credentials()
        print(f"bowlstack fleet sim -> {url}")

    fleet = make_fleet(args.ids, args.seed)
    print(f"  {len(fleet)} buffers: {fleet[0].device_id} .. {fleet[-1].device_id}")

    up = Uplink(url, key, dry_run=args.dry_run)
    started = time.time()

    if args.once:
        run_round(fleet, up, time.time(), boot=True, span=LIVE_INTERVAL_S)
    elif args.backfill is not None:
        run_backfill(fleet, up, args.backfill)
    else:
        run_live(fleet, up, args.interval, respect_window=not args.always)

    print(f"\n  {up.samples_sent} samples, {up.patches_sent} status rows, "
          f"{len(up.errors)} distinct errors, {time.time() - started:.1f}s")

    if up.errors:
        print("\n  FAILURES:")
        for e in up.errors:
            print(f"    {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
