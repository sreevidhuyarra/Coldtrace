"""Transports: move text lines to and from a device. Nothing protocol-specific.

Each transport runs a background reader thread that splits incoming bytes into
lines and queues them with their arrival time, so `read_line()` never blocks
longer than its timeout and no line is lost while the test is busy.
"""
from __future__ import annotations

import os
import queue
import subprocess
import threading
import time
from pathlib import Path
from typing import Optional, Protocol

Line = tuple[float, str]  # (time.perf_counter() at arrival, text without line ending)


class Transport(Protocol):
    def write_line(self, line: str) -> None: ...
    def write_raw(self, data: bytes) -> None: ...
    def read_line(self, timeout: float) -> Optional[Line]: ...
    def close(self) -> None: ...


class _LineQueue:
    """Bytes in, complete lines out. Accepts \\n, \\r\\n (and strips stray \\r)."""

    def __init__(self) -> None:
        self._q: "queue.Queue[Line]" = queue.Queue()
        self._partial = bytearray()

    def feed(self, data: bytes) -> None:
        self._partial += data
        while True:
            nl = self._partial.find(b"\n")
            if nl < 0:
                return
            raw = bytes(self._partial[:nl])
            del self._partial[: nl + 1]
            text = raw.decode("ascii", errors="replace").strip("\r")
            self._q.put((time.perf_counter(), text))

    def get(self, timeout: float) -> Optional[Line]:
        try:
            return self._q.get(timeout=max(timeout, 0.0))
        except queue.Empty:
            return None


class ProcessTransport:
    """The host simulator as a child process, over its stdin/stdout."""

    def __init__(self, sim_path: Path, temp_c: float = 24.0) -> None:
        self.sim_path = Path(sim_path)
        self.temp_c = temp_c
        self._proc: Optional[subprocess.Popen] = None
        self._lines = _LineQueue()
        self.start()

    def start(self) -> None:
        if not self.sim_path.exists():
            raise FileNotFoundError(f"{self.sim_path} not found - run `make sim` first")
        self._lines = _LineQueue()
        self._proc = subprocess.Popen(
            [str(self.sim_path), "--temp", str(self.temp_c)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            bufsize=0,
        )
        threading.Thread(target=self._reader, args=(self._proc, self._lines), daemon=True).start()

    @staticmethod
    def _reader(proc: subprocess.Popen, lines: _LineQueue) -> None:
        assert proc.stdout is not None
        fd = proc.stdout.fileno()
        while True:
            try:
                data = os.read(fd, 4096)   # returns whatever has arrived
            except OSError:
                return
            if not data:
                return
            lines.feed(data)

    def restart(self) -> None:
        """Power-cycle: a brand-new simulator process."""
        self.close()
        self.start()

    def write_raw(self, data: bytes) -> None:
        assert self._proc is not None and self._proc.stdin is not None
        self._proc.stdin.write(data)
        self._proc.stdin.flush()

    def write_line(self, line: str) -> None:
        self.write_raw(line.encode("ascii") + b"\n")

    def read_line(self, timeout: float) -> Optional[Line]:
        return self._lines.get(timeout)

    def close(self) -> None:
        if self._proc is None:
            return
        try:
            if self._proc.stdin:
                self._proc.stdin.close()   # EOF: the simulator exits on its own
            self._proc.wait(timeout=2)
        except (subprocess.TimeoutExpired, OSError):
            self._proc.kill()
            self._proc.wait()
        self._proc = None


class SerialTransport:
    """A serial port: a real one (`COM3`, `/dev/ttyUSB0`) or a pyserial URL such
    as `rfc2217://localhost:4000` for Wokwi."""

    def __init__(self, url: str, baudrate: int = 115200) -> None:
        import serial  # pyserial; imported here so the sim target does not need it

        self.url = url
        self._ser = serial.serial_for_url(url, baudrate=baudrate, timeout=0.05)
        self._lines = _LineQueue()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    def _reader(self) -> None:
        while not self._stop.is_set():
            try:
                data = self._ser.read(self._ser.in_waiting or 1)
            except Exception:  # port closed under us
                return
            if data:
                self._lines.feed(data)

    def write_raw(self, data: bytes) -> None:
        self._ser.write(data)
        self._ser.flush()

    def write_line(self, line: str) -> None:
        self.write_raw(line.encode("ascii") + b"\n")

    def read_line(self, timeout: float) -> Optional[Line]:
        return self._lines.get(timeout)

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1)
        self._ser.close()
