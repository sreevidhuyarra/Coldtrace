/* Logger core tests. Built with -DLOGGER_TEST_HOOKS (see Makefile). */
#include <string.h>

#include "unity.h"
#include "logger.h"
#include "fake_hal.h"

#define RAW_24_01  (-4260)    /* -> 24.01 */
#define RAW_85_00  16480      /* -> 85.00 */

static fake_hal_state_t st;
static hal_t hal;
static logger_t lg;

static void boot(uint32_t at_ms, reset_reason_t reason)
{
    hal = fake_hal_init(&st);
    fake_hal_set_raw_temp(&st, RAW_24_01);
    st.now = at_ms;
    logger_init(&lg, &hal, reason);
}

void setUp(void)
{
    boot(1000, RESET_POWERON);
    fake_hal_uart_clear(&st);
}

void tearDown(void) {}

/* Send one command line; return everything the logger printed in reply. */
static const char *send(const char *line)
{
    fake_hal_uart_clear(&st);
    for (; *line; line++) {
        logger_rx_byte(&lg, *line);
    }
    logger_rx_byte(&lg, '\n');
    return st.uart;
}

/* Move the fake clock to t and run one loop pass. */
static void tick_at(uint32_t t)
{
    st.now = t;
    logger_tick(&lg);
}

/* ---- boot ------------------------------------------------------------ */

static void test_boot_line_reports_version_and_reset_reason(void)
{
    boot(0, RESET_WATCHDOG);
    TEST_ASSERT_EQUAL_STRING("BOOT fw=1.0.0 reset=WATCHDOG\r\n", st.uart);
    TEST_ASSERT_EQUAL(SENSOR_OK, lg.sensor);
}

static void test_boot_without_sensor_reports_lost_and_keeps_running(void)
{
    hal = fake_hal_init(&st);
    st.fail_always = true;
    logger_init(&lg, &hal, RESET_POWERON);
    TEST_ASSERT_EQUAL_STRING("BOOT fw=1.0.0 reset=POWERON\r\nEVT SENSOR_LOST\r\n", st.uart);
    TEST_ASSERT_EQUAL_STRING("OK PONG\r\n", send("PING"));
}

/* ---- simple commands and framing -------------------------------------- */

static void test_ping_and_version(void)
{
    TEST_ASSERT_EQUAL_STRING("OK PONG\r\n", send("PING"));
    TEST_ASSERT_EQUAL_STRING("OK VERSION 1.0.0\r\n", send("VERSION"));
}

static void test_unknown_and_lowercase_commands_are_unknown(void)
{
    TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN\r\n", send("HELLO"));
    TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN\r\n", send("ping"));
}

static void test_too_long_line_answers_toolong_once(void)
{
    char line[101];
    memset(line, 'A', 100);
    line[100] = '\0';
    TEST_ASSERT_EQUAL_STRING("ERR TOOLONG\r\n", send(line));
    TEST_ASSERT_EQUAL_STRING("OK PONG\r\n", send("PING"));
}

static void test_unexpected_or_malformed_args_are_rejected(void)
{
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("PING now"));
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("RATE  500"));
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("STREAM"));
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("STREAM MAYBE"));
}

/* ---- RATE and sampling ---------------------------------------------- */

static void test_rate_boundaries(void)
{
    TEST_ASSERT_EQUAL_STRING("ERR RANGE\r\n", send("RATE 99"));
    TEST_ASSERT_EQUAL_STRING("OK RATE 100\r\n", send("RATE 100"));
    TEST_ASSERT_EQUAL_STRING("OK RATE 60000\r\n", send("RATE 60000"));
    TEST_ASSERT_EQUAL_STRING("ERR RANGE\r\n", send("RATE 60001"));
    TEST_ASSERT_EQUAL_STRING("ERR RANGE\r\n", send("RATE 99999999999"));
    TEST_ASSERT_EQUAL_UINT32(60000, lg.rate_ms);   /* rejected values change nothing */
}

static void test_rate_rejects_non_numbers(void)
{
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("RATE"));
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("RATE abc"));
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("RATE -5"));
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("RATE 1 2"));
}

