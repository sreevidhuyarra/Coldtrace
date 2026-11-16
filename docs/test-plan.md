# Test plan — ESP32 temperature logger firmware v2.0.0

Every requirement in [protocol.md](protocol.md) is tested automatically by the hardware-in-the-loop (HIL)
harness, on the host simulator in CI and on the real firmware in Wokwi. Final run: **38/38 pass on the
simulator; 36 pass, 1 not applicable, 1 known Wokwi limitation on Wokwi.** The ColdTrace cloud
requirements (v2.0.0) are in [section 8](#8-coldtrace-cloud-v200).

## 1. Scope

| In scope | Out of scope |
| --- | --- |
| The UART command protocol (every command, error response and unsolicited line) | Accelerometer and gyro data |
| Sampling schedule, ring buffer, streaming | Wi-Fi, BLE, flash storage |
| Sensor fault detection and recovery | Electrical tests (brown-out, signal integrity) |
| Software and watchdog resets | A real ESP32 board (the `serial` target exists; no board run yet) |
| Robustness against malformed input and floods | Multi-device load testing |
| Command latency | |

## 2. Approach

Three levels, each catching what the one below cannot:

| Level | What runs | Catches | Command |
| --- | --- | --- | --- |
| Unit | 59 Unity tests on the C core with a fake HAL (fake I²C bus, fake clock) | Logic errors, boundaries, timing arithmetic | `make test` |
| HIL on the host simulator | The same core compiled for the PC (`sim/host_main.c`), driven by the pytest harness over stdin/stdout | Protocol behaviour end to end, fast and deterministic; runs in CI | `pytest --target sim` |
| HIL on Wokwi | The real ESP32 firmware in the Wokwi simulator, over RFC2217 | Platform-layer bugs: UART, watchdog, reset reasons, timing | `pytest --target wokwi` |

Design rules for the HIL suite:

- **Black box.** Tests use only the serial protocol, so the same 38 tests run on every target.
- **Independent.** An autouse fixture resets state before every test (stream off, fault off, rate 1000,
  buffer cleared, sensor OK). The suite passes in random order (`pytest -p random_order`).
- **No sleeping for results.** Tests wait for the event or response they need, with timeouts scaled per
  target (Wokwi ×4). Timing checks use the device's own timestamps, not host time.
- **Fault injection** through test-hook commands (`FAULT I2C`, `FAULT HANG`) that exist only in the test
  build; a test proves the production build rejects them.
- **Evidence.** Every test's report carries the full timestamped serial conversation.

## 3. Environments

| Target | Firmware | Transport | Where | Timeout scale |
| --- | --- | --- | --- | --- |
| `sim` | `build/sim` (core + host platform, test hooks); `build/sim_prod` for the production check | Process stdin/stdout | Local (Windows) and GitHub Actions (Ubuntu) | ×1 |
| `wokwi` | `esp32_test` (Arduino-ESP32 2.0.17) | `rfc2217://localhost:4000` | Local: VS Code + Wokwi extension | ×4 |
| `serial` | `esp32_test` on a board | `COM3` / `/dev/ttyUSB0` at 115200 | Future | ×1.5 |

Tools: Python 3.12, pytest 9.2.0, pytest-html 4.2.0, pyserial 3.5, pytest-random-order 1.2.0; gcc (MSYS2
UCRT64 on Windows, Ubuntu in CI); PlatformIO with espressif32 6.9.0.

## 4. Entry and exit criteria

**Entry:** `make test` passes; `make sim` and both PlatformIO builds compile with zero warnings; the firmware
boots and prints `BOOT fw=2.0.0`.

**Exit (release):**

- All HIL tests pass on `sim`, in random order, in under 60 s.
- On `wokwi`, every test passes or is `xfail` with a written reason that points to a bug report.
- Every requirement below has at least one passing test.
- No open bug of severity medium or higher.
- CI is green on `main` and publishes the HTML report.

All exit criteria are met for v1.1.0.

## 5. Traceability

Test IDs appear in each test's docstring (`hil/tests/`). Unit tests are in `firmware/test/`.

| Req | Requirement (protocol.md) | HIL test | Unit test | sim | wokwi |
| --- | --- | --- | --- | --- | --- |
| R-01 | `BOOT fw=… reset=…` once after every start | HIL-SM-01, RS-01, RS-02 | `test_boot_*` | Pass | Pass |
| R-02 | `PING` → `OK PONG` | HIL-SM-02 | `test_ping_and_version` | Pass | Pass |
| R-03 | `VERSION` → `OK VERSION 2.0.0` | HIL-SM-03 | `test_ping_and_version` | Pass | Pass |
| R-04 | `STATUS` has all 7 fields in fixed order | HIL-SM-04 | `test_status_reports_every_field_in_fixed_order` | Pass | Pass |
| R-05 | `READ` within ±0.1 °C of the sensor | HIL-FN-01 | `test_conversion_*` | Pass | Pass |
| R-06 | `READ` → `ERR SENSOR` when the sensor fails | HIL-FT-01 | `test_three_failures_mark_lost_once_*` | Pass | Pass |
| R-07 | `RATE` accepts 100–60000, else `ERR RANGE` | HIL-FN-02 (4 cases) | `test_rate_boundaries` | Pass | Pass |
| R-08 | `RATE` with a missing or bad argument → `ERR ARGS` | HIL-FN-03 (4 cases) | `test_rate_rejects_non_numbers` | Pass | Pass |
| R-09 | Samples taken every `rate_ms` | HIL-FN-04 | `test_samples_are_taken_every_rate_ms` | Pass | Pass |
| R-10 | `DUMP` lists samples oldest first | HIL-FN-05 | `test_dump_lists_newest_n_samples_oldest_first` | Pass | Pass |
| R-11 | `DUMP n` returns the newest *n* | HIL-FN-06 | same | Pass | Pass |
| R-12 | `CLEAR` empties the buffer | HIL-FN-07 | `test_clear_empties_the_buffer` | Pass | Pass |
| R-13 | `STREAM ON` emits `EVT S`; `OFF` stops it | HIL-FN-08 | `test_stream_prints_each_sample_until_turned_off` | Pass | Pass |
| R-14 | Unsolicited lines never corrupt command responses | HIL-FN-09 | — | Pass | Pass |
| R-15 | Unknown or lower-case command → `ERR UNKNOWN` | HIL-RB-01 (3 cases) | `test_unknown_and_lowercase_commands_are_unknown` | Pass | Pass |
| R-16 | 64-char limit; 65 → `ERR TOOLONG`; parser recovers | HIL-RB-02 | `test_exactly_65_chars_is_toolong`, `test_exactly_64_chars_is_accepted` | Pass | Pass |
| R-17 | Input lines end with `\n`, `\r` or `\r\n`; one response each | HIL-RB-03 (3 cases) | `test_bare_cr_ends_a_line`, `test_crlf_gives_exactly_one_line` | Pass | Pass |
| R-18 | Output lines end with `\r\n` | — (transport strips `\r`) | every `test_logger` expectation | — | — |
| R-19 | Random binary input never wedges the parser | HIL-RB-04 | — | Pass | Pass ¹ |
| R-20 | A burst of 100 commands is answered completely, in order | HIL-RB-05 | — | Pass | Pass |
| R-21 | `RESET` → reboot, `reset=SOFTWARE`, defaults restored | HIL-RS-01 | `test_reset_requests_a_software_reset_once` | Pass | Pass |
| R-22 | Input after `RESET`/`FAULT HANG` is dropped | HIL-RS-03 | `test_input_after_reset_is_ignored`, `..._hang_...` | Pass | Pass ² |
| R-23 | 3 failed reads → `EVT SENSOR_LOST`, reported once | HIL-FT-01, FT-04 | `test_three_failures_mark_lost_once_*` | Pass | Pass |
| R-24 | `errors` counts every failed I²C transaction | HIL-FT-02 | `test_errors_counts_every_failed_transaction` | Pass | Pass |
| R-25 | Recovery on the 5 s retry → `EVT SENSOR_OK` | HIL-FT-03 | `test_lost_sensor_recovers_on_the_5s_retry` | Pass | Pass |
| R-26 | Commands are served while the sensor is LOST | HIL-FT-04 | same as R-23 | Pass | Pass |
| R-27 | Production build rejects `FAULT` (`ERR UNKNOWN`) | HIL-FT-05 | `test_fault_commands_are_unknown_without_hooks` | Pass | N/A ³ |
| R-28 | Stalled loop → watchdog reset in 3 s, `reset=WATCHDOG` | HIL-RS-02 | — | Pass | Pass |
| R-29 | `uptime_ms` restarts from 0 after a reset | HIL-RS-04 | — | Pass | xfail ⁴ |
| R-30 | `PING` round trip p95 < 50 ms on the simulator | HIL-PF-01 | — | Pass (p95 0.21 ms) | Reported (p95 299 ms) |

¹ After the fix for [bug 004](bugs/004-uart-rx-overflow-drops-bytes-in-bursts.md); failed before it.
² After the fix for [bug 003](bugs/003-commands-answered-after-reset-or-hang.md).
³ Wokwi runs the test build; the production check runs on `build/sim_prod`.
⁴ Wokwi limitation, [bug 002](bugs/002-uptime-not-reset-after-software-reset.md); real ESP32 restarts the clock.

Fault-injection coverage (design doc table): sensor disconnected → FT-01, FT-02, FT-04; sensor reconnected →
FT-03; firmware hang → RS-02; malformed input → RB-02, RB-03, RB-04; command flood → RB-05.

## 6. Defects

| Bug | Title | Found by | Severity | Status |
| --- | --- | --- | --- | --- |
| [001](bugs/001-serial-output-staircase.md) | Serial output drifts right in the Wokwi terminal | Manual Wokwi run | Low | Fixed |
| [002](bugs/002-uptime-not-reset-after-software-reset.md) | `uptime_ms` not restarted after `RESET` in Wokwi | Manual Wokwi run | Low | Wokwi limitation, xfail |
| [003](bugs/003-commands-answered-after-reset-or-hang.md) | Commands queued behind `RESET`/`FAULT HANG` still answered | Simulator bring-up | Medium | Fixed |
| [004](bugs/004-uart-rx-overflow-drops-bytes-in-bursts.md) | UART receive buffer overflow on bursts over ~1.5 KB | HIL-RB-04 on Wokwi | Medium | Fixed |
| [005](bugs/005-watchdog-reset-during-tls-handshake.md) | Cloud firmware: watchdog resets around connecting (TLS starvation; interrupt watchdog surviving `RESET`) | Wokwi runs, HIL-CT-07, RTC breadcrumbs | High | Fixed |
| [006](bugs/006-orphaned-alert-from-concurrent-function-instances.md) | Two Function instances race on a device's state; an alert is orphaned | Dashboard | Medium | Fixed |

## 7. Reports

- `reports/sim-report.html`, `reports/wokwi-report.html`: self-contained pytest-html reports with target,
  firmware version and commit in the header, and each test's serial conversation.
- `reports/latency-sim.json`, `reports/latency-wokwi.json`: `PING` round-trip p50 / p95 / max.
- CI uploads the simulator report as the `hil-sim-report` artifact on every push.
- `reports/wokwi-cloud-report.html`, `reports/cloud-latency.json`: the ColdTrace end-to-end run and its
  measured numbers.

## 8. ColdTrace cloud (v2.0.0)

Scope: the `esp32_cloud` firmware in Wokwi, Azure IoT Hub (F1), the Azure Function and Table Storage, driven
from the PC by `pytest --target wokwi --cloud -m cloud` (`hil/tests/test_cloud.py`, `hil/embedded_hil/cloud.py`).
Credentials come from the tester's `az login`; nothing secret is in the repository.

Levels: 46 Unity tests for store and forward and the twin reader (`test_publisher`, `test_logger_net`,
`test_twin`), 35 pytest tests for the cloud logic with an in-memory store (`azure/tests`), and the seven
end-to-end tests below. Final run 2026-11-17: **7 / 7 pass** in 10 min (device on the hub's main host name, RSA chain).

Rules: latency is measured with cloud clocks (IoT Hub enqueue → stored) or the PC's clock at both ends,
never the device's (Wokwi's clock drifts). Every cloud test starts from link up, no faults, temperature held
at 4 °C with `FAULT TEMP`, and the twin at its defaults (30 s, 8 °C, 300 s). Every cloud test fails if the
device reboots by watchdog or reports a loop stall of 1.5 s or more (bug 005 guard).

| Req | Requirement | Test | Unit tests | Result |
| --- | --- | --- | --- | --- |
| C-01 | Readings reach storage quickly | HIL-CT-01 | — | Pass: ingest p50 38 ms, p95 45 ms |
| C-02 | A 2-minute outage loses nothing; every seq stored once | HIL-CT-02 | `test_two_minute_outage_backlog_*`, `test_fault_net_*` | Pass: 21 / 21, no gaps |
| C-03 | Readings sent more than once are stored once and counted | HIL-CT-03 | `test_each_reading_is_stored_once_*`, `test_fault_noack_*` | Pass: 3 copies, 1 row |
| C-04 | Twin settings are validated, applied and reported back | HIL-CT-04 | `test_twin_*` (logger, parser) | Pass: ~7 s device, ~11 s cloud |
| C-05 | Above the limit past the window: exactly one alert, then clears | HIL-CT-05 | `test_excursion_*`, `test_two_instances_racing_*` | Pass |
| C-06 | Tampered, wrong-key and expired credentials are refused | HIL-CT-06 | — | Pass |
| C-07 | `RESET` comes back `SOFTWARE` and reconnects; a hang still reads `WATCHDOG` | HIL-CT-07 | — | Pass: 3 / 3 `SOFTWARE`, hang → `WATCHDOG` |
| C-08 | Overflow drops the oldest reading and leaves a visible seq gap | — | `test_overflow_drops_oldest_*` | Pass (unit) |
| C-09 | A device cannot write readings as another device | — | `test_parse_rejects_a_device_claiming_*` | Pass (unit) |

Observations recorded rather than asserted: a SAS token that expired 60 s earlier was still accepted
(IoT Hub allows some clock-skew grace); the twin round trip includes 2-4 s of `az` CLI start-up per call.
