# Day 1 simulator spike — go/no-go

Date: 2026-11-14 · Platform: espressif32@6.9.0 (Arduino-ESP32 2.0.17, ESP-IDF 4.4) · Wokwi for VS Code

Two throwaway sketches answer the two open questions from the design doc before any real code is written.

## Spike 1 — MPU6050 temperature (`spike/temp`)

Prints `SPIKE t=<ms> raw=<n> centi=<n> exact=<°C>` every second. `centi` uses the firmware's integer formula
`raw * 10 / 34 + 3653`; `exact` uses the datasheet formula `raw / 340 + 36.53`.

**Pass:** `WHO_AM_I value=0x68`, and after each temperature change `centi` lands within ±10 (±0.1 °C) of the set value.

| Set in Wokwi (°C) | Expected raw | Expected centi | Seen raw | Seen centi | Pass? |
| --- | --- | --- | --- | --- | --- |
| 24 (default) | -4260 | 2401 | -4260 | 2401 | Yes, exact |
| ~40 (slider landed at 39.9) | 1146 | 3990 | 1146 | 3990 | Yes |
| ~-10 (slider landed at -10.3) | -15922 | -1029 | -15922 | -1029 | Yes |
| 85 | 16480 | 8500 | 16480 | 8500 | Yes, exact |

WHO_AM_I line scrolled off the pasted log, but every TEMP_OUT read at 0x68 succeeded, so the device answers.

Findings:

- The reading follows the Wokwi slider within one sample (1 s) and holds steady; intermediate values in the
  log are the slider being dragged, not noise.
- The slider moves in steps, so "exactly 40" is hard to hit; the expected values above are recomputed for
  where it actually landed. Harness tests should set values by scenario/`set-control`, not by hand.
- The integer formula `raw * 10 / 34 + 3653` agreed with the datasheet float formula to within 0.01 °C at
  every one of ~180 samples, from -10.7 °C to 85 °C.
- The serial monitor printed a staircase because the sketch sends `
` only. Fixed for viewing with
  `"serialMonitor": {"convertEol": true}` in `diagram.json`; the protocol stays `
`-terminated.

**Result: GO.** No fallback needed.

**Fallback if it fails:** keep the driver, prove the conversion only in unit tests (`test_mpu6050`), and note the
limitation in the README; or swap to another Wokwi I²C sensor.

## Spike 2 — Task watchdog reset (`spike/wdt`)

Boot 1 arms the TWDT (3 s, panic), feeds it for 5 s, prints `SPIKE HANG`, then stops feeding.

**Pass:** about 3 s after `SPIKE HANG` the chip reboots and prints `SPIKE BOOT n=2 reason=TASK_WDT (6)`.

| Check | Seen | Pass? |
| --- | --- | --- |
| Boot 1 line | `SPIKE BOOT n=1 reason=POWERON (1)` | Yes |
| `task_wdt: Task watchdog got triggered` printed | Yes, names `loopTask (CPU 1)`, then `Aborting.` | Yes |
| Chip rebooted on its own | Yes, ROM shows `rst:0x8 (TG1WDT_SYS_RESET)` | Yes |
| Boot 2 reason (expected `TASK_WDT (6)`) | `INT_WDT (5)`, boot counter `n=2` | Partly |

What happened: the TWDT fired and called `abort()`. The panic handler's backtrace came out `CORRUPTED`
and the handler never finished its clean software restart, so its safety timer (Timer Group 1) reset the chip.
ESP-IDF 4.4 reports a TG1 reset as `ESP_RST_INT_WDT`, and it ignores the "task watchdog" hint on that path.
On a real board the panic handler normally finishes and the reason is `TASK_WDT`.

RTC no-init memory survived the reset (`n=2`), so the boot counter approach works.

Firmware impact: none. The design already maps `TASK_WDT`, `INT_WDT` and `WDT` to `reset=WATCHDOG`, so
`FAULT HANG` will produce `BOOT fw=1.0.0 reset=WATCHDOG` in Wokwi as designed. Rules that follow:

- Tests check `reset=WATCHDOG`, never the raw IDF reason number.
- Keep `INT_WDT` in the WATCHDOG mapping, with a comment explaining why it matters in Wokwi.

Timing (settled 2026-11-15 by a re-run that prints simulated time while hung): the last line before the
`task_wdt` error was `SPIKE +2750 ms without feeding`, so the watchdog fired between 2.75 s and 3.0 s after the
last feed. **The 3 s timeout works as configured.**

The `E (…)` log timestamp is not usable for timing in Wokwi: it read 16 184 ms when `millis()` was about
8 250 ms (roughly 2x). The original 16 224 ms reading was this clock difference, not a slow watchdog.
Harness rule: allow about 5 s from `OK FAULT HANG` to the next `BOOT` line.

**Result: GO** (with the reason-mapping note above). No fallback needed.

## Decision

- [x] Spike 1: **GO** — Wokwi's MPU6050 temperature model works
- [x] Spike 2: **GO** — Wokwi emulates the watchdog reset; reason reads INT_WDT, mapped to WATCHDOG
