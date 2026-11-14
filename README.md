# ESP32 Temperature Logger Firmware

Firmware for an ESP32 that reads temperature from an MPU6050 over I²C, keeps the last 64 samples in a ring
buffer, and answers text commands over UART. It detects a failed sensor, keeps working, and recovers on its own
when the sensor comes back. A task watchdog reboots the chip if the main loop stalls, and every boot reports why
it restarted.

It runs entirely in the [Wokwi](https://wokwi.com) simulator, so no hardware is needed. The logic is portable
C99 with 57 unit tests that run on a PC in under a second.

![Firmware running in Wokwi](docs/img/wokwi-demo.png)

## Features

- **I²C driver** for the MPU6050 temperature sensor: probe (`WHO_AM_I`), wake, burst read, integer-only
  conversion to centi-degrees (within 0.01 °C of the datasheet formula over the full raw range)
- **UART command protocol** with strict framing and one response per command: see
  [docs/protocol.md](docs/protocol.md)
- **Ring buffer** of 64 samples that overwrites the oldest when full and counts what it lost
- **Sensor fault handling:** 1–2 failed reads tolerated, 3 in a row mark the sensor LOST, retry every 5 s,
  automatic recovery, no reboot
- **Task watchdog** (3 s) and reset-reason reporting (`POWERON`, `SOFTWARE`, `WATCHDOG`, `OTHER`)
- **Non-blocking main loop:** no `delay()`, wrap-safe timing (`now - last >= interval`)
- **Fault injection** (`FAULT I2C`, `FAULT HANG`) compiled only into the test build

## How it is built

```text
 UART text            src/main.cpp          (the only Arduino code)
     │                   │  Wire · Serial · millis() · watchdog · reset reason
     ▼                   ▼
 ┌────────────── hal_t: four function pointers ───────────────┐
 │                core/  — portable C99, no Arduino            │
 │   logger.c   sampling · state machine · command handlers   │
 │   cmd_parser.c   ring_buffer.c   mpu6050.c                  │
 └─────────────────────────────────────────────────────────────┘
     ▲
 test/fakes/fake_hal.c — fake I²C bus, fake clock, captured UART (unit tests)
```

The core reaches hardware only through a `hal_t` of function pointers. On the ESP32 they call Wire and Serial;
in the unit tests they are fakes, so the same code is tested on a PC with no board and no simulator. The core
never resets or hangs the chip itself: it *requests* that, and `main.cpp` acts on the request.

Design decisions worth knowing:

- **Integer centi-degrees** (`int16_t`, 2401 = 24.01 °C) instead of `float`: exact, cheap and easy to test.
- **Overwrite oldest when full:** a logger values the newest data, and sampling must never stop.
- **Fixed-size buffers, no `malloc`, no `String`** anywhere in the core.

## Repository layout

| Path | Contents |
| --- | --- |
| `firmware/core/` | Portable C99: HAL interface, ring buffer, command parser, MPU6050 driver, logger |
| `firmware/src/main.cpp` | Arduino platform layer: HAL implementation, watchdog, reset reason, main loop |
| `firmware/test/` | Unity unit tests and the fake HAL |
| `firmware/platformio.ini` | Builds `esp32` (production) and `esp32_test` (with fault hooks) |
| `firmware/diagram.json`, `wokwi.toml` | Wokwi circuit (ESP32 + MPU6050) and simulator config |
| `docs/protocol.md` | The serial protocol |
| `docs/spike-notes.md` | Day-1 checks of the simulator's sensor and watchdog models |
| `docs/bugs/` | Bugs found while testing in Wokwi |
| `spike/` | Throwaway sketches from those checks |
| `third_party/unity/` | Unity test framework (git submodule, v2.6.1) |

## Build and test

Prerequisites: VS Code with the PlatformIO and Wokwi extensions; for the unit tests, gcc and make (on Windows,
[MSYS2](https://www.msys2.org/) with `mingw-w64-ucrt-x86_64-gcc` and `make`).

```sh
git clone --recursive <repo-url>
cd esp32-logger

# unit tests on the PC (Windows: C:\msys64\usr\bin\make.exe test)
make test

# firmware, both builds
cd firmware
pio run
```

Both firmware builds compile with zero warnings (`-Wall -Wextra -Werror` on project code). The unit tests
build with `-Wall -Wextra -Werror -pedantic -Wconversion`.

## Run in Wokwi

1. Build once (`pio run` in `firmware/`).
2. Open `firmware/diagram.json` in VS Code, then F1 → **Wokwi: Start Simulator**.
3. The terminal shows `BOOT fw=1.0.0 reset=POWERON`. Type commands and press Enter:

```text
PING            → OK PONG
READ            → OK TEMP 24.01
RATE 500        → OK RATE 500
STREAM ON       → EVT S 50507 24.01 ...
DUMP 3          → OK DUMP 3, three S lines, END
FAULT I2C ON    → EVT SENSOR_LOST after about 3 s
FAULT I2C OFF   → EVT SENSOR_OK within 5 s
FAULT HANG      → watchdog reset, then BOOT ... reset=WATCHDOG
```

Click the MPU6050 to change its temperature, then `READ` again. The serial port is also on
`rfc2217://localhost:4000` for automated tests.

## Testing

| Suite | Tests | Covers |
| --- | --- | --- |
| `test_ring_buffer` | 7 | Order, wrap at 64, overwrite oldest, overflow count, clear |
| `test_cmd_parser` | 12 | Line endings, the 64/65-character boundary, recovery, argument splitting |
| `test_mpu6050` | 10 | Probe, wake, byte order, I²C failures, conversion at known points and over all 65 536 raw values |
| `test_logger` | 26 | Every protocol row, sampling schedule, `millis()` wrap, LOST/OK state machine, fault hooks |
| `test_logger_nohooks` | 2 | Fault commands absent from the production build |

To check that the tests catch real mistakes, 19 bugs were planted in the code one at a time (off-by-one limits,
a wrap-unsafe time comparison, swapped bytes, a counter that never resets, and others). Three got through at
first and led to stronger tests; in the end every one was caught by at least one test.

## Known Wokwi limitations

Both were found by testing in the simulator and are written up in `docs/`:

- A watchdog reset is reported as ESP-IDF's `INT_WDT` instead of `TASK_WDT`, because Wokwi's panic handler
  doesn't finish its normal restart. The firmware maps both to `reset=WATCHDOG`.
  ([spike notes](docs/spike-notes.md))
- `millis()` does not restart from 0 after a software reset (on real hardware it does), so `uptime_ms` keeps
  counting across `RESET`. ([bug 002](docs/bugs/002-uptime-not-reset-after-software-reset.md))

## Future work

- Bring-up on a real ESP32 board
- CRC or checksum on sample lines
- Persist samples to flash across resets
