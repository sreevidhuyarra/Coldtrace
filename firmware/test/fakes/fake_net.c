#include "fake_net.h"

#include <stdlib.h>
#include <string.h>

static bool fake_connected(void *ctx)
{
    return ((fake_net_state_t *)ctx)->up;
}

static int fake_publish(void *ctx, const char *payload, size_t len)
{
    fake_net_state_t *st = ctx;
    if (st->refuse || !st->up) {
        st->refused++;
        return -1;
    }
    unsigned i = st->published % FAKE_NET_MAX_MSGS;
    size_t n = len < PUB_PAYLOAD_MAX - 1u ? len : PUB_PAYLOAD_MAX - 1u;
    memcpy(st->payload[i], payload, n);
    st->payload[i][n] = '\0';
    st->msg_id[i] = st->next_msg_id++;
    st->published++;
    return st->msg_id[i];
}

net_t fake_net_init(fake_net_state_t *st)
{
    memset(st, 0, sizeof *st);
    st->up = true;
    st->next_msg_id = 100;
    net_t net = { fake_connected, fake_publish, st };
    return net;
}

const char *fake_net_payload(const fake_net_state_t *st, unsigned i)
{
    return st->payload[i % FAKE_NET_MAX_MSGS];
}

int fake_net_msg_id(const fake_net_state_t *st, unsigned i)
{
    return st->msg_id[i % FAKE_NET_MAX_MSGS];
}

unsigned long fake_net_seq(const fake_net_state_t *st, unsigned i)
{
    const char *p = strstr(fake_net_payload(st, i), "\"seq\":");
    return p ? strtoul(p + 6, NULL, 10) : 0ul;
}

int fake_net_last_id(const fake_net_state_t *st)
{
    return st->published ? fake_net_msg_id(st, st->published - 1u) : -1;
}
