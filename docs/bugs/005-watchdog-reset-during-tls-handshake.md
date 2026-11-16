# 005 — Cloud firmware: task watchdog resets the ESP32 during the MQTT/TLS handshake

Found: 2026-11-16, ColdTrace Block 4 (first runs of the `esp32_cloud` build in Wokwi)
Status: fixed 2026-11-17 (task priorities in `src/main.cpp` and `src/cloud.cpp`); verification below
Severity: high · Priority: high (the device reboots instead of connecting; with a backlog it loses
its queued readings)

## Symptom
The cloud firmware rebooted with `abort() was called ... on core 0` and `rst:0x8 (TG1WDT_SYS_RESET)`,
then `BOOT ... reset=WATCHDOG`. The same firmware without Wi-Fi never did. The task watchdog
(3 s, panic) was firing while the MQTT client connected to Azure IoT Hub.

## How it was narrowed down
The watchdog's own report (which task starved) does not reach the serial port in this build, so the
cause was found in three steps, each confirmed on the device:

| Run | Change | Result |
| --- | --- | --- |
| 1 | — (esp-mqtt task at its default priority 5) | reboot loop ~6 s after every boot, during the first TLS handshake |
| 2 | core 0's idle task taken off the watchdog | same loop: the idle task was not (the only) cause |
| 3 | MQTT task at priority 1, equal to `loop()` | no crash, but Azure reset the TLS connection (`-0x0050`): `loop()` never blocks, so it took half the CPU and the ~31 s (simulated) handshake grew past Azure's patience |
| 4 | plus `delay(1)` at the end of each `loop()` pass | connects in ~35 s; outage tests pass, but one reboot after a 3-minute outage |
| 5 | plus a crash breadcrumb in RTC memory (`EVT WDT_STAGE n` after a watchdog reset) | reproduced in cycle 3 of 3 outages: **`EVT WDT_STAGE 6`, inside `delay(1)`** |

Stage 6 showed that no call in our code was blocking: `loop()` had asked to sleep for 1 ms and was not
scheduled again for over 3 s. At equal priority, the ESP32's dual-core (SMP) FreeRTOS does not
guarantee that a task waking from a delay gets the core back from a busy task of the same priority.

## Root cause
The TLS handshake is several seconds of CPU-bound certificate verification (an ECC chain, simulated
slowly by Wokwi) in the esp-mqtt task. Whenever that task's priority was equal to or above the loop
task's, the loop task, which feeds the 3 s watchdog, could be starved.

## Fix
- `loop()` runs at priority **2**, set in `setup()` (`vTaskPrioritySet(NULL, 2)`), above the MQTT
  task's **1** (`cfg.task_prio = 1`). When the loop's 1 ms sleep ends it preempts the TLS work at once.
- `loop()` ends each pass with `delay(1)`, so the MQTT task gets the CPU the rest of the time and the
  handshake is not slowed.
- Core 0's idle task is not watched (Arduino-ESP32 subscribes it by default); the protocol's watchdog
  guarantee is about the loop task.
- All three are in the `esp32_cloud` build only; the offline firmware is unchanged.

The breadcrumb (`EVT WDT_STAGE`) stays in the cloud build: a watchdog reset now always says where the
loop was.

## Verification
- Outage cycles (3 min, 1 min, 3 min `FAULT NET`) on the fixed firmware: see the table in
  `docs/spike-notes.md` / the Block 4 results.
- Unit tests unaffected (the fix is in the platform layer).

## Lesson
A watchdog that guards a control loop also defines a scheduling contract: every task that can hold the
CPU for seconds must run below the watched task. Equal priorities are not a contract on SMP FreeRTOS.
