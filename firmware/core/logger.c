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
    if (lg->fault_temp) {               /* FAULT TEMP: a simulated temperature, no bus traffic */
        *centi = lg->fault_temp_centi;
        return true;
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

/* ---- cloud reporting (ColdTrace) --------------------------------------- */

static const char *link_name(const logger_t *lg)
{
    if (!lg->net_attached) {
        return "NONE";
    }
    return pub_link_up(&lg->pub) ? "UP" : "DOWN";
}

static void cmd_net(logger_t *lg)
{
    outf(lg, "OK NET link=%s queued=%u acked=%lu resends=%lu dropped=%lu next_seq=%lu report_ms=%lu boot=%08lx",
         link_name(lg),
         (unsigned)pub_queued(&lg->pub),
         (unsigned long)lg->pub.acked,
         (unsigned long)lg->pub.resends,
         (unsigned long)pub_dropped(&lg->pub),
         (unsigned long)lg->pub.next_seq,
         (unsigned long)lg->report_ms,
         (unsigned long)lg->pub.boot_id);
}

static void cmd_report(logger_t *lg, const cmd_t *c)
{
    uint32_t ms;
    if (c->argc != 1 || !parse_u32(c->args[0], &ms)) {
        outf(lg, "ERR ARGS");
        return;
    }
    if (ms < LOGGER_REPORT_MIN_MS || ms > LOGGER_REPORT_MAX_MS) {
        outf(lg, "ERR RANGE");
        return;
    }
    lg->report_ms = ms;
    outf(lg, "OK REPORT %lu", (unsigned long)ms);
}

/* Read now and queue the reading for the cloud; *seq is its number.
 * False if the sensor could not be read. */
static bool queue_reading_now(logger_t *lg, uint32_t *seq)
{
    int16_t centi;
    if (!sensor_read(lg, &centi)) {
        return false;
    }
    *seq = pub_enqueue(&lg->pub, now_ms(lg), centi);
    return true;
}

/* SEND: read now and queue the reading for the cloud straight away. */
static void cmd_send(logger_t *lg)
{
    uint32_t seq;
    if (!lg->net_attached) {
        outf(lg, "ERR NONET");
    } else if (!queue_reading_now(lg, &seq)) {
        outf(lg, "ERR SENSOR");
    } else {
        outf(lg, "OK SEND seq=%lu", (unsigned long)seq);
    }
}

static void cmd_cfg(logger_t *lg)
{
    char limit[12];
    logger_format_temp((int16_t)lg->pub.limit_centi, limit);
    outf(lg, "OK CFG report_ms=%lu limit_c=%s window_s=%lu twin_version=%lu",
         (unsigned long)lg->report_ms, limit, (unsigned long)lg->pub.window_s,
         (unsigned long)lg->twin_version);
}

#ifdef LOGGER_TEST_HOOKS
/* "ON" / "OFF" into *flag; false (and ERR ARGS) for anything else. */
static bool parse_on_off(logger_t *lg, const char *arg, bool *flag)
{
    if (strcmp(arg, "ON") == 0) {
        *flag = true;
    } else if (strcmp(arg, "OFF") == 0) {
        *flag = false;
    } else {
        outf(lg, "ERR ARGS");
        return false;
    }
    return true;
}

static void cmd_fault(logger_t *lg, const cmd_t *c)
{
    bool *flag = NULL;
    if (c->argc == 2 && strcmp(c->args[0], "I2C") == 0) {
        flag = &lg->fault_i2c;
    } else if (c->argc == 2 && strcmp(c->args[0], "NET") == 0) {
        flag = &lg->pub.fault_net;       /* link treated as down; platform also drops it */
    } else if (c->argc == 2 && strcmp(c->args[0], "NOACK") == 0) {
        flag = &lg->pub.fault_noack;     /* acks ignored, so readings are sent again */
    }

    if (flag != NULL) {
        if (parse_on_off(lg, c->args[1], flag)) {
            outf(lg, "OK FAULT %s %s", c->args[0], *flag ? "ON" : "OFF");
        }
    } else if (c->argc == 2 && strcmp(c->args[0], "TEMP") == 0) {
        /* FAULT TEMP <C> | OFF: the sensor reads this temperature (excursion tests) */
        int32_t centi;
        const char *a = c->args[1];
        if (strcmp(a, "OFF") == 0) {
            lg->fault_temp = false;
            outf(lg, "OK FAULT TEMP OFF");
        } else if (twin_parse_centi(a, a + strlen(a), &centi) &&
                   centi >= LOGGER_LIMIT_MIN_CENTI && centi <= LOGGER_LIMIT_MAX_CENTI) {
            char t[12];
            lg->fault_temp = true;
            lg->fault_temp_centi = (int16_t)centi;
            logger_format_temp(lg->fault_temp_centi, t);
            outf(lg, "OK FAULT TEMP %s", t);
        } else {
            outf(lg, "ERR ARGS");
        }
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
    } else if (is(&c, "NET")) {
        if (no_args(lg, &c)) cmd_net(lg);
    } else if (is(&c, "REPORT")) {
        cmd_report(lg, &c);
    } else if (is(&c, "SEND")) {
        if (no_args(lg, &c)) cmd_send(lg);
    } else if (is(&c, "CFG")) {
        if (no_args(lg, &c)) cmd_cfg(lg);
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
    pub_init(&lg->pub, NULL, "", 0);   /* no network until logger_attach_net() */
    lg->report_ms = LOGGER_REPORT_DEFAULT_MS;

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
                sample_t s = { .t_ms = now, .centi_c = centi, .seq = 0 };
                rb_push(&lg->rb, s);
                if (lg->streaming) {
                    char t[12];
                    logger_format_temp(centi, t);
                    outf(lg, "EVT S %lu %s", (unsigned long)now, t);
                }
                /* the first good sample, then one per report_ms, goes to the cloud */
                if (lg->net_attached && (!lg->reported || now - lg->last_report_ms >= lg->report_ms)) {
                    pub_enqueue(&lg->pub, now, centi);
                    lg->last_report_ms = now;
                    lg->reported = true;
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

    if (lg->net_attached) {
        pub_tick(&lg->pub, now);
        bool up = pub_link_up(&lg->pub);
        if (up != lg->net_was_up) {
            lg->net_was_up = up;
            outf(lg, "EVT %s", up ? "NET_UP" : "NET_DOWN");   /* literal format: Ubuntu's gcc has -Wformat-security on */
        }
    }
}

void logger_attach_net(logger_t *lg, const net_t *net, const char *dev_id, uint32_t boot_id)
{
    pub_init(&lg->pub, net, dev_id, boot_id);
    lg->net_attached = (net != NULL);
    lg->net_was_up = false;
    lg->reported = false;
}

void logger_on_ack(logger_t *lg, int msg_id)
{
    uint32_t seq = pub_on_ack(&lg->pub, msg_id);
    if (seq != 0) {
        outf(lg, "EVT ACK %lu", (unsigned long)seq);
    }
}

/* Append ",name" (or "name" first) to the rejected list. */
static void reject(char *list, size_t cap, const char *name)
{
    size_t n = strlen(list);
    snprintf(list + n, cap - n, "%s%s", n ? "," : "", name);
}

int logger_apply_twin(logger_t *lg, const twin_desired_t *d, char *reported, size_t cap)
{
    char rejected[48] = "";

    if (d->report_ms_state == TWIN_OK && d->report_ms >= LOGGER_REPORT_MIN_MS &&
        d->report_ms <= LOGGER_REPORT_MAX_MS) {
        lg->report_ms = d->report_ms;
    } else if (d->report_ms_state != TWIN_ABSENT) {
        reject(rejected, sizeof rejected, "report_ms");
    }

    if (d->limit_state == TWIN_OK && d->limit_centi >= LOGGER_LIMIT_MIN_CENTI &&
        d->limit_centi <= LOGGER_LIMIT_MAX_CENTI) {
        lg->pub.limit_centi = d->limit_centi;
    } else if (d->limit_state != TWIN_ABSENT) {
        reject(rejected, sizeof rejected, "limit_c");
    }

    if (d->window_state == TWIN_OK && d->window_s >= LOGGER_WINDOW_MIN_S &&
        d->window_s <= LOGGER_WINDOW_MAX_S) {
        lg->pub.window_s = d->window_s;
    } else if (d->window_state != TWIN_ABSENT) {
        reject(rejected, sizeof rejected, "window_s");
    }

    if (d->has_version) {
        lg->twin_version = d->version;
    }
    outf(lg, "EVT TWIN %lu%s%s", (unsigned long)lg->twin_version,
         rejected[0] ? " rejected=" : "", rejected);

    char limit[12];
    logger_format_temp((int16_t)lg->pub.limit_centi, limit);
    int n = snprintf(reported, cap,
                     "{\"report_ms\":%lu,\"limit_c\":%s,\"window_s\":%lu,\"applied_version\":%lu,"
                     "\"rejected\":\"%s\"}",
                     (unsigned long)lg->report_ms, limit, (unsigned long)lg->pub.window_s,
                     (unsigned long)lg->twin_version, rejected);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

int logger_flush(logger_t *lg, char *reply, size_t cap)
{
    uint32_t seq;
    if (!queue_reading_now(lg, &seq)) {
        snprintf(reply, cap, "{\"error\":\"sensor\"}");
        return 500;
    }
    snprintf(reply, cap, "{\"seq\":%lu,\"queued\":%u}", (unsigned long)seq,
             (unsigned)pub_queued(&lg->pub));
    return 200;
}

pending_action_t logger_take_action(logger_t *lg)
{
    pending_action_t a = lg->pending;
    lg->pending = ACT_NONE;
    return a;
}
