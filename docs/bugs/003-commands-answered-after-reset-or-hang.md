# 003 — Commands sent right after RESET or FAULT HANG are still answered

Found: 2026-11-15, Project 1 Block 1 (first hand check of the host simulator) · Status: fixed 2026-11-15 (core: `logger_rx_byte` ignores input while an action is pending)
Severity: medium · Priority: high (breaks the protocol contract the harness relies on)

## Steps to reproduce
Send two commands back to back, without waiting for the first response:

    FAULT HANG
    PING

(or `RESET` followed by any command, for example `RATE 500`).

## Expected (docs/protocol.md)
`RESET`: "the next line is `BOOT ... reset=SOFTWARE`". A hung chip answers nothing. Commands that arrive while
a reset or hang is pending are lost, as they would be on a real reboot.

## Actual
Host simulator (`build/sim`), timestamps from the host:

    0.781s  OK FAULT HANG
    0.781s  OK PONG                          <- answered by a "hung" device
    3.781s  BOOT fw=1.0.0 reset=WATCHDOG

The same happens after `RESET`: `RESET` then `RATE 500` prints `OK RESET`, `OK RATE 500`, then `BOOT`, and the
new rate is silently lost in the reboot.

## Root cause
The core only *requests* a reset or hang (`lg->pending`); the platform layer acts on it after its input loop.
Both platform layers feed **every** waiting byte to `logger_rx_byte()` before checking the request:

- firmware: `while (Serial.available() > 0) logger_rx_byte(...)` in `src/main.cpp`
- simulator: the same pattern in `sim/host_main.c`

So any command already in the receive buffer runs before the reboot or hang takes effect.

## Fix
In the core: once a reset or hang is pending, `logger_rx_byte()` ignores further input. That fixes both
platforms in one place and is unit-testable. Unit test `test_input_after_reset_is_ignored` and HIL test
`HIL-RS-03` cover it.

## Evidence
Simulator transcript above (Python driver, 2026-11-15).

## Verification (2026-11-15)
- Unit tests `test_input_after_reset_is_ignored` and `test_input_after_hang_is_ignored` failed before the fix
  (`Was 'OK RATE 500'`, `Was 'OK PONG'`) and pass after it.
- Simulator re-run: `RESET` + `RATE 500` back to back now gives `OK RESET` then `BOOT ... reset=SOFTWARE`;
  `FAULT HANG` + `PING` gives `OK FAULT HANG` then, 3.0 s later, `BOOT ... reset=WATCHDOG`.
- Both ESP32 builds rebuilt with the fix (0 warnings).
