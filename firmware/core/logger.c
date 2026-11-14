#include "logger.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mpu6050.h"

#define OUT_MAX 160u

/* ------------------------------------------------------------------------
 * Bus wrapper
 *
 * The driver talks to lg->bus, not to the real HAL. Every I2C call passes
 * through here, which gives one place to (a) count failed transactions for
 * STATUS and (b) inject the FAULT I2C test fault.
 * ---------------------------------------------------------------------- */

static int bus_read(void *ctx, uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    logger_t *lg = ctx;
    int rc = lg->fault_i2c ? -1
           : lg->hal->i2c_read_regs(lg->hal->ctx, addr, reg, buf, len);
    if (rc != 0) {
        lg->errors++;
    }
    return rc;
}

static int bus_write(void *ctx, uint8_t addr, uint8_t reg, uint8_t val)
{
    logger_t *lg = ctx;
    int rc = lg->fault_i2c ? -1
           : lg->hal->i2c_write_reg(lg->hal->ctx, addr, reg, val);
    if (rc != 0) {
        lg->errors++;
    }
    return rc;
}

static uint32_t bus_now(void *ctx)
{
    logger_t *lg = ctx;
    return lg->hal->now_ms(lg->hal->ctx);
}

static void bus_uart(void *ctx, const char *data, size_t len)
{
    logger_t *lg = ctx;
    lg->hal->uart_write(lg->hal->ctx, data, len);
}

/* ------------------------------------------------------------------------
 * Output helpers
 * ---------------------------------------------------------------------- */

static uint32_t now_ms(const logger_t *lg)
{
    return lg->hal->now_ms(lg->hal->ctx);
}

/* printf-style, one protocol line; the "\r\n" ending is added here.
 * "\r\n" rather than "\n" so terminals (including Wokwi's in VS Code)
 * return to column 0. See docs/bugs/001. */
static void outf(logger_t *lg, const char *fmt, ...)
{
    char line[OUT_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 2u, fmt, ap);   /* room for "\r\n" */
    va_end(ap);
    if (n < 0) {
        return;
    }
    size_t len = (size_t)n < sizeof line - 3u ? (size_t)n : sizeof line - 3u;
    line[len++] = '\r';
    line[len++] = '\n';
    lg->hal->uart_write(lg->hal->ctx, line, len);
}

void logger_format_temp(int16_t centi, char *buf)
{
    int32_t v = centi;
    const char *sign = "";
    if (v < 0) {
        sign = "-";
        v = -v;
    }
    snprintf(buf, 12, "%s%ld.%02ld", sign, (long)(v / 100), (long)(v % 100));
}

const char *logger_reset_name(reset_reason_t r)
{
    switch (r) {
    case RESET_POWERON:  return "POWERON";
    case RESET_SOFTWARE: return "SOFTWARE";
    case RESET_WATCHDOG: return "WATCHDOG";
    default:             return "OTHER";
    }
}

static const char *sensor_name(sensor_state_t s)
{
    switch (s) {
    case SENSOR_OK:   return "OK";
    case SENSOR_LOST: return "LOST";
    default:          return "INIT";
    }
}

/* ------------------------------------------------------------------------
 * Sensor state machine:  INIT -> OK | LOST,  OK -> LOST,  LOST -> OK
 * ---------------------------------------------------------------------- */

static bool sensor_try_init(logger_t *lg)
{
    if (mpu6050_init(&lg->bus) != MPU_OK) {
        return false;
    }
    lg->sensor = SENSOR_OK;
    lg->consecutive_fails = 0;
    lg->last_sample_ms = now_ms(lg);   /* first sample one period from now */
    return true;
}

static void sensor_mark_lost(logger_t *lg)
{
    lg->sensor = SENSOR_LOST;
    lg->last_retry_ms = now_ms(lg);
    outf(lg, "EVT SENSOR_LOST");
}

/* One read through the state machine. A single failure is tolerated;
 * LOGGER_FAIL_THRESHOLD in a row mark the sensor LOST. */
static bool sensor_read(logger_t *lg, int16_t *centi)
{
    if (lg->sensor != SENSOR_OK) {
        return false;
    }
    if (mpu6050_read_centi(&lg->bus, centi) == MPU_OK) {
        lg->consecutive_fails = 0;
        return true;
    }
    if (++lg->consecutive_fails >= LOGGER_FAIL_THRESHOLD) {
        sensor_mark_lost(lg);
    }
    return false;
}

