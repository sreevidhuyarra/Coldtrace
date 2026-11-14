/*
 * hal.h - the only way the portable core reaches hardware.
 *
 * The platform layer (src/main.cpp) fills one hal_t with functions that call
 * Wire, Serial and millis(). Unit tests fill it with fakes instead, which is
 * what lets every module in core/ run and be tested on a PC.
 *
 * Plain C99, no Arduino headers allowed in core/.
 */
#ifndef HAL_H
#define HAL_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    /* Read len bytes starting at register reg of device addr (7-bit).
     * Returns 0 on success, non-zero on any bus error (NACK, timeout). */
    int (*i2c_read_regs)(void *ctx, uint8_t addr, uint8_t reg, uint8_t *buf, size_t len);

    /* Write one byte to register reg. Returns 0 on success, non-zero on error. */
    int (*i2c_write_reg)(void *ctx, uint8_t addr, uint8_t reg, uint8_t val);

    /* Milliseconds since boot. Wraps after ~49.7 days; callers use
     * unsigned subtraction (now - last >= interval) so the wrap is harmless. */
    uint32_t (*now_ms)(void *ctx);

    /* Write len bytes to the serial port. */
    void (*uart_write)(void *ctx, const char *data, size_t len);

    /* Passed back unchanged to every function above. */
    void *ctx;
} hal_t;

#endif /* HAL_H */
