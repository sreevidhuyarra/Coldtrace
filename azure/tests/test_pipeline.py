"""Unit tests for the cloud pipeline logic (no Azure needed)."""
from __future__ import annotations

import copy
import json
from datetime import datetime, timedelta, timezone

import pytest

from pipeline import BadMessage, DeviceState, Reading, StateConflict, parse, process

T0 = datetime(2026, 11, 17, 12, 0, tzinfo=timezone.utc)
DEV = "coldtrace-01"


class MemoryStore:
    """In-memory Store with the same optimistic-concurrency rule as Table
    Storage: put_state succeeds only if the row is unchanged since get_state."""

    def __init__(self):
        self.rows: dict[tuple[str, str], dict] = {}
        self._states: dict[str, tuple[DeviceState, int]] = {}
        self.alerts: dict[str, dict] = {}
        self.conflicts = 0

    @property
    def states(self) -> dict[str, DeviceState]:
        return {d: s for d, (s, _) in self._states.items()}

    def insert_reading(self, r, stored_at):
        key = (r.device, r.row_key)
        if key in self.rows:
            self.rows[key]["duplicates"] += 1
            return False
        self.rows[key] = {"temp": r.temp, "stored_at": stored_at, "duplicates": 0}
        return True

    def get_state(self, device):
        if device not in self._states:
            return DeviceState()
        state, version = self._states[device]
        fresh = copy.deepcopy(state)
        fresh.etag = str(version)
        return fresh

    def put_state(self, device, state):
        current = self._states.get(device)
        if (current is None) != (state.etag is None) or (current and str(current[1]) != state.etag):
            self.conflicts += 1
            raise StateConflict(device)
        version = current[1] + 1 if current else 1
        self._states[device] = (copy.deepcopy(state), version)

    def raise_alert(self, alert_id, r, above_since_ms, raised_at):
        self.alerts[alert_id] = {"raised_seq": r.seq, "above_since_ms": above_since_ms, "cleared_seq": None}

    def clear_alert(self, device, alert_id, r, cleared_at):
        self.alerts[alert_id]["cleared_seq"] = r.seq


def rd(seq, temp=4.0, t_ms=None, boot="1a2b3c4d", at=None, **kw):
    t_ms = seq * 30_000 if t_ms is None else t_ms
    return Reading(DEV, boot, seq, t_ms, temp, at or T0 + timedelta(seconds=seq), **kw)


# ---- parse ---------------------------------------------------------------------------

def test_parse_reads_the_device_payload():
    body = json.dumps({"dev": DEV, "boot": "1a2b3c4d", "seq": 12, "t_ms": 123456, "temp": 24.01})
    r = parse(body.encode(), DEV, T0)
    assert (r.boot, r.seq, r.t_ms, r.temp) == ("1a2b3c4d", 12, 123456, 24.01)
    assert (r.limit_c, r.window_s) == (8.0, 300)
    assert r.row_key == "1a2b3c4d-0000000012"


def test_parse_takes_limit_and_window_from_the_payload():
    body = json.dumps({"boot": "1a2b3c4d", "seq": 1, "t_ms": 0, "temp": 5, "limit": 6.5, "window_s": 60})
    r = parse(body, DEV, T0)
    assert (r.limit_c, r.window_s) == (6.5, 60)


@pytest.mark.parametrize("body", [
    "not json",
    "[1, 2]",
    '{"boot": "1a2b3c4d", "seq": 1, "t_ms": 0}',                                      # no temp
    '{"boot": "XYZ", "seq": 1, "t_ms": 0, "temp": 1}',                                # bad boot
    '{"boot": "1a2b3c4d", "seq": 0, "t_ms": 0, "temp": 1}',                           # seq starts at 1
    '{"boot": "1a2b3c4d", "seq": "x", "t_ms": 0, "temp": 1}',
])
def test_parse_rejects_malformed_messages(body):
    with pytest.raises(BadMessage):
        parse(body, DEV, T0)


def test_parse_rejects_a_device_claiming_to_be_another():
    body = json.dumps({"dev": "coldtrace-99", "boot": "1a2b3c4d", "seq": 1, "t_ms": 0, "temp": 1})
    with pytest.raises(BadMessage, match="connection device"):
        parse(body, DEV, T0)


# ---- storage and de-duplication ----------------------------------------------------------

def test_each_reading_is_stored_once_and_replays_are_counted():
    s = MemoryStore()
    out = process([rd(1), rd(2), rd(3)], s, T0)
    assert (out.stored, out.duplicates) == (3, 0)

    out = process([rd(2), rd(3), rd(4)], s, T0)          # reconnect replays 2 and 3
    assert (out.stored, out.duplicates) == (1, 2)
    assert len(s.rows) == 4
    assert s.rows[(DEV, rd(2).row_key)]["duplicates"] == 1


def test_duplicate_keeps_the_first_stored_time_for_latency():
    s = MemoryStore()
    process([rd(1)], s, T0)
    process([rd(1)], s, T0 + timedelta(minutes=5))
    assert s.rows[(DEV, rd(1).row_key)]["stored_at"] == T0


def test_same_seq_in_a_new_boot_is_a_new_reading():
    s = MemoryStore()
    out = process([rd(1, boot="aaaaaaaa"), rd(1, boot="bbbbbbbb", at=T0 + timedelta(hours=1))], s, T0)
    assert (out.stored, out.duplicates) == (2, 0)


def test_last_seen_tracks_the_newest_reading():
    s = MemoryStore()
    process([rd(1, temp=4.1), rd(2, temp=4.2)], s, T0)
    st = s.states[DEV]
    assert (st.last_seq, st.last_temp, st.last_seen) == (2, 4.2, rd(2).enqueued_at)


