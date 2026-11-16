#include "publisher.h"

#include <stdio.h>
#include <string.h>

void pub_init(pub_t *p, const net_t *net, const char *dev_id, uint32_t boot_id)
{
    memset(p, 0, sizeof *p);
    p->net = net;
    rb_init(&p->outbox);
    if (dev_id != NULL) {
        size_t n = strlen(dev_id);
        if (n > sizeof p->dev_id - 1u) {
            n = sizeof p->dev_id - 1u;      /* cut an over-long id; memset left the NUL */
        }
        memcpy(p->dev_id, dev_id, n);
    }
    p->boot_id = boot_id;
    p->next_seq = 1;
    p->limit_centi = PUB_LIMIT_DEFAULT_CENTI;
    p->window_s = PUB_WINDOW_DEFAULT_S;
}

/* "-2.50" style: sign, whole, two decimals. */
static void centi_str(int32_t v, char *buf, size_t cap)
{
    const char *sign = "";
    if (v < 0) {
        sign = "-";
        v = -v;
    }
    snprintf(buf, cap, "%s%ld.%02ld", sign, (long)(v / 100), (long)(v % 100));
}

uint32_t pub_enqueue(pub_t *p, uint32_t t_ms, int16_t centi_c)
{
    sample_t s = { .t_ms = t_ms, .centi_c = centi_c, .seq = p->next_seq++ };
    rb_push(&p->outbox, s);   /* full: overwrites the oldest and counts it */
    return s.seq;
}

int pub_format(const pub_t *p, const sample_t *s, char *buf, size_t cap)
{
    char temp[24], limit[24];   /* room for any int32 value, so -O2 sees no truncation */
    centi_str(s->centi_c, temp, sizeof temp);
    centi_str(p->limit_centi, limit, sizeof limit);
    int n = snprintf(buf, cap,
                     "{\"dev\":\"%s\",\"boot\":\"%08lx\",\"seq\":%lu,\"t_ms\":%lu,\"temp\":%s,"
                     "\"limit\":%s,\"window_s\":%lu}",
                     p->dev_id, (unsigned long)p->boot_id, (unsigned long)s->seq,
                     (unsigned long)s->t_ms, temp, limit, (unsigned long)p->window_s);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

static bool link_ok(const pub_t *p)
{
    return p->net != NULL && !p->fault_net && p->net->connected(p->net->ctx);
}

void pub_tick(pub_t *p, uint32_t now_ms)
{
    p->link_up = link_ok(p);
    if (!p->link_up) {
        if (p->in_flight) {
            /* The ack can no longer arrive on this session. The broker may or
             * may not have the message; sending it again is the safe choice. */
            p->in_flight = false;
            p->resends++;
        }
        return;
    }

    if (p->in_flight) {
        if (now_ms - p->sent_at_ms < PUB_ACK_TIMEOUT_MS) {   /* wrap-safe */
            return;
        }
        p->in_flight = false;   /* timed out: send the oldest again */
        p->resends++;
    }

    if (p->refused && now_ms - p->refused_at_ms < PUB_RETRY_MS) {
        return;
    }

    sample_t s;
    if (!rb_get(&p->outbox, 0, &s)) {
        return;                 /* nothing queued */
    }

    char payload[PUB_PAYLOAD_MAX];
    int len = pub_format(p, &s, payload, sizeof payload);
    if (len < 0) {
        return;                 /* cannot happen with PUB_DEV_ID_MAX; never send a cut message */
    }
    int msg = p->net->publish(p->net->ctx, payload, (size_t)len);
    if (msg < 0) {
        p->refused = true;
        p->refused_at_ms = now_ms;
        return;
    }
    p->refused = false;
    p->in_flight = true;
    p->in_flight_seq = s.seq;
    p->in_flight_msg = msg;
    p->sent_at_ms = now_ms;
}

uint32_t pub_on_ack(pub_t *p, int msg_id)
{
    if (p->fault_noack || !p->in_flight || msg_id != p->in_flight_msg) {
        return 0;
    }
    p->in_flight = false;
    p->acked++;

    /* Drop the reading only if it is still the oldest. While it was in flight
     * the queue may have overflowed and overwritten it; then the oldest is a
     * newer reading that has not been sent yet and must stay. */
    sample_t oldest;
    if (rb_get(&p->outbox, 0, &oldest) && oldest.seq == p->in_flight_seq) {
        rb_drop_oldest(&p->outbox);
    }
    return p->in_flight_seq;
}

bool pub_link_up(const pub_t *p)
{
    return p->link_up;
}

uint8_t pub_queued(const pub_t *p)
{
    return rb_count(&p->outbox);
}

uint32_t pub_dropped(const pub_t *p)
{
    return rb_overflows(&p->outbox);
}
