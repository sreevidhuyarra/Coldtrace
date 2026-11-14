/*
 * fake_hal.h - a HAL for unit tests, with no hardware behind it.
 *
 *  - fake I2C bus: one 256-byte register map for the device at FAKE_I2C_ADDR,
 *    plus a switch that makes the next N transactions fail
 *  - fake clock: the test sets and advances time by hand, so timing tests
 *    never wait
 *  - fake UART: everything written is captured in a string for assertions
 */
#ifndef FAKE_HAL_H
#define FAKE_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal.h"

#define FAKE_I2C_ADDR 0x68u
#define FAKE_UART_CAP 4096u

typedef struct {
    uint8_t  regs[256];
    unsigned fail_next;      /* fail this many upcoming transactions */
    bool     fail_always;    /* fail every transaction while set */
    bool     fail_writes;    /* fail every write (reads still work) */
    unsigned reads;          /* transactions attempted */
    unsigned writes;
    uint8_t  last_write_reg;
    uint8_t  last_write_val;
    uint32_t now;            /* fake clock, ms */
    char     uart[FAKE_UART_CAP];
    size_t   uart_len;
} fake_hal_state_t;

/* Reset state and return a hal_t wired to it. */
hal_t fake_hal_init(fake_hal_state_t *st);

/* Load a raw temperature into TEMP_OUT_H/L (big-endian, like the chip). */
void  fake_hal_set_raw_temp(fake_hal_state_t *st, int16_t raw);

/* Forget captured UART output. */
void  fake_hal_uart_clear(fake_hal_state_t *st);

#endif /* FAKE_HAL_H */