/* ------------------------------------------------------------------------
 * Command handlers
 * ---------------------------------------------------------------------- */

/* Unsigned decimal, digits only. Values too big for uint32 saturate, so
 * "RATE 99999999999" is a range error rather than an argument error. */
static bool parse_u32(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    if (*s == '\0') {
        return false;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        uint32_t d = (uint32_t)(*s - '0');
        v = (v > (UINT32_MAX - d) / 10u) ? UINT32_MAX : v * 10u + d;
    }
    *out = v;
    return true;
}

static void cmd_read(logger_t *lg)
{
    int16_t centi;
    char t[12];
    if (!sensor_read(lg, &centi)) {
        outf(lg, "ERR SENSOR");
        return;
    }
    logger_format_temp(centi, t);
    outf(lg, "OK TEMP %s", t);
}

static void cmd_status(logger_t *lg)
{
    outf(lg, "OK STATUS uptime_ms=%lu samples=%u rate_ms=%lu sensor=%s errors=%lu overflows=%lu reset=%s",
         (unsigned long)now_ms(lg),
         (unsigned)rb_count(&lg->rb),
         (unsigned long)lg->rate_ms,
         sensor_name(lg->sensor),
         (unsigned long)lg->errors,
         (unsigned long)rb_overflows(&lg->rb),
         logger_reset_name(lg->reset_reason));
}

static void cmd_rate(logger_t *lg, const cmd_t *c)
{
    uint32_t ms;
    if (c->argc != 1 || !parse_u32(c->args[0], &ms)) {
        outf(lg, "ERR ARGS");
        return;
    }
    if (ms < LOGGER_RATE_MIN_MS || ms > LOGGER_RATE_MAX_MS) {
        outf(lg, "ERR RANGE");
        return;
    }
    lg->rate_ms = ms;
    outf(lg, "OK RATE %lu", (unsigned long)ms);
}

/* DUMP [n]: the newest n samples (all if n is omitted), oldest first. */
static void cmd_dump(logger_t *lg, const cmd_t *c)
{
    uint8_t count = rb_count(&lg->rb);
    uint32_t n = count;
    if (c->argc > 1 || (c->argc == 1 && !parse_u32(c->args[0], &n))) {
        outf(lg, "ERR ARGS");
        return;
    }
    uint8_t k = (n < count) ? (uint8_t)n : count;
    outf(lg, "OK DUMP %u", (unsigned)k);
    for (uint8_t i = (uint8_t)(count - k); i < count; i++) {
        sample_t s;
        char t[12];
        rb_get(&lg->rb, i, &s);
        logger_format_temp(s.centi_c, t);
        outf(lg, "S %lu %s", (unsigned long)s.t_ms, t);
    }
    outf(lg, "END");
}

static void cmd_stream(logger_t *lg, const cmd_t *c)
{
    if (c->argc == 1 && strcmp(c->args[0], "ON") == 0) {
        lg->streaming = true;
    } else if (c->argc == 1 && strcmp(c->args[0], "OFF") == 0) {
        lg->streaming = false;
    } else {
        outf(lg, "ERR ARGS");
        return;
    }
    outf(lg, "OK STREAM %s", lg->streaming ? "ON" : "OFF");
}

#ifdef LOGGER_TEST_HOOKS
static void cmd_fault(logger_t *lg, const cmd_t *c)
{
    if (c->argc == 2 && strcmp(c->args[0], "I2C") == 0) {
        if (strcmp(c->args[1], "ON") == 0) {
            lg->fault_i2c = true;
        } else if (strcmp(c->args[1], "OFF") == 0) {
            lg->fault_i2c = false;
        } else {
            outf(lg, "ERR ARGS");
            return;
        }
        outf(lg, "OK FAULT I2C %s", lg->fault_i2c ? "ON" : "OFF");
    } else if (c->argc == 1 && strcmp(c->args[0], "HANG") == 0) {
        outf(lg, "OK FAULT HANG");
        lg->pending = ACT_HANG;
    } else {
        outf(lg, "ERR ARGS");
    }
}
#endif

static bool is(const cmd_t *c, const char *name)
{
    return strcmp(c->name, name) == 0;
}

