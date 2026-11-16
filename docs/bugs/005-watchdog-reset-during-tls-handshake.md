# 005 — Cloud firmware: watchdog resets around connecting to Azure (two causes)

Found: 2026-11-16, ColdTrace Block 4 (first runs of the `esp32_cloud` build in Wokwi)
Reopened: 2026-11-17 (end-to-end suite: two watchdog resets right after `RESET`)
Status: fixed 2026-11-17, both causes; verification below
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
- **A:** `loop()` at priority 10 (`vTaskPrioritySet` in `setup()`), MQTT task asked for priority 1,
  1 ms sleep per pass, core 0's idle task not watched, `WiFi.persistent(false)` (no flash writes when
  joining).
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
- Final end-to-end run (2026-11-17, firmware `db51abff`): **7 / 7 pass** in 13 minutes. `HIL-CT-07`: three
  `RESET`s, each back as `SOFTWARE` and delivering a reading ~59 s later; `FAULT HANG` → `WATCHDOG`; no
  `EVT RESET_CHAIN`; no loop gap of 500 ms or more anywhere in the run.
- Details in [the test plan](../test-plan.md) and `reports/wokwi-cloud-report.html`.

## Lessons
1. A watchdog that guards a control loop defines a scheduling contract: every task that can hold the
   CPU for seconds must run below the watched task.
2. One symptom can have two causes. The breadcrumb showed different stages (6 and 5) for the two; reading
   which stage, not just "it crashed", is what separated them.
