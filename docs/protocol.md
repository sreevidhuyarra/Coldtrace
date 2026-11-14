# Serial command protocol — v1.0.0

The firmware is driven entirely over UART with plain-text commands. This document is the contract that the
Project 1 test harness checks; every row below has at least one unit test in `firmware/test/test_logger.c`.

## Link settings

| Setting | Value |
| --- | --- |
| Baud rate | 115200 |
| Framing | 8 data bits, no parity, 1 stop bit (8N1) |
| Encoding | ASCII |
| In Wokwi | VS Code serial terminal, or `rfc2217://localhost:4000` (pyserial) |

## Framing

- **Input** lines end with `\n`, `\r` or `\r\n`. Terminals send a bare `\r` for Enter; the `\n` of a `\r\n`
  then arrives as an empty line, which is ignored.
- **Output** lines always end with `\r\n`. A harness should strip the `\r`.
- At most **64 characters** per input line. A longer line is discarded up to its line ending and answered once
  with `ERR TOOLONG`.
- Commands are **upper-case**. Arguments are separated by **exactly one space**; a leading, trailing or double
  space is answered with `ERR ARGS`. At most 3 arguments.
- Empty lines are ignored (no response). A host that may have lost bytes (for example after a very long
  burst; the firmware buffers 4 KB, see [bug 004](bugs/004-uart-rx-overflow-drops-bytes-in-bursts.md)) can
  resynchronise by sending an empty line: it ends any partial line and is otherwise ignored.
- Input that arrives after `OK RESET` or `OK FAULT HANG` is discarded; the next line from the device is
  `BOOT` ([bug 003](bugs/003-commands-answered-after-reset-or-hang.md)).
- Every command gets **exactly one** response line starting with `OK` or `ERR`. `DUMP` then adds its sample
  lines and `END`.
- Lines the firmware sends on its own start with `BOOT` or `EVT`.

## Commands

| Command | Success response | Error responses | Notes |
| --- | --- | --- | --- |
| `PING` | `OK PONG` | `ERR ARGS` | Liveness check |
| `VERSION` | `OK VERSION 1.0.0` | `ERR ARGS` | Firmware version |
| `STATUS` | `OK STATUS uptime_ms=<n> samples=<n> rate_ms=<n> sensor=OK\|LOST errors=<n> overflows=<n> reset=<reason>` | `ERR ARGS` | Fields always in this order |
| `READ` | `OK TEMP 24.01` | `ERR SENSOR`, `ERR ARGS` | Immediate read, not stored |
| `RATE <ms>` | `OK RATE 500` | `ERR ARGS`, `ERR RANGE` | Valid 100–60000; default 1000 |
| `DUMP [n]` | `OK DUMP <k>`, then *k* lines `S <t_ms> <temp>`, then `END` | `ERR ARGS` | The newest *n* samples (all if omitted), oldest first |
| `CLEAR` | `OK CLEAR` | `ERR ARGS` | Empties the buffer; `overflows` is kept |
| `STREAM ON\|OFF` | `OK STREAM ON` / `OK STREAM OFF` | `ERR ARGS` | While on, each sample prints `EVT S <t_ms> <temp>` |
| `RESET` | `OK RESET` | `ERR ARGS` | Software reboot; the next line is `BOOT ... reset=SOFTWARE` |
| anything else | — | `ERR UNKNOWN` | Includes lower-case commands |

Commands that take no arguments answer `ERR ARGS` if given any (`PING now`).

### Field formats

- `<temp>`: degrees Celsius with exactly two decimals and a leading `-` when negative: `24.01`, `-0.89`,
  `85.00`. Resolution 0.01 °C; accuracy against the sensor's own formula is better than ±0.01 °C.
- `<t_ms>`, `uptime_ms`: milliseconds since boot, unsigned 32-bit (wraps after about 49.7 days).
- `samples`: samples in the buffer, 0–64.
- `errors`: failed I²C transactions since boot (including retries while the sensor is LOST).
- `overflows`: samples lost because the 64-slot buffer was full (oldest overwritten), since boot.

## Test-hook commands

Only in the `esp32_test` build (`-DLOGGER_TEST_HOOKS`), which is the one Wokwi runs. In the production `esp32`
build they answer `ERR UNKNOWN`.

| Command | Response | Effect |
| --- | --- | --- |
| `FAULT I2C ON\|OFF` | `OK FAULT I2C ON` / `OFF` | While ON, every I²C transaction fails (simulates an unplugged sensor) |
| `FAULT HANG` | `OK FAULT HANG` | The main loop stops feeding the watchdog; a watchdog reset follows |

## Unsolicited messages

| Message | When |
| --- | --- |
| `BOOT fw=1.0.0 reset=POWERON\|SOFTWARE\|WATCHDOG\|OTHER` | Once after every start |
| `EVT SENSOR_LOST` | 3 failed reads in a row (scheduled or `READ`), or the sensor was missing at boot |
| `EVT SENSOR_OK` | The 5 s retry found the sensor again |
| `EVT S <t_ms> <temp>` | A new sample while streaming is on |

## Timing guarantees

| Behaviour | Guarantee |
| --- | --- |
| Sample spacing | `rate_ms` (±1 loop pass) |
| Sensor marked LOST | On the 3rd failed read in a row; 1 or 2 failures are tolerated |
| Retry while LOST | Every 5000 ms; commands keep working meanwhile |
| Watchdog | Loop stalled for 3 s → reset, reported as `reset=WATCHDOG` |

## Example session

```text
BOOT fw=1.0.0 reset=POWERON
> PING
OK PONG
> RATE 500
OK RATE 500
> DUMP 2
OK DUMP 2
S 57507 24.01
S 58007 24.01
END
> FAULT I2C ON
OK FAULT I2C ON
EVT SENSOR_LOST
> READ
ERR SENSOR
> FAULT I2C OFF
OK FAULT I2C OFF
EVT SENSOR_OK
```

(`>` marks lines sent to the device.)

## Wokwi notes for harness authors

- **Reset reason:** a watchdog reset shows up in Wokwi as ESP-IDF's `INT_WDT`, not `TASK_WDT`. Both map to
  `reset=WATCHDOG`, so check that string, never a raw reason number.
- **Uptime after `RESET`:** Wokwi does not restart `uptime_ms` from 0 after a software reset
  ([bug 002](bugs/002-uptime-not-reset-after-software-reset.md)). Check the `BOOT ... reset=SOFTWARE` line instead.
- Set the sensor temperature from a scenario or the diagram rather than the slider; the slider moves in steps.
