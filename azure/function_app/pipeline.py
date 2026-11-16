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
    last_seen: Optional[datetime] = None
    last_temp: Optional[float] = None


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

    def get_state(self, device: str) -> DeviceState: ...

    def put_state(self, device: str, state: DeviceState) -> None: ...

    def raise_alert(self, alert_id: str, r: Reading, above_since_ms: int, raised_at: datetime) -> None: ...

    def clear_alert(self, device: str, alert_id: str, r: Reading, cleared_at: datetime) -> None: ...


class BadMessage(ValueError):
    pass


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


def _excursion(state: DeviceState, r: Reading, store: Store, now: datetime, out: Outcome) -> None:
    if r.boot != state.boot:
        state.above_since_ms = None        # t_ms from another boot can't be compared
    if r.temp > r.limit_c:
        if state.above_since_ms is None:
            state.above_since_ms = r.t_ms
        held_ms = r.t_ms - state.above_since_ms
        if state.open_alert is None and held_ms >= r.window_s * 1000:
            state.open_alert = f"{r.device}-{r.row_key}"
            store.raise_alert(state.open_alert, r, state.above_since_ms, now)
            out.alerts_raised.append(state.open_alert)
    else:
        state.above_since_ms = None
        if state.open_alert is not None:
            store.clear_alert(r.device, state.open_alert, r, now)
            out.alerts_cleared.append(state.open_alert)
            state.open_alert = None


def process(readings: Iterable[Reading], store: Store, now: datetime) -> Outcome:
    """Store a batch and update alerts. Readings are handled per device in
    (boot, seq) order; a reading older than one already seen in the same boot
    is stored but does not move the alert state backwards."""
    out = Outcome()
    batch = sorted(readings, key=lambda r: (r.device, r.enqueued_at, r.boot, r.seq))
    states: dict[str, DeviceState] = {}
    for r in batch:
        state = states.get(r.device) or store.get_state(r.device)
        states[r.device] = state
        if not store.insert_reading(r, now):
            out.duplicates += 1
            continue
        out.stored += 1
        if r.boot == state.boot and r.seq <= state.last_seq:
            continue                       # late arrival: stored, history unchanged
        _excursion(state, r, store, now, out)
        state.boot, state.last_seq = r.boot, r.seq
        state.last_seen, state.last_temp = r.enqueued_at, r.temp
    for device, state in states.items():
        store.put_state(device, state)
    return out
