/*
 * cloud.h - ColdTrace platform layer: Wi-Fi and MQTT/TLS to Azure IoT Hub.
 *
 * Only compiled into the esp32_cloud build (-DCOLDTRACE_CLOUD). It gives the
 * core a net_t (see core/publisher.h) backed by ESP-IDF's esp-mqtt client and
 * hands MQTT acknowledgements to the core from the main loop.
 */
#ifndef CLOUD_H
#define CLOUD_H

extern "C" {
#include "logger.h"
}

/* Call once in setup(), after logger_init(): starts Wi-Fi (non-blocking) and
 * attaches the network to the logger. */
void cloud_begin(logger_t *lg);

/* Call every loop pass, before logger_tick(): starts MQTT once Wi-Fi is up,
 * delivers acks and errors to the core, and carries out FAULT NET. */
void cloud_loop(logger_t *lg);

/* Crash breadcrumb: the step the loop task is in, kept in RTC memory so it
 * survives a watchdog reset. After one, cloud_begin() prints
 * "EVT WDT_STAGE <n>" so a hang can be traced to the call that blocked. */
extern volatile uint32_t g_cloud_stage;
#define CLOUD_STAGE(n) (g_cloud_stage = (n))

enum {
    STAGE_FEED = 1, STAGE_SERIAL = 2, STAGE_CLOUD = 3, STAGE_TICK = 4, STAGE_ACTION = 5, STAGE_IDLE = 6,
    STAGE_FAULT_WIFI_OFF = 31, STAGE_FAULT_WIFI_ON = 32, STAGE_MQTT_START = 33, STAGE_QUEUE = 34,
    STAGE_BOOT_ID = 35, STAGE_DATA = 36, STAGE_PUBLISH = 41,
};

#endif /* CLOUD_H */
