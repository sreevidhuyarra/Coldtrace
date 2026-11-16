/*
 * logger.h - the temperature logger: sampling schedule, sensor state
 * machine, serial command handlers, streaming and events.
 *
 * Portable C99. Reaches hardware only through the HAL, and never resets or
 * hangs the chip itself: it *requests* that through a pending action, which
 * the platform layer (main.cpp) carries out. That keeps it testable on a PC.
 *
 * The platform layer calls, from its loop:
 *   logger_rx_byte()      for every byte received on the UART
 *   logger_tick()         once per loop pass
 *   logger_take_action()  and acts on ACT_RESET / ACT_HANG
 */
#ifndef LOGGER_H
#define LOGGER_H

#include <stdbool.h>
#include <stdint.h>

#include "cmd_parser.h"
#include "hal.h"
#include "publisher.h"
#include "ring_buffer.h"

#define LOGGER_FW_VERSION        "1.0.0"
#define LOGGER_RATE_DEFAULT_MS   1000u
#define LOGGER_RATE_MIN_MS       100u
#define LOGGER_RATE_MAX_MS       60000u
#define LOGGER_FAIL_THRESHOLD    3u     /* consecutive failed reads -> LOST */
#define LOGGER_RETRY_MS          5000u  /* re-initialise interval while LOST */
#define LOGGER_REPORT_DEFAULT_MS 30000u /* one cloud reading per 30 s: ~2,900 messages/day */
#define LOGGER_REPORT_MIN_MS     1000u
#define LOGGER_REPORT_MAX_MS     3600000u

typedef enum { SENSOR_INIT, SENSOR_OK, SENSOR_LOST } sensor_state_t;

typedef enum { ACT_NONE, ACT_RESET, ACT_HANG } pending_action_t;

typedef enum {
    RESET_POWERON,
    RESET_SOFTWARE,
    RESET_WATCHDOG,
    RESET_OTHER
} reset_reason_t;

typedef struct {
    const hal_t     *hal;          /* the real (or fake) hardware */
    hal_t            bus;          /* wrapper the driver uses: counts errors, injects faults */

    ring_buffer_t    rb;
    cmd_parser_t     parser;

    uint32_t         rate_ms;
    uint32_t         last_sample_ms;
    uint32_t         last_retry_ms;
    uint32_t         errors;       /* failed I2C transactions since boot */
    uint8_t          consecutive_fails;
    sensor_state_t   sensor;
    bool             streaming;
    bool             fault_i2c;    /* only settable in LOGGER_TEST_HOOKS builds */
    pending_action_t pending;
    reset_reason_t   reset_reason;

    /* Cloud reporting (ColdTrace). Inactive until logger_attach_net(). */
    pub_t            pub;          /* store-and-forward queue */
    bool             net_attached;
    bool             net_was_up;   /* for EVT NET_UP / NET_DOWN */
    uint32_t         report_ms;    /* one sample in this interval becomes a cloud reading */
    uint32_t         last_report_ms;
    bool             reported;     /* false until the first reading is queued */
} logger_t;

/* Print the BOOT line and bring the sensor up (OK, or LOST with an event). */
void             logger_init(logger_t *lg, const hal_t *hal, reset_reason_t reason);

/* Feed one received UART byte; a completed line runs its command.
 * Ignored while a reset or hang is pending (see logger_take_action). */
void             logger_rx_byte(logger_t *lg, char c);

/* Take a sample when due; retry the sensor while LOST. Never blocks. */
void             logger_tick(logger_t *lg);

/* Return the pending action and clear it. */
pending_action_t logger_take_action(logger_t *lg);

/* Turn on cloud reporting: every report_ms one sample is queued for the
 * cloud and sent through net (see publisher.h). Call after logger_init. */
void             logger_attach_net(logger_t *lg, const net_t *net, const char *dev_id, uint32_t boot_id);

/* The broker acknowledged message msg_id (prints EVT ACK <seq>). Call from
 * the main loop, not from the MQTT client's task. */
void             logger_on_ack(logger_t *lg, int msg_id);

/* Format centi-degrees as "24.01" / "-0.89". buf must hold 12 bytes. */
void             logger_format_temp(int16_t centi, char *buf);

const char      *logger_reset_name(reset_reason_t r);

#endif /* LOGGER_H */
