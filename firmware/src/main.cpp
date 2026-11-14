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

#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

static const int      PIN_SDA     = 21;
static const int      PIN_SCL     = 22;
static const uint32_t I2C_HZ      = 100000;   /* standard mode */
static const uint32_t UART_BAUD   = 115200;
static const uint32_t WDT_TIMEOUT_S = 3;

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
    watchdog_start();
    logger_init(&g_logger, &g_hal, reason);
}

void loop()
{
    esp_task_wdt_reset();                     /* 1. feed the watchdog */

    while (Serial.available() > 0) {          /* 2. commands */
        logger_rx_byte(&g_logger, (char)Serial.read());
    }

    logger_tick(&g_logger);                   /* 3-4. sampling, sensor retry */

    switch (logger_take_action(&g_logger)) {  /* 5. pending actions */
    case ACT_RESET:
        Serial.flush();                       /* let "OK RESET" go out first */
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
}
