"""ColdTrace cloud pipeline: the logic, with no Azure SDK imports.

function_app.py feeds IoT Hub events in and supplies a Store backed by Azure
Table Storage; the unit tests supply an in-memory Store. Everything that can be
decided without the cloud is decided here:

- parse and validate a telemetry message
- de-duplicate on (device, boot, seq): the first copy is stored, later copies
  only bump a `duplicates` counter on the stored row
- track when each device was last seen
- excursion alerts: a reading above the limit starts a timer; if readings stay
  above it for the alert window, exactly one alert is raised; the first reading
  back at or below the limit clears it

The excursion timer uses the device's own sample times (t_ms) within one boot,
not hub arrival times: after an outage the backlog arrives all at once, and
arrival times would make a five-minute excursion look instantaneous.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from datetime import datetime
from typing import Iterable, Optional, Protocol

DEFAULT_LIMIT_C = 8.0
DEFAULT_WINDOW_S = 300


@dataclass(frozen=True)
class Reading:
    device: str            # from IoT Hub's authenticated connection, not the payload
    boot: str              # 8 hex digits, random per device boot
    seq: int               # 1, 2, 3 ... per boot
    t_ms: int              # device uptime when sampled
    temp: float            # degrees C
    enqueued_at: datetime  # IoT Hub's receive time (UTC)
    limit_c: float = DEFAULT_LIMIT_C
    window_s: int = DEFAULT_WINDOW_S

    @property
    def row_key(self) -> str:
        """Sorts by boot, then seq: zero-padded so string order is numeric order."""
        return f"{self.boot}-{self.seq:010d}"


@dataclass
class DeviceState:
    """Per-device state the pipeline keeps between batches."""
    boot: str = ""
    last_seq: int = 0
    above_since_ms: Optional[int] = None   # t_ms of the first reading above the limit
    open_alert: Optional[str] = None       # id of the raised, not yet cleared, alert
    last_seen: Optional[datetime] = None   # IoT Hub enqueue time of the newest reading applied
    last_temp: Optional[float] = None
    etag: Optional[str] = None             # version of the stored row this was read from


@dataclass
class Outcome:
    stored: int = 0
    duplicates: int = 0
    rejected: int = 0
    alerts_raised: list[str] = field(default_factory=list)
    alerts_cleared: list[str] = field(default_factory=list)


class Store(Protocol):
    def insert_reading(self, r: Reading, stored_at: datetime) -> bool:
        """Store r; True if new, False if (device, boot, seq) was already stored
        (then the stored row's duplicate counter goes up instead)."""

    def get_state(self, device: str) -> DeviceState:
        """Current state with its etag (etag None: no row yet)."""

    def put_state(self, device: str, state: DeviceState) -> None:
        """Write state only if the stored row is still the version state.etag
        names (or still absent); otherwise raise StateConflict."""

    def raise_alert(self, alert_id: str, r: Reading, above_since_ms: int, raised_at: datetime) -> None: ...

    def clear_alert(self, device: str, alert_id: str, r: Reading, cleared_at: datetime) -> None: ...


class BadMessage(ValueError):
    pass


class StateConflict(Exception):
    """Another instance updated the device's state since we read it."""


def parse(body: bytes | str, device: str, enqueued_at: datetime) -> Reading:
    """One telemetry message -> Reading. Raises BadMessage if it is not one."""
    try:
        d = json.loads(body)
    except (ValueError, UnicodeDecodeError) as e:
        raise BadMessage(f"not JSON: {e}") from None
    if not isinstance(d, dict):
        raise BadMessage("not a JSON object")
    if d.get("dev") not in (None, device):
        raise BadMessage(f"payload dev {d.get('dev')!r} != connection device {device!r}")
    try:
        boot = str(d["boot"])
        seq = int(d["seq"])
        t_ms = int(d["t_ms"])
        temp = float(d["temp"])
    except (KeyError, TypeError, ValueError) as e:
        raise BadMessage(f"missing or bad field: {e}") from None
    if len(boot) != 8 or any(c not in "0123456789abcdef" for c in boot):
        raise BadMessage(f"bad boot id {boot!r}")
    if seq < 1 or t_ms < 0:
        raise BadMessage(f"bad seq/t_ms {seq}/{t_ms}")
    limit = float(d.get("limit", DEFAULT_LIMIT_C))
    window = int(d.get("window_s", DEFAULT_WINDOW_S))
    return Reading(device, boot, seq, t_ms, temp, enqueued_at, limit, window)


def _is_newer(state: DeviceState, r: Reading) -> bool:
    """Does r move this device's history forward? Replays (Event Hubs
    redelivery, device resends) and late arrivals do not. Within a boot seq
    decides; across boots (random ids, not ordered) the hub's receive time does."""
    if not state.boot:
        return True
    if r.boot == state.boot:
        return r.seq > state.last_seq
    return state.last_seen is None or r.enqueued_at > state.last_seen


def _excursion(state: DeviceState, r: Reading, actions: list) -> None:
    """Advance the alert state machine by one reading. Alert writes are only
    collected here; process() performs them after the state commit succeeds."""
    if r.boot != state.boot:
        state.above_since_ms = None        # t_ms from another boot can't be compared
    if r.temp > r.limit_c:
        if state.above_since_ms is None:
            state.above_since_ms = r.t_ms
        held_ms = r.t_ms - state.above_since_ms
        if state.open_alert is None and held_ms >= r.window_s * 1000:
            # deterministic: two instances judging the same excursion name it alike
            state.open_alert = f"{r.device}-{r.boot}-{state.above_since_ms:010d}"
            actions.append(("raise", state.open_alert, r, state.above_since_ms))
    else:
        state.above_since_ms = None
        if state.open_alert is not None:
            actions.append(("clear", state.open_alert, r, None))
            state.open_alert = None


def process(readings: Iterable[Reading], store: Store, now: datetime, attempts: int = 5) -> Outcome:
    """Store a batch, then advance each device's alert state.

    Event Hubs delivers at least once, and two Function instances can process
    the same partition while ownership moves (bug 006). So:
      1. every reading is inserted once (de-duplicated on device, boot, seq);
      2. per device, the state is read with its etag, advanced by the readings
         that are newer than it, and written back only if nobody else wrote it
         meanwhile; on a conflict the step repeats on fresh state;
      3. alert rows are written only after the state commit succeeded, so a
         losing attempt leaves nothing behind.
    """
    out = Outcome()
    batch = sorted(readings, key=lambda r: (r.device, r.enqueued_at, r.boot, r.seq))
    by_device: dict[str, list[Reading]] = {}
    for r in batch:
        if store.insert_reading(r, now):
            out.stored += 1
        else:
            out.duplicates += 1
        by_device.setdefault(r.device, []).append(r)

    for device, rows in by_device.items():
        for _ in range(attempts):
            state = store.get_state(device)
            actions: list = []
            for r in rows:
                if not _is_newer(state, r):
                    continue                   # replay or late arrival: stored, history unchanged
                _excursion(state, r, actions)
                state.boot, state.last_seq = r.boot, r.seq
                state.last_seen, state.last_temp = r.enqueued_at, r.temp
            try:
                store.put_state(device, state)
            except StateConflict:
                continue
            for kind, alert_id, r, since in actions:
                if kind == "raise":
                    store.raise_alert(alert_id, r, since, now)
                    out.alerts_raised.append(alert_id)
                else:
                    store.clear_alert(device, alert_id, r, now)
                    out.alerts_cleared.append(alert_id)
            break
        else:
            raise StateConflict(f"{device}: state still changing after {attempts} attempts")
    return out
