#include "ring_buffer.h"

void rb_init(ring_buffer_t *rb)
{
    rb->head = 0;
    rb->count = 0;
    rb->overflows = 0;
}

void rb_push(ring_buffer_t *rb, sample_t s)
{
    rb->buf[rb->head] = s;
    rb->head = (uint8_t)((rb->head + 1u) % RB_CAPACITY);
    if (rb->count < RB_CAPACITY) {
        rb->count++;
    } else {
        rb->overflows++;   /* the slot just written held the oldest sample */
    }
}

uint8_t rb_count(const ring_buffer_t *rb)
{
    return rb->count;
}

bool rb_get(const ring_buffer_t *rb, uint8_t i, sample_t *out)
{
    if (i >= rb->count) {
        return false;
    }
    /* the oldest sample sits count slots behind head; adding the capacity
     * keeps the sum non-negative before the modulo */
    uint8_t idx = (uint8_t)((rb->head + RB_CAPACITY - rb->count + i) % RB_CAPACITY);
    *out = rb->buf[idx];
    return true;
}

void rb_clear(ring_buffer_t *rb)
{
    rb->head = 0;
    rb->count = 0;
}

uint32_t rb_overflows(const ring_buffer_t *rb)
{
    return rb->overflows;
}
