#include "fake_hal.h"

#include <string.h>

static bool should_fail(fake_hal_state_t *st)
{
    if (st->fail_always) {
        return true;
    }
    if (st->fail_next > 0) {
        st->fail_next--;
        return true;
    }
    return false;
}

static int fake_read(void *ctx, uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    fake_hal_state_t *st = ctx;
    st->reads++;
    if (addr != FAKE_I2C_ADDR || should_fail(st)) {
        return -1;                 /* NACK */
    }
    for (size_t i = 0; i < len; i++) {
        buf[i] = st->regs[(uint8_t)(reg + i)];   /* auto-increment, like the chip */
    }
    return 0;
}

static int fake_write(void *ctx, uint8_t addr, uint8_t reg, uint8_t val)
{
    fake_hal_state_t *st = ctx;
    st->writes++;
    if (addr != FAKE_I2C_ADDR || st->fail_writes || should_fail(st)) {
        return -1;
    }
    st->regs[reg] = val;
    st->last_write_reg = reg;
    st->last_write_val = val;
    return 0;
}

static uint32_t fake_now(void *ctx)
{
    return ((fake_hal_state_t *)ctx)->now;
}

static void fake_uart_write(void *ctx, const char *data, size_t len)
{
    fake_hal_state_t *st = ctx;
    size_t room = FAKE_UART_CAP - 1u - st->uart_len;
    if (len > room) {
        len = room;
    }
    memcpy(&st->uart[st->uart_len], data, len);
    st->uart_len += len;
    st->uart[st->uart_len] = '\0';
}

hal_t fake_hal_init(fake_hal_state_t *st)
{
    memset(st, 0, sizeof *st);
    st->regs[0x75] = 0x68;          /* WHO_AM_I of a real MPU6050 */
    st->regs[0x6B] = 0x40;          /* PWR_MGMT_1 reset value: SLEEP set */
    hal_t hal = { fake_read, fake_write, fake_now, fake_uart_write, st };
    return hal;
}

void fake_hal_set_raw_temp(fake_hal_state_t *st, int16_t raw)
{
    uint16_t u = (uint16_t)raw;
    st->regs[0x41] = (uint8_t)(u >> 8);
    st->regs[0x42] = (uint8_t)(u & 0xFFu);
}

void fake_hal_uart_clear(fake_hal_state_t *st)
{
    st->uart_len = 0;
    st->uart[0] = '\0';
}