def test_batch_order_does_not_matter():
    s = MemoryStore()
    process([rd(3), rd(1), rd(2)], s, T0)
    assert s.states[DEV].last_seq == 3


# ---- excursion alerts ------------------------------------------------------------------------

def above(seq, temp=9.0, **kw):
    return rd(seq, temp=temp, **kw)


def test_short_excursion_raises_nothing():
    s = MemoryStore()
    out = process([rd(1), above(2), above(3), rd(4)], s, T0)   # above for 30 s < 300 s
    assert out.alerts_raised == [] and s.alerts == {}


def test_excursion_held_for_the_window_raises_exactly_one_alert_then_clears():
    s = MemoryStore()
    readings = [rd(1)] + [above(n) for n in range(2, 14)] + [rd(14)]   # above 2..13 = 330 s
    out = process(readings, s, T0)
    assert len(out.alerts_raised) == 1
    alert = s.alerts[out.alerts_raised[0]]
    assert alert["raised_seq"] == 12          # t=360 s, 300 s after first above at t=60 s
    assert alert["above_since_ms"] == 60_000
    assert alert["cleared_seq"] == 14
    assert out.alerts_cleared == out.alerts_raised


def test_exactly_the_limit_is_not_an_excursion():
    s = MemoryStore()
    out = process([rd(n, temp=8.0) for n in range(1, 30)], s, T0)
    assert out.alerts_raised == []


def test_alert_state_carries_across_batches():
    s = MemoryStore()
    process([above(n) for n in range(1, 8)], s, T0)          # 180 s so far
    out = process([above(n) for n in range(8, 13)], s, T0)    # reaches 330 s
    assert len(out.alerts_raised) == 1
    out = process([rd(13)], s, T0)
    assert len(out.alerts_cleared) == 1


def test_backlog_after_outage_is_timed_by_the_device_clock():
    """Ten readings above the limit, 30 s apart on the device, all arriving
    within one second after an outage: the excursion lasted 270 s, not 1 s."""
    s = MemoryStore()
    burst = [above(n, at=T0 + timedelta(milliseconds=n)) for n in range(1, 11)]
    assert process(burst, s, T0).alerts_raised == []          # 270 s < 300 s
    assert len(process([above(11)], s, T0).alerts_raised) == 1  # 300 s


def test_replayed_readings_do_not_raise_a_second_alert():
    s = MemoryStore()
    readings = [above(n) for n in range(1, 13)]
    first = process(readings, s, T0)
    again = process(readings, s, T0)
    assert len(first.alerts_raised) == 1 and again.alerts_raised == []


def test_late_older_reading_does_not_clear_an_open_alert():
    s = MemoryStore()
    readings = [above(n) for n in range(1, 13) if n != 5]
    process(readings, s, T0)
    out = process([rd(5, temp=4.0)], s, T0)                   # seq 5 arrives late, below limit
    assert out.stored == 1 and out.alerts_cleared == []


def test_reboot_restarts_the_excursion_timer():
    s = MemoryStore()
    process([above(n, boot="aaaaaaaa") for n in range(1, 10)], s, T0)        # 240 s in boot a
    later = T0 + timedelta(hours=1)
    out = process([above(n, boot="bbbbbbbb", at=later + timedelta(seconds=n)) for n in range(1, 5)], s, T0)
    assert out.alerts_raised == []                                           # only 90 s in boot b


def test_replay_does_not_count_as_new_in_alert_state_even_if_reinserted():
    """A batch delivered twice (Event Hubs at-least-once) changes nothing the second time."""
    s = MemoryStore()
    batch = [rd(1), above(2), above(3)]
    process(batch, s, T0)
    before = copy.deepcopy(s.states[DEV])
    out = process(batch, s, T0)
    assert out.duplicates == 3 and s.states[DEV].above_since_ms == before.above_since_ms


# ---- concurrency (bug 006) -------------------------------------------------------------------

class RacingStore(MemoryStore):
    """Lets a second "instance" run a whole batch between the first instance's
    get_state and put_state: the interleaving that orphaned an alert."""

    def __init__(self, other_batch):
        super().__init__()
        self.other_batch = other_batch
        self.raced = False

    def put_state(self, device, state):
        if not self.raced:
            self.raced = True
            process(self.other_batch, self, T0)     # instance B commits first
        super().put_state(device, state)


def test_two_instances_racing_raise_exactly_one_alert():
    # instance A: boot a, above the limit long enough to alert
    a = [above(n, boot="aaaaaaaa") for n in range(1, 13)]
    # instance B: a later boot, also above the limit long enough
    later = T0 + timedelta(hours=1)
    b = [above(n, boot="bbbbbbbb", at=later + timedelta(seconds=n)) for n in range(1, 13)]
    s = RacingStore(other_batch=b)
    out_a = process(a, s, T0)

    assert s.conflicts == 1                          # A's first commit was refused
    assert len(s.alerts) == 1                        # not one per instance
    only = next(iter(s.alerts))
    assert s.states[DEV].open_alert == only          # and the state points at it
    assert out_a.alerts_raised == []                 # A, retrying on B's state, saw it open


def test_conflict_retry_gives_up_after_the_attempt_limit():
    class AlwaysConflict(MemoryStore):
        def put_state(self, device, state):
            raise StateConflict(device)
    with pytest.raises(StateConflict, match="attempts"):
        process([rd(1)], AlwaysConflict(), T0, attempts=3)


def test_payload_limit_and_window_are_used():
    s = MemoryStore()
    readings = [rd(n, temp=7.0, limit_c=6.5, window_s=60) for n in range(1, 4)]   # 60 s above 6.5
    assert len(process(readings, s, T0).alerts_raised) == 1
