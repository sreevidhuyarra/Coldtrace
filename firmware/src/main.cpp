/*
 * main.cpp - the platform layer. The only file that knows about Arduino.
 *
 * It does four things:
 *   1. fills a hal_t with Wire / Serial / millis() so the portable core can
 *      reach the hardware
 *   2. reads the reset reason once and maps it for the BOOT line
 *   3. arms the Task Watchdog (3 s, panic = reboot) and feeds it every pass
 *   4. runs the non-blocking loop and carries out RESET / HANG requests
 *
 * Everything else lives in core/ and is unit-tested on a PC.
 */
#include <Arduino.h>
#include <Wire.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

extern "C" {
#include "logger.h"
}

#ifdef COLDTRACE_CLOUD
#include "cloud.h"           /* Wi-Fi + MQTT to Azure IoT Hub (esp32_cloud build) */
#endif

#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

static const int      PIN_SDA     = 21;
static const int      PIN_SCL     = 22;
static const uint32_t I2C_HZ      = 100000;   /* standard mode */
static const uint32_t UART_BAUD   = 115200;
#ifdef COLDTRACE_CLOUD
/* The cloud build's watchdog must outlast the longest legitimate pause. In
 * Wokwi the TLS handshake (ESP-IDF's precompiled crypto, in the MQTT task)
 * can stall CPU 1 with interrupts blocked for over 3 s, so even a priority-10
 * loop cannot run (docs/bugs/005). The offline builds keep 3 s. */
static const uint32_t WDT_TIMEOUT_S = 10;
#else
static const uint32_t WDT_TIMEOUT_S = 3;
#endif
#ifdef COLDTRACE_CLOUD
#include <esp_timer.h>
static const UBaseType_t LOOP_TASK_PRIORITY = 10;  /* above esp-mqtt (1, default 5), below Wi-Fi/TCP-IP */
static const uint32_t    LOOP_GAP_REPORT_MS = 500;
#endif

/* ---- HAL implementation ------------------------------------------------ */

static int hal_i2c_read_regs(void *ctx, uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    (void)ctx;
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {          /* repeated start, no stop */
        return -1;
    }
    if (Wire.requestFrom((uint16_t)addr, len, true) != len) {
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)Wire.read();
    }
    return 0;
}

static int hal_i2c_write_reg(void *ctx, uint8_t addr, uint8_t reg, uint8_t val)
{
    (void)ctx;
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0 ? 0 : -1;
}

static uint32_t hal_now_ms(void *ctx)
{
    (void)ctx;
    return millis();
}

static void hal_uart_write(void *ctx, const char *data, size_t len)
{
    (void)ctx;
    Serial.write(reinterpret_cast<const uint8_t *>(data), len);
}

static const hal_t g_hal = {
    hal_i2c_read_regs,
    hal_i2c_write_reg,
    hal_now_ms,
    hal_uart_write,
    nullptr,
};

static logger_t g_logger;

/* ---- reset reason -------------------------------------------------------- */

static reset_reason_t map_reset_reason(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:
        return RESET_POWERON;
    case ESP_RST_SW:
        return RESET_SOFTWARE;
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:   /* Wokwi reports a task-watchdog reset as INT_WDT:
                               the panic handler's backup timer (TG1) does the
                               reset there. See docs/spike-notes.md. */
    case ESP_RST_WDT:
        return RESET_WATCHDOG;
    default:
        return RESET_OTHER;
    }
}

/* ---- watchdog ------------------------------------------------------------ */

static void watchdog_start(void)
{
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t cfg = {};
    cfg.timeout_ms = WDT_TIMEOUT_S * 1000u;
    cfg.idle_core_mask = 0;
    cfg.trigger_panic = true;
    esp_task_wdt_reconfigure(&cfg);
#else
    esp_task_wdt_init(WDT_TIMEOUT_S, true);   /* panic = reboot on timeout */
#endif
    esp_task_wdt_add(NULL);                   /* watch the loop task */
#ifdef COLDTRACE_CLOUD
    /* Arduino-ESP32 also subscribes core 0's idle task. With Wi-Fi, the MQTT
     * task verifies the TLS certificate chain on core 0 (~31 s simulated in
     * Wokwi), the idle task starves past 3 s and the panic reboots the chip.
     * The guarantee we make is about the loop task, which stays watched. */
    esp_task_wdt_delete(xTaskGetIdleTaskHandleForCPU(0));
#endif
}