static void test_samples_are_taken_every_rate_ms(void)
{
    tick_at(1999);
    TEST_ASSERT_EQUAL_UINT8(0, rb_count(&lg.rb));
    tick_at(2000);
    TEST_ASSERT_EQUAL_UINT8(1, rb_count(&lg.rb));
    tick_at(2999);
    TEST_ASSERT_EQUAL_UINT8(1, rb_count(&lg.rb));
    tick_at(3000);
    TEST_ASSERT_EQUAL_UINT8(2, rb_count(&lg.rb));

    send("RATE 500");
    tick_at(3499);
    TEST_ASSERT_EQUAL_UINT8(2, rb_count(&lg.rb));
    tick_at(3500);
    TEST_ASSERT_EQUAL_UINT8(3, rb_count(&lg.rb));
}

static void test_sampling_survives_millis_wraparound(void)
{
    boot(UINT32_MAX - 499u, RESET_POWERON);   /* 500 ms before the wrap */
    tick_at(UINT32_MAX - 399u);               /* 100 ms later, before the wrap:
                                                 "now >= last + rate" fails here */
    TEST_ASSERT_EQUAL_UINT8(0, rb_count(&lg.rb));
    tick_at(499);                             /* 999 ms later, after the wrap */
    TEST_ASSERT_EQUAL_UINT8(0, rb_count(&lg.rb));
    tick_at(500);                             /* exactly 1000 ms later */
    TEST_ASSERT_EQUAL_UINT8(1, rb_count(&lg.rb));
}

/* ---- READ, DUMP, CLEAR, STREAM, STATUS ------------------------------ */

static void test_read_returns_temperature_without_storing_it(void)
{
    TEST_ASSERT_EQUAL_STRING("OK TEMP 24.01\r\n", send("READ"));
    TEST_ASSERT_EQUAL_UINT8(0, rb_count(&lg.rb));
}

static void test_negative_temperatures_are_formatted_with_sign(void)
{
    fake_hal_set_raw_temp(&st, -12723);       /* -0.89: sign must not be lost */
    TEST_ASSERT_EQUAL_STRING("OK TEMP -0.89\r\n", send("READ"));
    fake_hal_set_raw_temp(&st, -15922);       /* value seen in the Wokwi spike */
    TEST_ASSERT_EQUAL_STRING("OK TEMP -10.29\r\n", send("READ"));
}

static void test_dump_lists_newest_n_samples_oldest_first(void)
{
    tick_at(2000);
    fake_hal_set_raw_temp(&st, RAW_85_00);
    tick_at(3000);

    TEST_ASSERT_EQUAL_STRING("OK DUMP 2\r\nS 2000 24.01\r\nS 3000 85.00\r\nEND\r\n", send("DUMP"));
    TEST_ASSERT_EQUAL_STRING("OK DUMP 1\r\nS 3000 85.00\r\nEND\r\n", send("DUMP 1"));
    TEST_ASSERT_EQUAL_STRING("OK DUMP 0\r\nEND\r\n", send("DUMP 0"));
    TEST_ASSERT_EQUAL_STRING("OK DUMP 2\r\nS 2000 24.01\r\nS 3000 85.00\r\nEND\r\n", send("DUMP 99"));
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("DUMP x"));
}

static void test_clear_empties_the_buffer(void)
{
    tick_at(2000);
    TEST_ASSERT_EQUAL_STRING("OK CLEAR\r\n", send("CLEAR"));
    TEST_ASSERT_EQUAL_STRING("OK DUMP 0\r\nEND\r\n", send("DUMP"));
}

static void test_stream_prints_each_sample_until_turned_off(void)
{
    TEST_ASSERT_EQUAL_STRING("OK STREAM ON\r\n", send("STREAM ON"));
    fake_hal_uart_clear(&st);
    tick_at(2000);
    TEST_ASSERT_EQUAL_STRING("EVT S 2000 24.01\r\n", st.uart);

    TEST_ASSERT_EQUAL_STRING("OK STREAM OFF\r\n", send("STREAM OFF"));
    fake_hal_uart_clear(&st);
    tick_at(3000);
    TEST_ASSERT_EQUAL_STRING("", st.uart);
    TEST_ASSERT_EQUAL_UINT8(2, rb_count(&lg.rb));  /* still sampling */
}

static void test_status_reports_every_field_in_fixed_order(void)
{
    tick_at(2000);
    TEST_ASSERT_EQUAL_STRING(
        "OK STATUS uptime_ms=2000 samples=1 rate_ms=1000 sensor=OK errors=0 overflows=0 reset=POWERON\r\n",
        send("STATUS"));
}

