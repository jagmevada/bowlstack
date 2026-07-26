#!/usr/bin/env python3
"""Bowlstack fleet simulator -- writes plausible telemetry for all 32 devices.

WHY THIS EXISTS
---------------
The front-end is the next major piece of work, and it cannot be built against a
single prototype on a bench: a stock dashboard is only meaningful with several
areas populated, a health view is only meaningful with something unhealthy in
it, and a history chart is only meaningful with history. This produces all
three, through the SAME write path the firmware uses.

WHAT IT DELIBERATELY DOES NOT DO
--------------------------------
It does not bypass the schema. Every write goes through PostgREST with the anon
key, against the same policies, grants, CHECK constraints and triggers as a real
device. That is the point of the exercise: if this script can write it, a device
can, and if the schema rejects it the fleet would have been rejected too. Nothing
here uses the service_role key -- that would prove nothing.

It does not write to `devices`. The registry is human-managed and anon has no
grant on it, correctly. Deployment (location / food_slot) is applied separately by
an owner -- see supabase/assign_devices.sql.

CLOCKS
------
`status_events.recorded_at` is computed server-side as `now() - age_ms`, exactly
as it is for a device with no RTC. Backfill therefore works by sending an AGE,
not a timestamp, and the server supplies the reference instant. `now()` is
transaction_timestamp, so every row of one batch shares one reference and their
relative order is exact.

The server clamps age_ms at 7 days, so that is the hard limit on how far back
history can be placed.

USAGE
-----
    python tools/fleet_sim.py --once              # one round, all 32 devices
    python tools/fleet_sim.py --backfill 5        # 5 days of service history
    python tools/fleet_sim.py --live              # continuous, Ctrl-C to stop
    python tools/fleet_sim.py --once --dry-run    # print payloads, send nothing

Credentials come from the environment, or from include/secret.h (gitignored) as
a convenience:
    BOWLSTACK_SUPABASE_URL, BOWLSTACK_ANON_KEY
"""

from __future__ import annotations

import argparse
import json
import os
import random
import re
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from pathlib import Path

try:
    import requests
except ImportError:
    sys.exit("needs `requests`:  python -m pip install requests")

REPO = Path(__file__).resolve().parent.parent

DEVICE_COUNT = 32
LEVELS = 4
FIRMWARE = "0.2.0"

# Server-side clamp in tg_status_events_stamp(). Nothing older than this can be
# placed, because age_ms is how position in time is expressed.
AGE_MAX_MS = 604_800_000

# Matches `check (battery_mv between 0 and 6000)` and the firmware's
# config::BATTERY_PUBLISH_MAX_MV.
BATTERY_PUBLISH_MAX_MV = 6000

# Fleet service windows, from schema.sql. Devices are powered ONLY during these,
# which is why absence of data outside them is normal rather than a fault.
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

    if "service_role" in key or len(key) > 500:
        sys.exit(
            "that looks like a service_role key. Refusing.\n"
            "service_role carries BYPASSRLS, so a test using it proves nothing\n"
            "about whether a real device could write."
        )
    return url, key


