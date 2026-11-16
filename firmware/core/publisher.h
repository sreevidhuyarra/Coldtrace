/*
 * publisher.h - store and forward: the queue of readings bound for the cloud.
 *
 * Readings are numbered (seq 1, 2, 3 ... per boot) and kept in a 64-slot ring
 * buffer until the broker acknowledges them (MQTT QoS 1 PUBACK). If the link
 * is down they simply wait; when it returns, the backlog is sent oldest first.
 * If the queue overflows, the oldest readings are dropped and counted, and the
 * cloud sees the gap in sequence numbers.
 *
 * One message is in flight at a time ("stop and wait"): send the oldest
 * reading, wait for the ack carrying that message id, then drop it from the
 * queue. No ack within PUB_ACK_TIMEOUT_MS, or the link drops first, and the
 * same reading is sent again. So delivery is at least once: the cloud may see
 * a reading twice and de-duplicates on (device, boot, seq).
 *
 * seq restarts at 1 on every boot, so each boot also gets a random boot id,
 * and the cloud's key is (device, boot, seq).
 *
 * Portable C99, single-threaded: the platform layer calls pub_on_ack() from
 * its main loop, never from the MQTT client's own task.
 */
#ifndef PUBLISHER_H
#define PUBLISHER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ring_buffer.h"

#define PUB_ACK_TIMEOUT_MS  20000u  /* resend if no PUBACK in this time (Wokwi round trip ~0.3 s) */
#define PUB_RETRY_MS        1000u   /* wait after the transport refused a message */
#define PUB_PAYLOAD_MAX     160u    /* bytes incl. NUL; IoT Hub meters in 512-byte blocks */
#define PUB_DEV_ID_MAX      32u     /* incl. NUL */
#define PUB_LIMIT_DEFAULT_CENTI  800 /* 8.00 C: cold-chain upper limit */
#define PUB_WINDOW_DEFAULT_S     300u /* above the limit this long -> alert */

/* What the publisher needs from the network. The platform layer fills it in
 * (esp-mqtt on the ESP32, a fake in the unit tests). */
typedef struct {
    /* True while the MQTT session is up and messages can be handed over. */
    bool (*connected)(void *ctx);

    /* Hand one telemetry message to the transport for QoS 1 delivery.
     * Returns the MQTT message id that the later ack will carry (>= 0),
     * or < 0 if the transport could not take it. */
    int  (*publish)(void *ctx, const char *payload, size_t len);

    void *ctx;
} net_t;

typedef struct {
    const net_t  *net;            /* NULL = no network (plain logger build) */
    ring_buffer_t outbox;         /* readings not yet acknowledged, oldest first */
    char          dev_id[PUB_DEV_ID_MAX];
    uint32_t      boot_id;
    uint32_t      next_seq;       /* seq the next enqueued reading gets; starts at 1 */
    int32_t       limit_centi;    /* alarm settings sent with every reading (device twin) */
    uint32_t      window_s;

    bool          in_flight;      /* a message is out and its ack is awaited */
    uint32_t      in_flight_seq;
    int           in_flight_msg;  /* MQTT message id of that message */
    uint32_t      sent_at_ms;

    bool          refused;        /* the transport refused the last attempt */
    uint32_t      refused_at_ms;

    bool          link_up;        /* as of the last pub_tick() */
    uint32_t      acked;          /* readings acknowledged since boot */
    uint32_t      resends;        /* messages sent again (timeout or link loss) */

    bool          fault_net;      /* test hook: treat the link as down */
    bool          fault_noack;    /* test hook: ignore every ack, forcing resends */
} pub_t;

void     pub_init(pub_t *p, const net_t *net, const char *dev_id, uint32_t boot_id);

/* Queue a reading; returns the seq it was given. */
uint32_t pub_enqueue(pub_t *p, uint32_t t_ms, int16_t centi_c);

/* Send the oldest reading if the link is up and nothing is in flight; resend
 * after a timeout. Call once per loop pass. Never blocks. */
void     pub_tick(pub_t *p, uint32_t now_ms);

/* The broker acknowledged message msg_id. Returns the seq it delivered, or 0
 * if the ack was not for the message in flight (or acks are being ignored). */
uint32_t pub_on_ack(pub_t *p, int msg_id);

bool     pub_link_up(const pub_t *p);
uint8_t  pub_queued(const pub_t *p);
uint32_t pub_dropped(const pub_t *p);   /* lost to queue overflow since boot */

/* {"dev":"coldtrace-01","boot":"1a2b3c4d","seq":12,"t_ms":123456,"temp":24.01,"limit":8.00,"window_s":300}
 * limit/window_s are the settings in force when the reading is sent; the
 * cloud's excursion check uses them, so a twin change applies end to end.
 * Returns the length written (excluding NUL), or -1 if cap is too small. */
int      pub_format(const pub_t *p, const sample_t *s, char *buf, size_t cap);

#endif /* PUBLISHER_H */