/* ---- sensor fault handling ------------------------------------------ */

static void test_two_failures_in_a_row_are_tolerated(void)
{
    st.fail_next = 2;
    tick_at(2000);
    tick_at(3000);
    tick_at(4000);                             /* this read succeeds */
    TEST_ASSERT_EQUAL(SENSOR_OK, lg.sensor);
    TEST_ASSERT_EQUAL_STRING("", st.uart);
    TEST_ASSERT_EQUAL_UINT8(1, rb_count(&lg.rb));

    /* the success reset the count: two more failures are still tolerated */
    st.fail_next = 2;
    tick_at(5000);
    tick_at(6000);
    TEST_ASSERT_EQUAL(SENSOR_OK, lg.sensor);
    TEST_ASSERT_EQUAL_STRING("", st.uart);
}

static void test_three_failures_mark_lost_once_and_commands_still_work(void)
{
    st.fail_always = true;
    tick_at(2000);
    tick_at(3000);
    TEST_ASSERT_EQUAL(SENSOR_OK, lg.sensor);
    tick_at(4000);
    TEST_ASSERT_EQUAL(SENSOR_LOST, lg.sensor);
    tick_at(5000);
    tick_at(6000);
    TEST_ASSERT_EQUAL_STRING("EVT SENSOR_LOST\r\n", st.uart);   /* exactly once */

    TEST_ASSERT_EQUAL_STRING("OK PONG\r\n", send("PING"));
    TEST_ASSERT_EQUAL_STRING("ERR SENSOR\r\n", send("READ"));
    TEST_ASSERT_NOT_NULL(strstr(send("STATUS"), "sensor=LOST errors=3 "));
}

static void test_lost_sensor_recovers_on_the_5s_retry(void)
{
    st.fail_always = true;
    tick_at(2000);
    tick_at(3000);
    tick_at(4000);                             /* LOST at 4000 */
    st.fail_always = false;
    fake_hal_uart_clear(&st);

    tick_at(8999);
    TEST_ASSERT_EQUAL(SENSOR_LOST, lg.sensor);
    tick_at(9000);
    TEST_ASSERT_EQUAL(SENSOR_OK, lg.sensor);
    TEST_ASSERT_EQUAL_STRING("EVT SENSOR_OK\r\n", st.uart);

    tick_at(10000);                            /* sampling resumes */
    TEST_ASSERT_EQUAL_UINT8(1, rb_count(&lg.rb));
}

static void test_failed_retry_stays_lost_and_tries_again_later(void)
{
    st.fail_always = true;
    tick_at(2000);
    tick_at(3000);
    tick_at(4000);
    fake_hal_uart_clear(&st);
    tick_at(9000);                             /* retry fails */
    TEST_ASSERT_EQUAL(SENSOR_LOST, lg.sensor);
    TEST_ASSERT_EQUAL_STRING("", st.uart);
    st.fail_always = false;
    tick_at(13999);
    TEST_ASSERT_EQUAL(SENSOR_LOST, lg.sensor);
    tick_at(14000);
    TEST_ASSERT_EQUAL(SENSOR_OK, lg.sensor);
}

static void test_errors_counts_every_failed_transaction(void)
{
    st.fail_next = 2;
    TEST_ASSERT_EQUAL_STRING("ERR SENSOR\r\n", send("READ"));
    TEST_ASSERT_EQUAL_STRING("ERR SENSOR\r\n", send("READ"));
    TEST_ASSERT_EQUAL_STRING("OK TEMP 24.01\r\n", send("READ"));
    TEST_ASSERT_EQUAL_UINT32(2, lg.errors);
}

/* ---- pending actions and test hooks --------------------------------- */

static void test_reset_requests_a_software_reset_once(void)
{
    TEST_ASSERT_EQUAL(ACT_NONE, logger_take_action(&lg));
    TEST_ASSERT_EQUAL_STRING("OK RESET\r\n", send("RESET"));
    TEST_ASSERT_EQUAL(ACT_RESET, logger_take_action(&lg));
    TEST_ASSERT_EQUAL(ACT_NONE, logger_take_action(&lg));
}

/* Bug 003: commands already in the receive buffer must not run once a
 * reset or hang is pending; "the next line after OK RESET is BOOT". */