# ---------------------------------------------------------------------------
# Simulated device
# ---------------------------------------------------------------------------
@dataclass
class SimDevice:
    device_id: str
    rng: random.Random

    boot_id: int = 0
    seq: int = 0
    mac: str = ""
    uptime_s: int = 0

    stack: int = LEVELS
    sensors_ok: list[bool] = field(default_factory=lambda: [True] * LEVELS)
    fault: str | None = None  # None | 'degraded' | 'discontiguous'

    cell_mv: int = 4100
    band: str | None = None
    charging: bool = False

    def __post_init__(self) -> None:
        self.boot_id = self.rng.getrandbits(31)
        self.mac = ":".join(f"{self.rng.randrange(256):02X}" for _ in range(6))
        self.cell_mv = self.rng.randint(3850, 4150)
        self.band = band_with_hysteresis(soc_from_mv(self.cell_mv), None)

    # -- state evolution ---------------------------------------------------
    def reboot(self) -> None:
        """New boot_id resets the sequence. This is what a device does when it
        is powered on for a service, and it is why (device_id, boot_id, seq) is
        the idempotency key rather than seq alone."""
        self.boot_id = self.rng.getrandbits(31)
        self.seq = 0
        self.uptime_s = 0
        self.stack = LEVELS
        self.fault = None
        self.sensors_ok = [True] * LEVELS

    def serve_tick(self, minutes: int) -> bool:
        """Advance by `minutes` of service. Returns True if anything a server
        would care about changed -- the same question device_status::differs()
        answers on the device."""
        before = self.wire_state()
        self.uptime_s += minutes * 60

        # Bowls are consumed, and occasionally the counter is restocked.
        if self.stack > 0 and self.rng.random() < 0.22:
            self.stack -= 1
        elif self.stack <= 1 and self.rng.random() < 0.35:
            self.stack = LEVELS

        # Faults, rare and self-clearing -- a health view is only useful if
        # something in it is occasionally unhealthy.
        if self.fault is None:
            r = self.rng.random()
            if r < 0.004:
                self.fault = "degraded"
                self.sensors_ok[self.rng.randrange(LEVELS)] = False
            elif r < 0.005:
                self.fault = "discontiguous"
        elif self.rng.random() < 0.15:
            self.fault = None
            self.sensors_ok = [True] * LEVELS

        # Discharge. ~8 h of service takes a cell from full to roughly 45%.
        drop = self.rng.uniform(0.9, 1.4) * minutes
        self.cell_mv = max(3000, int(self.cell_mv - drop))
        self.band = band_with_hysteresis(soc_from_mv(self.cell_mv), self.band)

        return self.wire_state() != before

    def charge_overnight(self) -> None:
        self.cell_mv = self.rng.randint(4080, 4159)
        self.band = band_with_hysteresis(soc_from_mv(self.cell_mv), self.band)
        self.charging = False

    # -- payload construction ---------------------------------------------
    def levels(self) -> list[str]:
        """Bottom-up f1..f4. A valid stack is contiguous from index 0, because
        bowls rest on each other and cannot float."""
        if self.fault == "discontiguous":
            # Physically impossible on purpose: a bowl above an empty level.
            # The firmware reports this as a fault and refuses to give a count.
            out = ["absent"] * LEVELS
            out[1] = "present"
            return out
        out = ["present"] * min(self.stack, LEVELS) + ["absent"] * max(0, LEVELS - self.stack)
        for i, ok in enumerate(self.sensors_ok):
            if not ok:
                out[i] = "unknown"
        return out

    def stack_status(self) -> str:
        if self.fault == "discontiguous":
            return "discontiguous"
        if self.fault == "degraded" or not all(self.sensors_ok):
            return "degraded"
        return "ok"

    def reported_count(self) -> int:
        # A device that cannot conclude does not guess. discontiguous reports a
        # fault and no count, which the schema records as 0 alongside the status.
        return 0 if self.fault == "discontiguous" else min(self.stack, LEVELS)

    def wire_state(self) -> tuple:
        """Exactly the fields differs() compares -- so a simulated `change`
        event means the same thing a real one does."""
        return (
            self.reported_count(),
            self.stack_status(),
            tuple(self.levels()),
            tuple(self.sensors_ok),
            self.band,
            self.charging,
        )

    def common(self) -> dict:
        return {
            "stack_count": self.reported_count(),
            "stack_status": self.stack_status(),
            "levels": self.levels(),
            "sensors_ok": list(self.sensors_ok),
            "sensors_online": sum(self.sensors_ok),
            "battery_level": self.band,
            "charging": self.charging,
            "firmware": FIRMWARE,
        }

    def event(self, reason: str, age_ms: int) -> dict:
        self.seq += 1
        return {
            "device_id": self.device_id,
            "boot_id": self.boot_id,
            "seq": self.seq,
            "age_ms": max(0, min(int(age_ms), AGE_MAX_MS)),
            "reason": reason,
            **self.common(),
        }

    def status(self) -> dict:
        return {
            "boot_id": self.boot_id,
            "uptime_s": self.uptime_s,
            "battery_mv": min(self.cell_mv, BATTERY_PUBLISH_MAX_MV),
            "mac": self.mac,
            **self.common(),
        }


