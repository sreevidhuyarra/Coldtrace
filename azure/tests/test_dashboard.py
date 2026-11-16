"""Unit tests for the dashboard summary."""
from datetime import datetime, timedelta, timezone

from dashboard import gaps, percentile, summarize

NOW = datetime(2026, 11, 17, 12, 0, tzinfo=timezone.utc)


class I64:
    """Stands in for azure.data.tables.EntityProperty (Int64 columns)."""
    def __init__(self, v):
        self.value = v


def row(seq, temp=4.0, ingest=40.0, dups=0, limit=8.0):
    return {"seq": I64(seq), "temp": temp, "ingest_ms": ingest, "duplicates": dups, "limit_c": limit,
            "enqueued_at": NOW - timedelta(seconds=100 - seq)}


STATE = {"boot": "1a2b3c4d", "last_seen": NOW - timedelta(seconds=20), "last_temp": 4.2, "open_alert": ""}


def test_gaps_counts_from_seq_1():
    assert gaps([1, 2, 3]) == (3, [], 3)
    assert gaps([2, 3, 5]) == (5, [1, 4], 3)       # seq 1 lost too
    assert gaps([]) == (0, [], 0)
    assert gaps([3, 3, 1, 2]) == (3, [], 3)


def test_percentile_nearest_rank():
    vals = sorted(float(v) for v in range(1, 101))
    assert percentile(vals, 50) == 50.0
    assert percentile(vals, 95) == 95.0
    assert percentile([7.0], 95) == 7.0
    assert percentile([], 50) is None


def test_unknown_device():
    assert summarize("x", None, [], [], NOW) == {"device": "x", "known": False}


def test_healthy_device_summary():
    s = summarize("coldtrace-01", STATE, [row(n) for n in range(1, 11)], [], NOW)
    assert s["online"] is True and s["status"] == "OK"
    assert s["last_temp"] == 4.2 and s["limit_c"] == 8.0
    assert s["integrity"] == {"expected": 10, "received": 10, "missing_count": 0, "missing": [],
                              "duplicates_absorbed": 0}
    assert s["ingest_ms"]["p50"] == 40.0
    assert [p["seq"] for p in s["series"]] == list(range(1, 11))


def test_gaps_and_duplicates_are_reported():
    rows = [row(n, dups=1 if n in (3, 4) else 0) for n in range(1, 11) if n not in (5, 6)]
    s = summarize("coldtrace-01", STATE, rows, [], NOW)
    assert s["integrity"]["missing"] == [5, 6]
    assert s["integrity"]["received"] == 8 and s["integrity"]["expected"] == 10
    assert s["integrity"]["duplicates_absorbed"] == 2


def test_offline_after_two_minutes_silence():
    state = dict(STATE, last_seen=NOW - timedelta(seconds=121))
    assert summarize("coldtrace-01", state, [row(1)], [], NOW)["online"] is False


def test_backlog_latency_is_excluded_from_percentiles():
    rows = [row(1, ingest=40.0), row(2, ingest=50.0), row(3, ingest=900_000.0)]
    s = summarize("coldtrace-01", STATE, rows, [], NOW)
    assert s["ingest_ms"]["n"] == 2 and s["ingest_ms"]["p95"] == 50.0


def test_open_alert_and_alert_list_newest_first():
    state = dict(STATE, open_alert="coldtrace-01-1a2b3c4d-0000000012")
    alerts = [
        {"RowKey": "a1", "status": "CLEARED", "raised_at": NOW - timedelta(hours=2), "raised_seq": I64(5)},
        {"RowKey": "a2", "status": "OPEN", "raised_at": NOW - timedelta(minutes=5), "raised_seq": I64(12)},
    ]
    s = summarize("coldtrace-01", state, [row(12, temp=9.1)], alerts, NOW)
    assert s["status"] == "ALERT"
    assert [a["id"] for a in s["alerts"]] == ["a2", "a1"]
    assert s["alerts"][0]["raised_seq"] == 12


def test_series_is_capped_to_the_newest_readings():
    s = summarize("coldtrace-01", STATE, [row(n) for n in range(1, 301)], [], NOW)
    assert len(s["series"]) == 120 and s["series"][-1]["seq"] == 300