static void test_input_after_reset_is_ignored(void)
{
    TEST_ASSERT_EQUAL_STRING("OK RESET\r\n", send("RESET"));
    TEST_ASSERT_EQUAL_STRING("", send("RATE 500"));
    TEST_ASSERT_EQUAL_UINT32(LOGGER_RATE_DEFAULT_MS, lg.rate_ms);
    TEST_ASSERT_EQUAL(ACT_RESET, logger_take_action(&lg));
}

static void test_input_after_hang_is_ignored(void)
{
    TEST_ASSERT_EQUAL_STRING("OK FAULT HANG\r\n", send("FAULT HANG"));
    TEST_ASSERT_EQUAL_STRING("", send("PING"));
    TEST_ASSERT_EQUAL(ACT_HANG, logger_take_action(&lg));
}

static void test_fault_i2c_loses_the_sensor_and_off_recovers_it(void)
{
    TEST_ASSERT_EQUAL_STRING("OK FAULT I2C ON\r\n", send("FAULT I2C ON"));
    fake_hal_uart_clear(&st);
    tick_at(2000);
    tick_at(3000);
    tick_at(4000);
    TEST_ASSERT_EQUAL_STRING("EVT SENSOR_LOST\r\n", st.uart);

    TEST_ASSERT_EQUAL_STRING("OK FAULT I2C OFF\r\n", send("FAULT I2C OFF"));
    fake_hal_uart_clear(&st);
    tick_at(9000);
    TEST_ASSERT_EQUAL_STRING("EVT SENSOR_OK\r\n", st.uart);
}

static void test_fault_i2c_never_touches_the_real_bus(void)
{
    unsigned before = st.reads;
    send("FAULT I2C ON");
    send("READ");
    TEST_ASSERT_EQUAL_UINT(before, st.reads);
    TEST_ASSERT_EQUAL_UINT32(1, lg.errors);   /* still counted as a failure */
}

static void test_fault_hang_requests_a_hang(void)
{
    TEST_ASSERT_EQUAL_STRING("OK FAULT HANG\r\n", send("FAULT HANG"));
    TEST_ASSERT_EQUAL(ACT_HANG, logger_take_action(&lg));
}

static void test_fault_rejects_bad_args(void)
{
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("FAULT"));
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("FAULT I2C"));
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("FAULT I2C MAYBE"));
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("FAULT DISK"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_boot_line_reports_version_and_reset_reason);
    RUN_TEST(test_boot_without_sensor_reports_lost_and_keeps_running);
    RUN_TEST(test_ping_and_version);
    RUN_TEST(test_unknown_and_lowercase_commands_are_unknown);
    RUN_TEST(test_too_long_line_answers_toolong_once);
    RUN_TEST(test_unexpected_or_malformed_args_are_rejected);
    RUN_TEST(test_rate_boundaries);
    RUN_TEST(test_rate_rejects_non_numbers);
    RUN_TEST(test_samples_are_taken_every_rate_ms);
    RUN_TEST(test_sampling_survives_millis_wraparound);
    RUN_TEST(test_read_returns_temperature_without_storing_it);
    RUN_TEST(test_negative_temperatures_are_formatted_with_sign);
    RUN_TEST(test_dump_lists_newest_n_samples_oldest_first);
    RUN_TEST(test_clear_empties_the_buffer);
    RUN_TEST(test_stream_prints_each_sample_until_turned_off);
    RUN_TEST(test_status_reports_every_field_in_fixed_order);
    RUN_TEST(test_two_failures_in_a_row_are_tolerated);
    RUN_TEST(test_three_failures_mark_lost_once_and_commands_still_work);
    RUN_TEST(test_lost_sensor_recovers_on_the_5s_retry);
    RUN_TEST(test_failed_retry_stays_lost_and_tries_again_later);
    RUN_TEST(test_errors_counts_every_failed_transaction);
    RUN_TEST(test_reset_requests_a_software_reset_once);
    RUN_TEST(test_input_after_reset_is_ignored);
    RUN_TEST(test_input_after_hang_is_ignored);
    RUN_TEST(test_fault_i2c_loses_the_sensor_and_off_recovers_it);
    RUN_TEST(test_fault_i2c_never_touches_the_real_bus);
    RUN_TEST(test_fault_hang_requests_a_hang);
    RUN_TEST(test_fault_rejects_bad_args);
    return UNITY_END();
}