# ---------------------------------------------------------------------------
# Transport -- the same two calls the firmware makes, and nothing else.
# ---------------------------------------------------------------------------
class Uplink:
    def __init__(self, url: str, key: str, dry_run: bool = False):
        self.base = url
        self.dry_run = dry_run
        self.session = requests.Session()
        self.session.headers.update({
            "apikey": key,
            "Authorization": f"Bearer {key}",
            "Content-Type": "application/json",
        })
        self.events_sent = 0
        self.patches_sent = 0
        self.errors: list[str] = []

    def post_events(self, batch: list[dict]) -> bool:
        if not batch:
            return True
        if self.dry_run:
            print(json.dumps(batch[:2], indent=2))
            print(f"  ... {len(batch)} events (dry run)")
            self.events_sent += len(batch)
            return True

        r = self.session.post(
            f"{self.base}/rest/v1/status_events",
            headers={"Prefer": "return=minimal"},
            data=json.dumps(batch),
            timeout=30,
        )
        if 200 <= r.status_code < 300:
            self.events_sent += len(batch)
            return True

        # 23505 means these rows are already stored, which is success for an
        # idempotent retry rather than a failure.
        if "23505" in r.text:
            return True
        self._record("POST status_events", r)
        return False

    def patch_status(self, device_id: str, body: dict) -> bool:
        if self.dry_run:
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
        # count there is no way to tell "reported" from "wrote nothing, forever"
        # -- which is exactly how an unregistered device looks.
        rng = r.headers.get("Content-Range", "")
        if rng.endswith("/0"):
            self._record(
                f"PATCH device_status {device_id}: matched 0 rows -- run "
                f"supabase/register_devices.sql",
                r,
            )
            return False
        self.patches_sent += 1
        return True

    def _record(self, what: str, r: requests.Response) -> None:
        msg = f"{what} -> {r.status_code} {r.text[:200]}"
        if msg not in self.errors:
            self.errors.append(msg)
        print(f"  ERROR {msg}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Modes
# ---------------------------------------------------------------------------
def make_fleet(seed: int) -> list[SimDevice]:
    # Seeded so successive runs are reproducible: a front-end developer
    # comparing two screenshots should not be fighting fresh randomness.
    return [
        SimDevice(f"BWL-{i:03d}", random.Random(seed + i))
        for i in range(1, DEVICE_COUNT + 1)
    ]


def run_once(fleet: list[SimDevice], up: Uplink) -> None:
    batch = []
    for d in fleet:
        d.serve_tick(1)
        batch.append(d.event("periodic", 0))
    up.post_events(batch)
    for d in fleet:
        up.patch_status(d.device_id, d.status())


def run_backfill(fleet: list[SimDevice], up: Uplink, days: int) -> None:
    """Replays `days` of meal service into status_events.

    Everything is positioned by AGE, because that is the only time reference the
    write path has -- the device has no clock and the server refuses to take a
    timestamp from it. Oldest first, so the sequence a dashboard reads back is
    the sequence in which things happened.
    """
    max_days = AGE_MAX_MS // 86_400_000
    if days > max_days:
        print(f"  age_ms is clamped at {max_days} days server-side; using {max_days}")
        days = max_days

    now = datetime.now(timezone.utc)
    total = 0

    for day_offset in range(days, 0, -1):
        day = now - timedelta(days=day_offset)
        for label, start_min, end_min in SERVICE_WINDOWS:
            batch = []
            for d in fleet:
                # A device is powered on for each service, so each service is a
                # new boot -- which is what makes seq restart from zero.
                d.reboot()
                svc_start = day.replace(
                    hour=start_min // 60, minute=start_min % 60,
                    second=0, microsecond=0,
                )
                age_at = lambda t: (now - t).total_seconds() * 1000.0

                batch.append(d.event("boot", age_at(svc_start)))

                minutes = end_min - start_min
                step = 5
                for m in range(step, minutes, step):
                    at = svc_start + timedelta(minutes=m)
                    if age_at(at) < 0:
                        break  # not yet happened
                    if d.serve_tick(step):
                        batch.append(d.event("change", age_at(at)))
                d.charge_overnight()

            if up.post_events(batch):
                total += len(batch)
            print(f"  {day.date()} {label:9s} {len(batch):5d} events")

    # Leave every row's current state consistent with the end of the replay.
    for d in fleet:
        up.patch_status(d.device_id, d.status())
    print(f"  backfilled {total} events across {days} days")


def in_service_window(at: datetime) -> str | None:
    mins = at.hour * 60 + at.minute
    for label, start, end in SERVICE_WINDOWS:
        if start <= mins < end:
            return label
    return None


def run_live(fleet: list[SimDevice], up: Uplink, interval: int, respect_window: bool) -> None:
    print(f"  live, {interval}s per round, Ctrl-C to stop")
    round_no = 0
    try:
        while True:
            round_no += 1
            window = in_service_window(datetime.now())
            if respect_window and window is None:
                print(f"  round {round_no}: outside service hours, idle "
                      f"(--always to override)")
                time.sleep(interval)
                continue

            batch = []
            for d in fleet:
                if d.serve_tick(max(1, interval // 60)):
                    batch.append(d.event("change", 0))
            up.post_events(batch)
            for d in fleet:
                up.patch_status(d.device_id, d.status())
            print(f"  round {round_no} ({window or 'off-hours'}): "
                  f"{len(batch)} changes, {len(fleet)} status rows")
            time.sleep(interval)
    except KeyboardInterrupt:
        print("\n  stopped")


# ---------------------------------------------------------------------------
def main() -> int:
    p = argparse.ArgumentParser(
        description="Simulate the Bowlstack fleet against Supabase.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument("--once", action="store_true", help="one round for all devices")
    mode.add_argument("--backfill", type=int, metavar="DAYS",
                      help="replay DAYS of meal service into status_events (max 7)")
    mode.add_argument("--live", action="store_true", help="run continuously")

    p.add_argument("--interval", type=int, default=60,
                   help="seconds per round in --live (default 60)")
    p.add_argument("--always", action="store_true",
                   help="in --live, report outside service hours too")
    p.add_argument("--devices", type=int, default=DEVICE_COUNT,
                   help=f"how many devices to simulate (default {DEVICE_COUNT})")
    p.add_argument("--seed", type=int, default=20260726,
                   help="RNG seed, for reproducible runs")
    p.add_argument("--dry-run", action="store_true",
                   help="print payloads, send nothing")
    args = p.parse_args()

    url, key = load_credentials()
    print(f"bowlstack fleet sim -> {url}")
    if args.dry_run:
        print("  DRY RUN -- nothing will be sent")

    fleet = make_fleet(args.seed)[: args.devices]
    print(f"  {len(fleet)} devices: {fleet[0].device_id} .. {fleet[-1].device_id}")

    up = Uplink(url, key, dry_run=args.dry_run)
    started = time.time()

    if args.once:
        run_once(fleet, up)
    elif args.backfill is not None:
        run_backfill(fleet, up, args.backfill)
    else:
        run_live(fleet, up, args.interval, respect_window=not args.always)

    print(f"\n  {up.events_sent} events, {up.patches_sent} status rows, "
          f"{len(up.errors)} distinct errors, {time.time() - started:.1f}s")

    if up.errors:
        print("\n  FAILURES:")
        for e in up.errors:
            print(f"    {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
