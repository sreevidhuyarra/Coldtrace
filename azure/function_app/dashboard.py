"""Dashboard summary: turn table rows into what the page shows. No Azure imports.

summarize() answers, for one device:
  - what is the temperature now, against which limit, and is an alert open?
  - when was it last seen, is it online?
  - did every reading of the current boot arrive? (received vs expected, gaps)
  - how many replayed copies did de-duplication absorb?
  - how fast do readings reach storage (cloud clocks only)?
"""
from __future__ import annotations

import math
from datetime import datetime
from typing import Any, Iterable, Optional

ONLINE_WITHIN_S = 120     # seen this recently -> online (default report interval is 30 s)
SERIES_LEN = 120          # readings drawn on the chart
GAPS_SHOWN = 20


def _v(x: Any) -> Any:
    """Table Storage returns Int64 columns wrapped (EntityProperty); unwrap."""
    return getattr(x, "value", x)


def _iso(t: Optional[datetime]) -> Optional[str]:
    return t.isoformat().replace("+00:00", "Z") if t else None


def percentile(sorted_vals: list[float], p: float) -> Optional[float]:
    """Nearest-rank percentile of an already sorted list."""
    if not sorted_vals:
        return None
    k = max(0, min(len(sorted_vals) - 1, math.ceil(p / 100 * len(sorted_vals)) - 1))
    return sorted_vals[k]


def gaps(seqs: Iterable[int]) -> tuple[int, list[int], int]:
    """(expected count from seq 1 to the highest seen, missing seqs, received count)."""
    got = sorted(set(seqs))
    if not got:
        return 0, [], 0
    missing = sorted(set(range(1, got[-1] + 1)) - set(got))
    return got[-1], missing, len(got)


def summarize(device: str, state: Optional[dict], readings: list[dict], alerts: list[dict],
              now: datetime) -> dict:
    """state: the devices-table row (or None); readings: rows of the current boot;
    alerts: alert rows for the device. Returns the JSON the page renders."""
    if not state:
        return {"device": device, "known": False}

    boot = state.get("boot", "")
    last_seen: Optional[datetime] = state.get("last_seen")
    since = (now - last_seen).total_seconds() if last_seen else None
    rows = sorted(readings, key=lambda r: _v(r["seq"]))
    expected, missing, received = gaps(_v(r["seq"]) for r in rows)
    live = sorted(float(r["ingest_ms"]) for r in rows if float(r.get("ingest_ms", 1e12)) < 60_000)
    last = rows[-1] if rows else None
    open_alert = state.get("open_alert") or None

    recent_alerts = sorted(alerts, key=lambda a: a["raised_at"], reverse=True)[:10]
    return {
        "device": device,
        "known": True,
        "now": _iso(now),
        "boot": boot,
        "last_seen": _iso(last_seen),
        "seconds_since_seen": round(since, 1) if since is not None else None,
        "online": since is not None and since <= ONLINE_WITHIN_S,
        "last_temp": state.get("last_temp"),
        "limit_c": float(last["limit_c"]) if last is not None and "limit_c" in last else None,
        "status": "ALERT" if open_alert else "OK",
        "open_alert": open_alert,
        "integrity": {
            "expected": expected,
            "received": received,
            "missing_count": len(missing),
            "missing": missing[:GAPS_SHOWN],
            "duplicates_absorbed": sum(int(r.get("duplicates", 0)) for r in rows),
        },
        "ingest_ms": {
            "n": len(live),
            "p50": percentile(live, 50),
            "p95": percentile(live, 95),
            "note": "IoT Hub enqueue -> stored, cloud clocks; backlog older than 60 s excluded",
        },
        "series": [
            {"seq": _v(r["seq"]), "temp": float(r["temp"]), "at": _iso(r.get("enqueued_at")),
             "limit_c": float(r["limit_c"]) if "limit_c" in r else None}
            for r in rows[-SERIES_LEN:]
        ],
        "alerts": [
            {"id": a["RowKey"], "status": a.get("status"), "raised_at": _iso(a.get("raised_at")),
             "cleared_at": _iso(a.get("cleared_at")), "temp": a.get("temp"), "limit_c": a.get("limit_c"),
             "window_s": a.get("window_s"), "raised_seq": _v(a.get("raised_seq")),
             "cleared_seq": _v(a.get("cleared_seq"))}
            for a in recent_alerts
        ],
    }
