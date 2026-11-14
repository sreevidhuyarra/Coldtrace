# 004 — UART receive buffer overflows on bursts over ~1.5 KB; the next command is lost

Found: 2026-11-15, Project 1 Block 5 (first full HIL run against Wokwi, test `HIL-RB-04`)
Status: fixed 2026-11-15 (`Serial.setRxBufferSize(4096)` in `src/main.cpp`)
Severity: medium · Priority: medium (a host that sends a burst silently loses its next command)

## Steps to reproduce
On the ESP32 firmware (Wokwi, `esp32_test` build), send one burst of 2000 `x` bytes followed by `\n`, then `PING`.

## Expected
One `ERR TOOLONG` for the overlong line, then `OK PONG`.

## Actual
No response to the burst at all; then `PING` gets `ERR TOOLONG`. The burst's newline was dropped, so the
parser was still discarding the overlong line when `PING` arrived and swallowed it.

## Investigation
Probing with bursts of increasing size (`HIL` client over RFC2217, firmware `v1.0.0` + bug 003 fix):

| Burst | Response to burst | Then `PING` |
| --- | --- | --- |
| 100 – 1400 bytes | `ERR TOOLONG` | `OK PONG` |
| 1600 – 2000 bytes | nothing | `ERR TOOLONG` |
| 2000 bytes, in 100-byte chunks 0.2 s apart | `ERR TOOLONG` | `OK PONG` |
| 4000 bytes, in 100-byte chunks 0.2 s apart | `ERR TOOLONG` | `OK PONG` |

The host simulator, which runs the same parser, handled a 20 000-byte burst correctly. So the parser was ruled
out: bytes were lost in the serial receive path, and only when they arrived faster than they were read.

Two suspects: Arduino-ESP32's 256-byte default receive buffer, or Wokwi's RFC2217 bridge. To decide,
the firmware's buffer was raised to 4 KB and the probe repeated:

| Burst (4 KB buffer) | Response to burst | Then `PING` |
| --- | --- | --- |
| 1400 – 8000 bytes | `ERR TOOLONG` | `OK PONG` |

The limit moved, so the firmware's receive buffer was the cause.

## Root cause
`Serial` was started with Arduino-ESP32's default 256-byte receive buffer. `loop()` drains it every pass, but
a long enough burst still fills it between passes, and the UART driver drops what does not fit, including
any line ending inside the lost part.

## Fix
`Serial.setRxBufferSize(4096)` before `Serial.begin()` (4 KB of the ESP32's ~320 KB RAM). Bursts of at least
8000 bytes now arrive intact, because `loop()` drains the buffer while the burst is still arriving.

## Verification
- Probe above, after the fix: every size up to 8000 bytes answered correctly.
- `HIL-RB-04` (2 KB random burst) passes on Wokwi with the fixed firmware.

## Not fixed (by design)
No buffer survives an unlimited flood. If bytes are ever lost, a host can resynchronise by sending an empty
line, which ends any partial line and is otherwise ignored. This is noted in `docs/protocol.md`.
