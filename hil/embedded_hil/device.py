"""DeviceClient: the protocol rules on top of a transport.

The key job is telling a command's response apart from asynchronous lines.
Every line read is sorted as it arrives:

  OK ... / ERR ...      the response to the command in flight
  S ... / END           body of a DUMP response
  BOOT ... / EVT ...    events: queued with a timestamp, never mistaken for a
                        response, collected later by wait_for_event()
  anything else         noise (ESP32 ROM boot text, panic dumps): logged only

So a sample event that arrives in the middle of `STATUS` does not break it.
"""
from __future__ import annotations

import time
from collections import deque
from typing import Callable, Iterable, Optional

from .models import BootInfo, Event, Response, Sample
from .transport import Transport


class DeviceError(Exception):
    """The device answered, but not in the expected form."""


class DeviceTimeout(DeviceError):
    """Nothing (or not enough) arrived in time."""


class DeviceClient:
    def __init__(
        self,
        transport: Transport,
        timeout_scale: float = 1.0,
        restart: Optional[Callable[[], None]] = None,
    ) -> None:
        """timeout_scale multiplies every timeout (Wokwi runs slower than real
        time). restart power-cycles the target; None means "send RESET"."""
        self.transport = transport
        self.timeout_scale = timeout_scale
        self._restart = restart
        self.events: deque[Event] = deque()
        self.log: list[tuple[float, str, str]] = []   # (host time, "->"/"<-", text)
        self._t0 = time.perf_counter()
        self._has_hooks: Optional[bool] = None
        self.boot: Optional[BootInfo] = None

    # ---- low level ------------------------------------------------------------

    def _log(self, direction: str, text: str) -> None:
        self.log.append((time.perf_counter(), direction, text))

    def _deadline(self, timeout: float) -> float:
        return time.perf_counter() + timeout * self.timeout_scale

    def send(self, line: str) -> None:
        self._log("->", line)
        self.transport.write_line(line)

    def send_raw(self, data: bytes) -> None:
        self._log("->", f"<{len(data)} raw bytes> {data[:40]!r}{'...' if len(data) > 40 else ''}")
        self.transport.write_raw(data)

    def _next_line(self, deadline: float, stop_on_event: bool = False) -> Optional[tuple[float, str]]:
        """Next non-event line before the deadline. Events are queued on the way;
        with stop_on_event the call also returns right after queuing one, so an
        event waiter can check the queue without waiting out its deadline."""
        while True:
            remaining = deadline - time.perf_counter()
            if remaining <= 0:
                return None
            got = self.transport.read_line(remaining)
            if got is None:
                return None
            t, text = got
            self._log("<-", text)
            if not text:
                continue
            if text.startswith("BOOT ") or text.startswith("EVT "):
                event = Event.parse(text, t)
                if event.name == "BOOT":
                    self.boot = BootInfo.from_event(event)
                self.events.append(event)
                if stop_on_event:
                    return got
                continue
            if text.startswith(("OK", "ERR", "S ")) or text == "END":
                return got
            # ROM boot text, panic output, garbage: keep in the log only

    # ---- commands ------------------------------------------------------------------

    def command(self, cmd: str, timeout: float = 1.0) -> Response:
        """Send one command and return its OK/ERR response."""
        start = time.perf_counter()
        self.send(cmd)
        return self._read_response(cmd, start, timeout)

    def _read_response(self, cmd: str, start: float, timeout: float) -> Response:
        deadline = self._deadline(timeout)
        while True:
            got = self._next_line(deadline)
            if got is None:
                raise DeviceTimeout(f"no response to {cmd!r} within {timeout * self.timeout_scale:.1f} s")
            t, text = got
            if text.startswith(("OK", "ERR")):
                return Response.parse(text, (t - start) * 1000.0)
            # a stray S/END line (e.g. from an earlier DUMP) is not ours: skip it

    def read_response(self, timeout: float = 1.0) -> Response:
        """The next OK/ERR line, for commands that were sent with send_raw()."""
        return self._read_response("<pipelined>", time.perf_counter(), timeout)

    def expect_ok(self, cmd: str, timeout: float = 1.0) -> Response:
        resp = self.command(cmd, timeout)
        if not resp.ok:
            raise DeviceError(f"{cmd!r} failed: {resp.raw}")
        return resp

    def dump(self, n: Optional[int] = None, timeout: float = 2.0) -> list[Sample]:
        """DUMP [n]: the OK line, then exactly k `S` lines, then END."""
        cmd = "DUMP" if n is None else f"DUMP {n}"
        resp = self.expect_ok(cmd, timeout)
        if resp.keyword != "DUMP" or len(resp.args) != 1:
            raise DeviceError(f"bad DUMP header: {resp.raw}")
        k = int(resp.args[0])
        deadline = self._deadline(timeout)
        samples: list[Sample] = []
        while True:
            got = self._next_line(deadline)
            if got is None:
                raise DeviceTimeout(f"DUMP: got {len(samples)} of {k} samples, no END")
            text = got[1]
            if text == "END":
                break
            if not text.startswith("S "):
                raise DeviceError(f"DUMP: unexpected line {text!r}")
            _, t_ms, temp = text.split()
            samples.append(Sample.from_fields(t_ms, temp))
        if len(samples) != k:
            raise DeviceError(f"DUMP header said {k} samples, got {len(samples)}")
        return samples

    def status(self, timeout: float = 1.0) -> dict[str, object]:
        """STATUS as a dict; numeric values become ints. Key order is kept."""
        resp = self.expect_ok("STATUS", timeout)
        out: dict[str, object] = {}
        for pair in resp.args:
            key, _, value = pair.partition("=")
            out[key] = int(value) if value.isdigit() else value
        return out

    def read_temp(self, timeout: float = 1.0) -> float:
        resp = self.expect_ok("READ", timeout)
        return float(resp.args[0])

    def has_hooks(self) -> bool:
        """True if this build accepts FAULT commands (the test build)."""
        if self._has_hooks is None:
            self._has_hooks = self.command("FAULT I2C OFF").ok
        return self._has_hooks

    # ---- events ---------------------------------------------------------------------

    def _take_event(self, name: str) -> Optional[Event]:
        for i, event in enumerate(self.events):
            if event.name == name:
                del self.events[i]
                return event
        return None

    def wait_for_event(self, name: str, timeout: float) -> Event:
        """The first queued or arriving event called `name` (others stay queued)."""
        deadline = self._deadline(timeout)
        while True:
            event = self._take_event(name)
            if event is not None:
                return event
            if time.perf_counter() >= deadline:
                raise DeviceTimeout(f"no {name} event within {timeout * self.timeout_scale:.1f} s")
            self._next_line(deadline, stop_on_event=True)

    def wait_for_boot(self, timeout: float = 5.0) -> BootInfo:
        return BootInfo.from_event(self.wait_for_event("BOOT", timeout))

    def collect_events(self, name: str, duration: float) -> list[Event]:
        """All `name` events that arrive within `duration` (scaled) seconds."""
        deadline = self._deadline(duration)
        while time.perf_counter() < deadline:
            self._next_line(deadline)   # queues any events that arrive
        found = [e for e in self.events if e.name == name]
        self.events = deque(e for e in self.events if e.name != name)
        return found

    def drain(self, quiet: float = 0.2) -> list[str]:
        """Read until the line has been quiet for `quiet` s; return what came."""
        got: list[str] = []
        while True:
            line = self.transport.read_line(quiet * self.timeout_scale)
            if line is None:
                return got
            self._log("<-", line[1])
            got.append(line[1])

    def clear_events(self) -> None:
        self.events.clear()

    # ---- reset ---------------------------------------------------------------------

    def restart(self, timeout: float = 5.0) -> BootInfo:
        """Start from power-on state where possible (new sim process), else RESET."""
        self.clear_events()
        if self._restart is not None:
            self._restart()
        else:
            self.expect_ok("RESET")
        return self.wait_for_boot(timeout)

    # ---- logging -------------------------------------------------------------------

    def log_mark(self) -> int:
        return len(self.log)

    def format_log(self, since: int = 0, lines: Optional[Iterable[tuple[float, str, str]]] = None) -> str:
        rows = self.log[since:] if lines is None else lines
        return "\n".join(f"{t - self._t0:9.3f}s {d} {text}" for t, d, text in rows)
