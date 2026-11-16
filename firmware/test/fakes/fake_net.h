/*
 * fake_net.h - a network for unit tests: no sockets, no broker.
 *
 * The test decides whether the link is up, whether the transport accepts a
 * message, and when (or whether) an ack arrives. Every published payload is
 * recorded so tests can check exactly what would have gone to the cloud.
 */
#ifndef FAKE_NET_H
#define FAKE_NET_H

#include <stdbool.h>
#include <stddef.h>

#include "publisher.h"

#define FAKE_NET_MAX_MSGS 256u

typedef struct {
    bool     up;                    /* connected() returns this */
    bool     refuse;                /* publish() returns -1 while set */
    int      next_msg_id;           /* id the next accepted publish gets */
    unsigned published;             /* accepted publishes */
    unsigned refused;
    char     payload[FAKE_NET_MAX_MSGS][PUB_PAYLOAD_MAX];
    int      msg_id[FAKE_NET_MAX_MSGS];
} fake_net_state_t;

/* Reset state (link up, ids from 100) and return a net_t wired to it. */
net_t        fake_net_init(fake_net_state_t *st);

/* Payload / message id of the i-th accepted publish (0 = first). */
const char  *fake_net_payload(const fake_net_state_t *st, unsigned i);
int          fake_net_msg_id(const fake_net_state_t *st, unsigned i);

/* The seq field of the i-th payload, or 0 if it has none. */
unsigned long fake_net_seq(const fake_net_state_t *st, unsigned i);

/* Id of the most recent accepted publish (what an ack for it would carry). */
int          fake_net_last_id(const fake_net_state_t *st);

#endif /* FAKE_NET_H */
