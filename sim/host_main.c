/*
 * host_main.c - host simulator for the logger firmware.
 *
 * Runs the SAME core C files as the ESP32 (firmware/core/), with a host
 * platform layer instead of main.cpp:
 *
 *   ESP32                      host simulator
 *   Serial at 115200     ->    stdin / stdout (binary)
 *   millis()             ->    CLOCK_MONOTONIC, restarted at every boot
 *   Wire + MPU6050       ->    fake MPU6050 register map, --temp sets it
 *   Task watchdog (3 s)  ->    emulated: no feed for 3 s -> reboot
 *   esp_restart()        ->    re-initialise the core
 *
 * `make sim` builds two binaries: build/sim with -DLOGGER_TEST_HOOKS
 * (FAULT commands, used by the harness) and build/sim_prod without them
 * (the production build, used to prove FAULT is rejected there).
 *
 * Input is read by a background thread so the main loop never blocks; this
 * works on Linux, macOS and Windows (MinGW), unlike select() on stdin.
 * The process exits when stdin reaches EOF and all input is handled, so
 * `echo PING | ./build/sim` prints the BOOT line and OK PONG, then exits.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "logger.h"
#include "mpu6050.h"

#define WDT_TIMEOUT_MS 3000u
#define RXQ_SIZE       65536u

/* ---- clock ----------------------------------------------------------------- */

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t g_boot_ms;   /* millis() restarts from 0 at every boot, like the chip */

/* ---- fake MPU6050 ------------------------------------------------------------ */

static uint8_t g_regs[256];

static void mpu_reset(double temp_c)
{
    memset(g_regs, 0, sizeof g_regs);
    g_regs[MPU6050_REG_WHO_AM_I] = MPU6050_WHO_AM_I_VALUE;
    g_regs[MPU6050_REG_PWR_MGMT_1] = 0x40;            /* SLEEP set at power-on */

    /* inverse of T = raw / 340 + 36.53, rounded to nearest */
    double raw_f = (temp_c - 36.53) * 340.0;
    long raw = (long)(raw_f < 0 ? raw_f - 0.5 : raw_f + 0.5);
    if (raw > INT16_MAX) raw = INT16_MAX;
    if (raw < INT16_MIN) raw = INT16_MIN;
    uint16_t u = (uint16_t)(int16_t)raw;
    g_regs[MPU6050_REG_TEMP_OUT_H] = (uint8_t)(u >> 8);
    g_regs[MPU6050_REG_TEMP_OUT_H + 1u] = (uint8_t)(u & 0xFFu);
}

/* ---- HAL ----------------------------------------------------------------- */

static int sim_i2c_read(void *ctx, uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    (void)ctx;
    if (addr != MPU6050_ADDR) {
        return -1;                                    /* nobody answers: NACK */
    }
    for (size_t i = 0; i < len; i++) {
        buf[i] = g_regs[(uint8_t)(reg + i)];
    }
    return 0;
}

static int sim_i2c_write(void *ctx, uint8_t addr, uint8_t reg, uint8_t val)
{
    (void)ctx;
    if (addr != MPU6050_ADDR) {
        return -1;
    }
    if (reg != MPU6050_REG_TEMP_OUT_H && reg != MPU6050_REG_TEMP_OUT_H + 1u) {
        g_regs[reg] = val;                            /* temperature is read-only */
    }
    return 0;
}

static uint32_t sim_now(void *ctx)
{
    (void)ctx;
    return (uint32_t)(mono_ms() - g_boot_ms);
}

static void sim_uart_write(void *ctx, const char *data, size_t len)
{
    (void)ctx;
    fwrite(data, 1, len, stdout);
    fflush(stdout);
}

static const hal_t g_hal = { sim_i2c_read, sim_i2c_write, sim_now, sim_uart_write, NULL };

/* ---- stdin reader thread -> byte queue ------------------------------------------- */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static unsigned char   g_rxq[RXQ_SIZE];
static size_t          g_rx_head, g_rx_len;
static bool            g_eof;

