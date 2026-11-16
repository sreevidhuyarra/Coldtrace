/*
 * ring_buffer.h - fixed-size store for temperature samples.
 *
 * Holds the newest RB_CAPACITY samples. When full, a push overwrites the
 * oldest sample and increments the overflow counter: for a logger the newest
 * data is the most valuable, and a full buffer must never stop sampling.
 */
#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdbool.h>
#include <stdint.h>

#define RB_CAPACITY 64u

typedef struct {
    uint32_t t_ms;     /* uptime when sampled */
    int16_t  centi_c;  /* temperature x 100, e.g. 2401 = 24.01 C */
    uint32_t seq;      /* cloud sequence number; 0 = not a cloud reading */
} sample_t;

typedef struct {
    sample_t buf[RB_CAPACITY];
    uint8_t  head;       /* index of the next slot to write */
    uint8_t  count;      /* samples stored, 0..RB_CAPACITY */
    uint32_t overflows;  /* samples lost to overwrite since boot */
} ring_buffer_t;

void     rb_init(ring_buffer_t *rb);
void     rb_push(ring_buffer_t *rb, sample_t s);
uint8_t  rb_count(const ring_buffer_t *rb);
/* i = 0 is the oldest stored sample. Returns false if i >= count. */
bool     rb_get(const ring_buffer_t *rb, uint8_t i, sample_t *out);
/* Empties the buffer. The overflow counter is a since-boot statistic
 * and is deliberately kept. */
void     rb_clear(ring_buffer_t *rb);
/* Removes the oldest sample (the publish queue's "delivered" step).
 * Returns false if the buffer was empty. Does not count as an overflow. */
bool     rb_drop_oldest(ring_buffer_t *rb);
uint32_t rb_overflows(const ring_buffer_t *rb);

#endif /* RING_BUFFER_H */