/* Commands that take no arguments answer ERR ARGS if given any. */
static bool no_args(logger_t *lg, const cmd_t *c)
{
    if (c->argc != 0) {
        outf(lg, "ERR ARGS");
        return false;
    }
    return true;
}

static void run_line(logger_t *lg, char *line)
{
    cmd_t c;
    if (!cmd_split(line, &c)) {
        outf(lg, "ERR ARGS");
        return;
    }

    if (is(&c, "PING")) {
        if (no_args(lg, &c)) outf(lg, "OK PONG");
    } else if (is(&c, "VERSION")) {
        if (no_args(lg, &c)) outf(lg, "OK VERSION %s", LOGGER_FW_VERSION);
    } else if (is(&c, "STATUS")) {
        if (no_args(lg, &c)) cmd_status(lg);
    } else if (is(&c, "READ")) {
        if (no_args(lg, &c)) cmd_read(lg);
    } else if (is(&c, "RATE")) {
        cmd_rate(lg, &c);
    } else if (is(&c, "DUMP")) {
        cmd_dump(lg, &c);
    } else if (is(&c, "CLEAR")) {
        if (no_args(lg, &c)) {
            rb_clear(&lg->rb);
            outf(lg, "OK CLEAR");
        }
    } else if (is(&c, "STREAM")) {
        cmd_stream(lg, &c);
    } else if (is(&c, "RESET")) {
        if (no_args(lg, &c)) {
            outf(lg, "OK RESET");
            lg->pending = ACT_RESET;
        }
#ifdef LOGGER_TEST_HOOKS
    } else if (is(&c, "FAULT")) {
        cmd_fault(lg, &c);
#endif
    } else {
        outf(lg, "ERR UNKNOWN");
    }
}

/* ------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void logger_init(logger_t *lg, const hal_t *hal, reset_reason_t reason)
{
    memset(lg, 0, sizeof *lg);
    lg->hal = hal;
    lg->bus.i2c_read_regs = bus_read;
    lg->bus.i2c_write_reg = bus_write;
    lg->bus.now_ms = bus_now;
    lg->bus.uart_write = bus_uart;
    lg->bus.ctx = lg;

    rb_init(&lg->rb);
    cmd_parser_init(&lg->parser);
    lg->rate_ms = LOGGER_RATE_DEFAULT_MS;
    lg->sensor = SENSOR_INIT;
    lg->pending = ACT_NONE;
    lg->reset_reason = reason;
    lg->last_sample_ms = now_ms(lg);

    outf(lg, "BOOT fw=%s reset=%s", LOGGER_FW_VERSION, logger_reset_name(reason));

    if (!sensor_try_init(lg)) {
        sensor_mark_lost(lg);          /* keep running; retry every 5 s */
    }
}

void logger_rx_byte(logger_t *lg, char c)
{
    /* A reset or hang is about to happen: input that arrives meanwhile is
     * lost, exactly as it would be on a real reboot. Without this, commands
     * already in the receive buffer ran before the reboot (docs/bugs/003). */
    if (lg->pending != ACT_NONE) {
        return;
    }

    switch (cmd_parser_feed(&lg->parser, c)) {
    case CMD_LINE:
        run_line(lg, cmd_parser_line(&lg->parser));
        break;
    case CMD_TOOLONG:
        outf(lg, "ERR TOOLONG");
        break;
    default:
        break;
    }
}

void logger_tick(logger_t *lg)
{
    uint32_t now = now_ms(lg);

    if (lg->sensor == SENSOR_OK) {
        if (now - lg->last_sample_ms >= lg->rate_ms) {   /* wrap-safe */
            int16_t centi;
            lg->last_sample_ms = now;
            if (sensor_read(lg, &centi)) {
                sample_t s = { now, centi };
                rb_push(&lg->rb, s);
                if (lg->streaming) {
                    char t[12];
                    logger_format_temp(centi, t);
                    outf(lg, "EVT S %lu %s", (unsigned long)now, t);
                }
            }
        }
    } else if (lg->sensor == SENSOR_LOST) {
        if (now - lg->last_retry_ms >= LOGGER_RETRY_MS) {
            lg->last_retry_ms = now;
            if (sensor_try_init(lg)) {
                outf(lg, "EVT SENSOR_OK");
            }
        }
    }
}

pending_action_t logger_take_action(logger_t *lg)
{
    pending_action_t a = lg->pending;
    lg->pending = ACT_NONE;
    return a;
}