static void *reader_main(void *arg)
{
    (void)arg;
    unsigned char chunk[512];
    for (;;) {
        ssize_t n = read(STDIN_FILENO, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        pthread_mutex_lock(&g_lock);
        if (n <= 0) {
            g_eof = true;
            pthread_cond_signal(&g_cond);
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }
        for (ssize_t i = 0; i < n; i++) {
            while (g_rx_len == RXQ_SIZE) {            /* full: wait for the loop */
                pthread_cond_wait(&g_cond, &g_lock);
            }
            g_rxq[(g_rx_head + g_rx_len) % RXQ_SIZE] = chunk[i];
            g_rx_len++;
        }
        pthread_cond_signal(&g_cond);
        pthread_mutex_unlock(&g_lock);
    }
}

/* Copy up to max queued bytes out; wait up to 1 ms if there are none. */
static size_t rx_take(unsigned char *out, size_t max, bool *eof)
{
    pthread_mutex_lock(&g_lock);
    if (g_rx_len == 0 && !g_eof) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 1000000L;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&g_cond, &g_lock, &until);
    }
    size_t n = g_rx_len < max ? g_rx_len : max;
    for (size_t i = 0; i < n; i++) {
        out[i] = g_rxq[(g_rx_head + i) % RXQ_SIZE];
    }
    g_rx_head = (g_rx_head + n) % RXQ_SIZE;
    g_rx_len -= n;
    *eof = g_eof && g_rx_len == 0;
    pthread_cond_signal(&g_cond);                     /* room for the reader */
    pthread_mutex_unlock(&g_lock);
    return n;
}

/* Bytes that arrive while the chip is hung or rebooting are lost. */
static void rx_discard(void)
{
    pthread_mutex_lock(&g_lock);
    g_rx_head = 0;
    g_rx_len = 0;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

/* ---- main --------------------------------------------------------------------- */

static logger_t g_logger;

static void boot(reset_reason_t reason)
{
    g_boot_ms = mono_ms();
    logger_init(&g_logger, &g_hal, reason);
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [--temp DEGREES_C]\n", prog);
}

int main(int argc, char **argv)
{
    double temp_c = 24.0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--temp") == 0 && i + 1 < argc) {
            char *end;
            temp_c = strtod(argv[++i], &end);
            if (*end != '\0') {
                usage(argv[0]);
                return 2;
            }
        } else {
            usage(argv[0]);
            return 2;
        }
    }

#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);   /* no CRLF translation either way */
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    pthread_t reader;
    if (pthread_create(&reader, NULL, reader_main, NULL) != 0) {
        fprintf(stderr, "sim: cannot start input thread\n");
        return 1;
    }
    pthread_detach(reader);

    mpu_reset(temp_c);
    boot(RESET_POWERON);

    uint64_t last_feed = mono_ms();
    bool hung = false;

    for (;;) {
        if (!hung) {
            last_feed = mono_ms();                    /* 1. feed the watchdog */

            unsigned char buf[256];                   /* 2. commands */
            bool eof;
            size_t n = rx_take(buf, sizeof buf, &eof);
            for (size_t i = 0; i < n; i++) {
                logger_rx_byte(&g_logger, (char)buf[i]);
            }

            logger_tick(&g_logger);                   /* 3-4. sampling, retry */

            switch (logger_take_action(&g_logger)) {  /* 5. pending actions */
            case ACT_RESET:
                rx_discard();
                mpu_reset(temp_c);                    /* chip re-powers the sensor */
                boot(RESET_SOFTWARE);
                break;
            case ACT_HANG:
                hung = true;                          /* stop feeding */
                break;
            default:
                break;
            }

            if (eof && n == 0 && !hung) {
                break;
            }
        } else {
            struct timespec ms = { 0, 1000000L };
            nanosleep(&ms, NULL);
            if (mono_ms() - last_feed >= WDT_TIMEOUT_MS) {
                rx_discard();
                mpu_reset(temp_c);
                hung = false;
                boot(RESET_WATCHDOG);
            }
        }
    }
    return 0;
}
