# 002 — uptime_ms appears not to restart from 0 after RESET in Wokwi

Found: 2026-11-15, Block 6 manual run · Status: confirmed Wokwi limitation, won't fix in firmware

## Observed
After `RESET` (BOOT line said `reset=SOFTWARE`), a later `STATUS` printed:

    uptime_ms=116316 samples=29 rate_ms=1000 sensor=OK errors=4 overflows=0

## Why that is inconsistent
- At 1 sample/s, 116 s of uptime should have filled the 64-slot buffer and given `overflows` > 0.
- `errors=4` means the sensor was LOST for at most about 10 s (3 failed reads + 1 failed retry), so
  that does not explain the missing samples.
- 29 samples plus about 13 s lost suggests only about 42 s of sampling since the reboot, so `millis()` probably
  started at about 74 s rather than 0. That is roughly when `RESET` was sent.

## Likely cause
Wokwi's software reset (`SW_CPU_RESET`) does not seem to reset the timer behind `millis()`. On a real
ESP32, `esp_restart()` resets it. The firmware has no state that carries `millis()` across a reboot.

## Impact
A harness test like "uptime_ms is small right after RESET" would fail in Wokwi only.

## Next step
Confirm: send `RESET`, then `STATUS` straight away. If `uptime_ms` is far above ~1000, this is confirmed.
If confirmed, document it as a Wokwi limitation; do not change the firmware.

## Confirmation (2026-11-15)
`STATUS` at uptime 12 383 ms, then `RESET`, then `STATUS` straight after the new BOOT line:

    BOOT fw=1.0.0 reset=SOFTWARE
    OK STATUS uptime_ms=21750 samples=2 ...

Two samples means about 2 s since the reboot, yet `uptime_ms` kept counting from before it. Confirmed.

## Resolution
Documented in the README under "Known Wokwi limitations". Harness tests must not assume `uptime_ms` restarts
from 0 after `RESET`; check the `BOOT ... reset=SOFTWARE` line instead.