/* ---- Arduino entry points ----------------------------------------------- */

void setup()
{
    /* 4 KB instead of the 256-byte default, so a burst from the host is not
     * dropped before loop() drains it (docs/bugs/004). Must precede begin(). */
    Serial.setRxBufferSize(4096);
    Serial.begin(UART_BAUD);
    Wire.begin(PIN_SDA, PIN_SCL, I2C_HZ);
    Wire.setTimeOut(50);                      /* a dead bus must not stall the loop */

    reset_reason_t reason = map_reset_reason(esp_reset_reason());
#ifdef COLDTRACE_CLOUD
    bool reset_chained = false;
    reason = cloud_check_requested_restart(reason, &reset_chained);
#endif
    watchdog_start();
    logger_init(&g_logger, &g_hal, reason);
#ifdef COLDTRACE_CLOUD
    /* The control loop outranks every task that can do seconds of CPU work:
     * the MQTT/TLS task (asked for 1; esp-mqtt's default is 5) and anything
     * else at the default priorities. Wi-Fi and TCP/IP (18-23) stay above it.
     * Equal or lower priority let a TLS handshake starve loop() past the 3 s
     * watchdog (docs/bugs/005). The loop sleeps 1 ms per pass, so it gives
     * the CPU back almost all the time. */
    vTaskPrioritySet(NULL, LOOP_TASK_PRIORITY);
    cloud_begin(&g_logger, reset_chained);
#endif
}

#ifdef COLDTRACE_CLOUD
/* Loop-gap monitor (bug 005): the time between loop passes, measured on the
 * hardware timer so a stalled scheduler cannot hide it. Normally ~1-2 ms; any
 * gap of 500 ms or more is printed with the stage the loop was stalled in,
 * long before the 3 s watchdog would fire. */
static void watch_loop_gap(void)
{
    static int64_t s_last_us;
    int64_t now = esp_timer_get_time();
    if (s_last_us != 0) {
        uint32_t gap_ms = (uint32_t)((now - s_last_us) / 1000);
        if (gap_ms >= LOOP_GAP_REPORT_MS) {
            Serial.printf("EVT GAP %lu stage=%lu\r\n", (unsigned long)gap_ms, (unsigned long)g_cloud_stage);
        }
    }
    s_last_us = now;
}
#else
#define CLOUD_STAGE(n) ((void)0)              /* crash breadcrumbs: cloud build only */
static void watch_loop_gap(void) {}
#endif

void loop()
{
    watch_loop_gap();                         /* before the stage changes: reports where it stalled */
    CLOUD_STAGE(STAGE_FEED);
    esp_task_wdt_reset();                     /* 1. feed the watchdog */

    CLOUD_STAGE(STAGE_SERIAL);
    while (Serial.available() > 0) {          /* 2. commands */
        logger_rx_byte(&g_logger, (char)Serial.read());
    }

#ifdef COLDTRACE_CLOUD
    CLOUD_STAGE(STAGE_CLOUD);
    cloud_loop(&g_logger);                    /* acks, link state, FAULT NET */
#endif
    CLOUD_STAGE(STAGE_TICK);
    logger_tick(&g_logger);                   /* 3-4. sampling, sensor retry, publish */

    CLOUD_STAGE(STAGE_ACTION);
    switch (logger_take_action(&g_logger)) {  /* 5. pending actions */
    case ACT_RESET:
        Serial.flush();                       /* let "OK RESET" go out first */
#ifdef COLDTRACE_CLOUD
        cloud_prepare_restart();              /* network down, watchdogs disarmed (bug 005) */
#endif
        esp_restart();
        break;
    case ACT_HANG:
        Serial.flush();
        for (;;) {
            delay(10);                        /* stuck: never feeds the watchdog */
        }
        break;
    default:
        break;
    }

#ifdef COLDTRACE_CLOUD
    /* Give the CPU away for 1 ms per pass. The loop outranks the MQTT task, so
     * a loop that never blocks would starve the TLS handshake until Azure
     * resets the connection (-0x0050). 1 ms is far below the 4 KB UART
     * buffer's ~350 ms of input at 115200 baud. */
    CLOUD_STAGE(STAGE_IDLE);
    delay(1);
#endif
}
