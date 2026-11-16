# 005 — Cloud firmware: watchdog resets around connecting to Azure (two causes)

Found: 2026-11-16, ColdTrace Block 4 (first runs of the `esp32_cloud` build in Wokwi)
Reopened: 2026-11-17 (end-to-end suite: two watchdog resets right after `RESET`)
Status: fixed 2026-11-17, both causes (cause A corrected after a third occurrence); verification below
Severity: high · Priority: high (the device reboots instead of connecting; with a backlog it loses
its queued readings, and `RESET` reports the wrong reset reason)

## Symptom
The cloud firmware sometimes came back with `rst:0x8 (TG1WDT_SYS_RESET)` and `BOOT ... reset=WATCHDOG`.
The same firmware without Wi-Fi never did. Two different mechanisms produced this one symptom, and the
first fix hid the second for a while.

## Cause A: the TLS handshake starved the loop task

| Run | Change | Result |
| --- | --- | --- |
| 1 | — (esp-mqtt task at its default priority 5) | reboot loop ~6 s after every boot, during the first TLS handshake |
| 2 | core 0's idle task taken off the watchdog | same loop |
| 3 | MQTT task at priority 1, equal to `loop()` | no crash, but Azure reset the TLS connection (`-0x0050`): a loop that never blocks took half the CPU and the ~31 s (simulated) handshake grew past Azure's patience |
| 4 | plus `delay(1)` at the end of each `loop()` pass | connects in ~35 s; one reboot after a 3-minute outage |
| 5 | plus a crash breadcrumb in RTC memory (`EVT WDT_STAGE n` after a watchdog reset) | reproduced: **`WDT_STAGE 6`**, the loop asked to sleep 1 ms and was not scheduled again for over 3 s |
| 6 | loop at priority **10**, a hardware-timer loop-gap monitor (`EVT GAP`), task report (`EVT TASKS`) | full end-to-end suite: **no loop gap of 500 ms or more** at any time; the device reports `EVT TASKS loop=10/1 mqtt=1/any` |

`EVT TASKS` confirms the settings took effect: the loop runs at 10 on CPU 1, the MQTT task at 1 and not
pinned to a core. It also shows the resets seen after the priority-2 build (below) were not starvation.

The TLS handshake is seconds of CPU-bound certificate verification in the esp-mqtt task, which runs on
the same CPU as `loop()`. With the MQTT task at or above the loop's priority, the loop, which feeds the
3 s task watchdog, could be starved. Priority 10 puts the loop above esp-mqtt whatever priority it
actually runs at (default 5), and below Wi-Fi and TCP/IP (18-23). The loop sleeps 1 ms per pass, so the
handshake still gets almost all of the CPU.

### Correction: priorities were not the fix

During hands-on testing after the "fix" above, the watchdog fired again 10 s after `FAULT NET OFF`
(reconnect), and once more 6 s into the next boot; the 24 readings queued during the outage were lost
in those reboots. The report named `loopTask (CPU 1)` starved with `CPU 1: mqtt_task` running, and the
breadcrumb was `WDT_STAGE 6`: the loop, at **priority 10**, was asleep in its 1 ms `delay()` while the
**priority 1** MQTT task kept CPU 1. Priority scheduling cannot do that unless CPU 1 is not taking
interrupts at all, so the stall is inside the TLS/crypto work of ESP-IDF's precompiled libraries, which
this project cannot change. The earlier clean runs were luck; the priority change does no harm but is
not what prevents the reset.

The loop-gap monitor then measured it: with a 10 s watchdog, the next end-to-end run recorded
**`EVT GAP 3348 stage=6`**, a 3.3 s freeze during the reconnect handshake right after `FAULT NET OFF`,
0.7 s before `EVT NET_UP`. That freeze alone exceeds the old 3 s watchdog.

**Actual fix for cause A:** the cloud build's task watchdog is **10 s** (the offline builds keep 3 s).
A watchdog must outlast the longest legitimate pause, and in Wokwi a TLS handshake pauses the CPU for
over 3 s. On a real ESP32 the same work takes milliseconds. The guard in every cloud test fails on any
watchdog reboot and on any loop stall of 5 s (half the watchdog) or more, and records every stall.

