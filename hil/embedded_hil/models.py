"""Parsed forms of the lines the firmware sends (see docs/protocol.md)."""
from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(frozen=True)
class Response:
    """The one OK/ERR line a command gets back.

    `OK RATE 500`  -> ok=True,  keyword="RATE",    args=["500"]
    `ERR RANGE`    -> ok=False, keyword="RANGE",   args=[]
    """

    ok: bool
    keyword: str
    args: list[str]
    raw: str
    latency_ms: float

    @classmethod
    def parse(cls, line: str, latency_ms: float) -> "Response":
        parts = line.split()
        return cls(
            ok=parts[0] == "OK",
            keyword=parts[1] if len(parts) > 1 else "",
            args=parts[2:],
            raw=line,
            latency_ms=latency_ms,
        )


@dataclass(frozen=True)
class Event:
    """An unsolicited line: `BOOT ...` or `EVT ...`.

    `EVT SENSOR_LOST`      -> name="SENSOR_LOST", args=[]
    `EVT S 2000 24.01`     -> name="S",           args=["2000", "24.01"]
    `BOOT fw=1.0.0 reset=` -> name="BOOT",        args=["fw=1.0.0", "reset=..."]
    """

    name: str
    args: list[str]
    raw: str
    host_time: float  # time.perf_counter() when the line arrived

    @classmethod
    def parse(cls, line: str, host_time: float) -> "Event":
        parts = line.split()
        if parts[0] == "BOOT":
            return cls("BOOT", parts[1:], line, host_time)
        return cls(parts[1] if len(parts) > 1 else "", parts[2:], line, host_time)


@dataclass(frozen=True)
class Sample:
    """One temperature sample: from a DUMP `S` line or an `EVT S` event."""

    t_ms: int
    temp_c: float

    @classmethod
    def from_fields(cls, t_ms: str, temp: str) -> "Sample":
        return cls(int(t_ms), float(temp))


@dataclass(frozen=True)
class BootInfo:
    """Parsed `BOOT fw=1.0.0 reset=POWERON`."""

    fw: str
    reset: str
    fields: dict[str, str] = field(default_factory=dict)

    @classmethod
    def from_event(cls, event: Event) -> "BootInfo":
        fields = dict(arg.split("=", 1) for arg in event.args if "=" in arg)
        return cls(fw=fields.get("fw", ""), reset=fields.get("reset", ""), fields=fields)