## Cause B: the interrupt watchdog survived `RESET`
After the priority fix, the end-to-end suite still saw `reset=WATCHDOG` twice, both right after the
harness sent `RESET`, and with no loop gap at all. The serial log showed the sequence exactly:

```
-> RESET
<- OK RESET
<- rst:0xc (SW_CPU_RESET)        the software reset worked ...
<- rst:0x8 (TG1WDT_SYS_RESET)    ... and 0.6 s later, still in the bootloader, a second reset
<- BOOT fw=2.0.0 reset=WATCHDOG
```

Timer group 1 is the ESP32's interrupt watchdog. It was still counting across the software reset and
fired before the application started. The breadcrumb from the morning's crashes agreed: `WDT_STAGE 5`,
the "carry out the pending action" step. In Wokwi a software reset does not reset every peripheral (the
same family of behaviour as [bug 002](002-uptime-not-reset-after-software-reset.md)); with Wi-Fi and
MQTT running, `esp_restart()` also has much more to shut down.

## Fix
- **A:** task watchdog 10 s in the cloud build (the fix); also kept: `loop()` at priority 10, MQTT task
  at 1, 1 ms sleep per pass, core 0's idle task not watched, `WiFi.persistent(false)`.
- **B:** `RESET` goes through `cloud_prepare_restart()`: take the loop off the task watchdog, stop the
  MQTT client, turn Wi-Fi off, then clear the **flash-boot protection bit** and disable the watchdog on
  both timer groups, then `esp_restart()`. The first attempt only disabled the interrupt watchdog, which
  ESP-IDF's restart already does, and the reset still happened: the flash-boot bit re-arms the watchdog
  while booting from flash, and the bootloader takes longer than it allows to load this 920 KB image
  (the smaller Day 2 image boots in time, which is why `RESET` worked there).
- **Safety net for the reported reason:** a `restart requested` flag in RTC memory is set just before
  `esp_restart()`. If the next boot finds it together with a watchdog reason, the reset happened inside the
  requested restart: it reports `reset=SOFTWARE` and prints `EVT RESET_CHAIN sw>wdt`. The flag is cleared
  at every boot, so a real hang still reads `WATCHDOG` (checked by `HIL-CT-07`). In the final run the
  chain never occurred (`reset_chain_events: 0`): clearing the bit fixed the cause.
- Kept as permanent diagnostics in the cloud build: `EVT WDT_STAGE` (breadcrumb after a watchdog reset),
  `EVT GAP` (loop stalls of 500 ms or more), `EVT TASKS` (actual task priorities and cores).
- All of it is in the `esp32_cloud` build only; the offline firmware is unchanged.

## Verification
- Every cloud test now fails if the device reboots by watchdog or reports a loop gap of 1.5 s or more
  during the test (`cloud_ready` fixture).
- `HIL-CT-07` reboots the device three times; each must come back `reset=SOFTWARE`, reconnect and
  deliver a reading.
- After the correction (firmware `90b4bb3f`): end-to-end suite **7 / 7**, then four more outage and
  reconnect cycles (3, 2, 3, 2 minutes), all readings delivered, no watchdog reset; one loop stall
  recorded, 3,348 ms, during a reconnect handshake.
- Earlier end-to-end run (2026-11-17, firmware `db51abff`): **7 / 7 pass** in 13 minutes. `HIL-CT-07`: three
  `RESET`s, each back as `SOFTWARE` and delivering a reading ~59 s later; `FAULT HANG` → `WATCHDOG`; no
  `EVT RESET_CHAIN`; no loop gap of 500 ms or more anywhere in the run.
- Details in [the test plan](../test-plan.md) and `reports/wokwi-cloud-report.html`.

## Lessons
1. Measure before concluding. The priority theory fitted the first evidence and was wrong; the loop-gap
   monitor, by timing the stalls the watchdog only reports as "too long", is what settled it. A
   watchdog's timeout must exceed the longest legitimate pause on the platform it runs on.
2. One symptom can have two causes. The breadcrumb showed different stages (6 and 5) for the two; reading
   which stage, not just "it crashed", is what separated them.
