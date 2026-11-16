/* Logger + publisher together: cloud reporting through the serial protocol.
 * Built with -DLOGGER_TEST_HOOKS (see Makefile). */
#include <string.h>

#include "unity.h"
#include "logger.h"
#include "fake_hal.h"
#include "fake_net.h"

#define RAW_24_01 (-4260)

static fake_hal_state_t st;
static hal_t hal;
static fake_net_state_t ns;
static net_t net;
static logger_t lg;

void setUp(void)
{
    hal = fake_hal_init(&st);
    fake_hal_set_raw_temp(&st, RAW_24_01);
    st.now = 0;
    logger_init(&lg, &hal, RESET_POWERON);
    net = fake_net_init(&ns);
    logger_attach_net(&lg, &net, "coldtrace-01", 0xdeadbeefu);
    fake_hal_uart_clear(&st);
}

void tearDown(void) {}

static const char *send(const char *line)
{
    fake_hal_uart_clear(&st);
    for (; *line; line++) {
        logger_rx_byte(&lg, *line);
    }
    logger_rx_byte(&lg, '\n');
    return st.uart;
}

static void tick_at(uint32_t t)
{
    st.now = t;
    logger_tick(&lg);
}

/* Run the loop every 100 ms from the current time up to t. */
static void run_until(uint32_t t)
{
    for (uint32_t x = st.now + 100u; x <= t; x += 100u) {
        tick_at(x);
    }
}

/* ---- without a network --------------------------------------------------- */

static void test_without_network_net_reports_none_and_send_is_refused(void)
{
    logger_init(&lg, &hal, RESET_POWERON);       /* fresh logger, never attached */
    TEST_ASSERT_EQUAL_STRING(
        "OK NET link=NONE queued=0 acked=0 resends=0 dropped=0 next_seq=1 report_ms=30000 boot=00000000\r\n",
        send("NET"));
    TEST_ASSERT_EQUAL_STRING("ERR NONET\r\n", send("SEND"));
    run_until(5000);
    TEST_ASSERT_EQUAL(0, ns.published);
}

/* ---- reporting schedule --------------------------------------------------------- */

static void test_first_sample_is_reported_then_one_per_report_interval(void)
{
    run_until(1000);                              /* first sample at 1 s: reported */
    TEST_ASSERT_EQUAL(1, ns.published);
    TEST_ASSERT_NOT_NULL(strstr(fake_net_payload(&ns, 0),
        "\"dev\":\"coldtrace-01\",\"boot\":\"deadbeef\",\"seq\":1,\"t_ms\":1000,\"temp\":24.01"));
    logger_on_ack(&lg, fake_net_last_id(&ns));

    run_until(30900);                             /* samples every 1 s, none reported */
    TEST_ASSERT_EQUAL(1, ns.published);
    run_until(31000);                             /* 30 s after the first report */
    TEST_ASSERT_EQUAL(2, ns.published);
    TEST_ASSERT_EQUAL_UINT32(2, fake_net_seq(&ns, 1));
}

static void test_report_interval_is_settable_within_range(void)
{
    TEST_ASSERT_EQUAL_STRING("ERR RANGE\r\n", send("REPORT 999"));
    TEST_ASSERT_EQUAL_STRING("OK REPORT 1000\r\n", send("REPORT 1000"));
    TEST_ASSERT_EQUAL_STRING("OK REPORT 3600000\r\n", send("REPORT 3600000"));
    TEST_ASSERT_EQUAL_STRING("ERR RANGE\r\n", send("REPORT 3600001"));
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("REPORT"));
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("REPORT 5s"));
    TEST_ASSERT_EQUAL_UINT32(3600000, lg.report_ms);
}

static void test_faster_report_interval_reports_every_sample(void)
{
    send("REPORT 1000");
    for (uint32_t t = 1000; t <= 5000; t += 1000) {
        tick_at(t);
        logger_on_ack(&lg, fake_net_last_id(&ns));
    }
    TEST_ASSERT_EQUAL(5, ns.published);
    TEST_ASSERT_EQUAL_UINT32(5, fake_net_seq(&ns, 4));
}

static void test_lost_sensor_reports_nothing(void)
{
    st.fail_always = true;
    run_until(40000);
    TEST_ASSERT_EQUAL(0, ns.published);
}

/* ---- commands and events -------------------------------------------------------- */

static void test_send_reads_now_and_publishes_on_the_next_pass(void)
{
    TEST_ASSERT_EQUAL_STRING("OK SEND seq=1\r\n", send("SEND"));
    TEST_ASSERT_EQUAL_STRING("OK SEND seq=2\r\n", send("SEND"));
    tick_at(10);
    TEST_ASSERT_EQUAL(1, ns.published);
    TEST_ASSERT_EQUAL_UINT32(1, fake_net_seq(&ns, 0));
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("SEND 3"));
}

static void test_send_with_a_failed_read_answers_err_sensor(void)
{
    st.fail_next = 1;
    TEST_ASSERT_EQUAL_STRING("ERR SENSOR\r\n", send("SEND"));
    TEST_ASSERT_EQUAL_UINT32(1, lg.pub.next_seq);         /* no seq used up */
}

static void test_ack_prints_evt_ack_with_the_seq(void)
{
    send("SEND");
    tick_at(10);
    fake_hal_uart_clear(&st);
    logger_on_ack(&lg, fake_net_last_id(&ns));
    TEST_ASSERT_EQUAL_STRING("EVT ACK 1\r\n", st.uart);

    fake_hal_uart_clear(&st);
    logger_on_ack(&lg, 9999);                              /* not ours: silent */
    TEST_ASSERT_EQUAL_STRING("", st.uart);
}

static void test_link_changes_print_net_up_and_net_down_once(void)
{
    tick_at(10);
    TEST_ASSERT_EQUAL_STRING("EVT NET_UP\r\n", st.uart);
    fake_hal_uart_clear(&st);
    tick_at(20);
    TEST_ASSERT_EQUAL_STRING("", st.uart);                 /* no repeat */

    ns.up = false;
    tick_at(30);
    TEST_ASSERT_EQUAL_STRING("EVT NET_DOWN\r\n", st.uart);
}

static void test_net_reports_queue_and_counters(void)
{
    tick_at(10);
    send("SEND");
    send("SEND");
    tick_at(20);                                           /* seq 1 in flight */
    logger_on_ack(&lg, fake_net_last_id(&ns));
    TEST_ASSERT_EQUAL_STRING(
        "OK NET link=UP queued=1 acked=1 resends=0 dropped=0 next_seq=3 report_ms=30000 boot=deadbeef\r\n",
        send("NET"));
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("NET X"));
}

/* ---- fault hooks ------------------------------------------------------------------ */

static void test_fault_net_takes_the_link_down_and_off_sends_the_backlog(void)
{
    tick_at(10);
    TEST_ASSERT_EQUAL_STRING("OK FAULT NET ON\r\n", send("FAULT NET ON"));
    send("REPORT 1000");
    fake_hal_uart_clear(&st);
    run_until(5010);                                       /* samples at 1010, 2010 ... 5010 */
    TEST_ASSERT_NOT_NULL(strstr(st.uart, "EVT NET_DOWN"));
    TEST_ASSERT_EQUAL(0, ns.published);
    TEST_ASSERT_EQUAL(5, pub_queued(&lg.pub));

    TEST_ASSERT_EQUAL_STRING("OK FAULT NET OFF\r\n", send("FAULT NET OFF"));
    for (unsigned i = 0; i < 10 && pub_queued(&lg.pub) > 0; i++) {
        tick_at(st.now + 1u);
        logger_on_ack(&lg, fake_net_last_id(&ns));
    }
    TEST_ASSERT_EQUAL(5, ns.published);
    TEST_ASSERT_EQUAL_UINT32(1, fake_net_seq(&ns, 0));
    TEST_ASSERT_EQUAL_UINT32(5, fake_net_seq(&ns, 4));
}

static void test_fault_noack_makes_the_device_send_a_reading_twice(void)
{
    TEST_ASSERT_EQUAL_STRING("OK FAULT NOACK ON\r\n", send("FAULT NOACK ON"));
    send("SEND");
    tick_at(10);
    logger_on_ack(&lg, fake_net_last_id(&ns));
    tick_at(10 + PUB_ACK_TIMEOUT_MS);
    TEST_ASSERT_EQUAL(2, ns.published);
    TEST_ASSERT_EQUAL_UINT32(fake_net_seq(&ns, 0), fake_net_seq(&ns, 1));
    TEST_ASSERT_EQUAL_STRING("OK FAULT NOACK OFF\r\n", send("FAULT NOACK OFF"));
}

static void test_fault_net_and_noack_reject_bad_args(void)
{
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("FAULT NET"));
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("FAULT NET MAYBE"));
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("FAULT NOACK on"));
    TEST_ASSERT_FALSE(lg.pub.fault_net);
    TEST_ASSERT_FALSE(lg.pub.fault_noack);
}

static void test_existing_commands_are_unchanged_by_cloud_reporting(void)
{
    TEST_ASSERT_EQUAL_STRING("OK PONG\r\n", send("PING"));
    TEST_ASSERT_EQUAL_STRING("OK VERSION 2.0.0\r\n", send("VERSION"));
    run_until(3000);
    TEST_ASSERT_EQUAL_STRING("OK DUMP 1\r\nS 3000 24.01\r\nEND\r\n", send("DUMP 1"));
}

/* ---- device twin, flush, CFG, FAULT TEMP ---------------------------------------- */

static int apply(const char *json, char *rep)
{
    twin_desired_t d;
    twin_parse(json, strlen(json), &d);
    return logger_apply_twin(&lg, &d, rep, LOGGER_REPORTED_MAX);
}

static void test_cfg_shows_defaults(void)
{
    TEST_ASSERT_EQUAL_STRING("OK CFG report_ms=30000 limit_c=8.00 window_s=300 twin_version=0\r\n",
                             send("CFG"));
}

static void test_twin_applies_settings_and_reports_them(void)
{
    char rep[LOGGER_REPORTED_MAX];
    int n = apply("{\"report_ms\":5000,\"limit_c\":6.5,\"window_s\":120,\"$version\":3}", rep);
    TEST_ASSERT_EQUAL_STRING(
        "{\"report_ms\":5000,\"limit_c\":6.50,\"window_s\":120,\"applied_version\":3,\"rejected\":\"\"}", rep);
    TEST_ASSERT_EQUAL((int)strlen(rep), n);
    TEST_ASSERT_EQUAL_STRING("EVT TWIN 3\r\n", st.uart);
    TEST_ASSERT_EQUAL_STRING("OK CFG report_ms=5000 limit_c=6.50 window_s=120 twin_version=3\r\n",
                             send("CFG"));
}

static void test_twin_settings_reach_the_cloud_payload(void)
{
    char rep[LOGGER_REPORTED_MAX];
    apply("{\"limit_c\":-2,\"window_s\":60,\"$version\":5}", rep);
    send("SEND");
    tick_at(10);
    TEST_ASSERT_NOT_NULL(strstr(fake_net_payload(&ns, 0), "\"limit\":-2.00,\"window_s\":60}"));
}

static void test_twin_rejects_bad_values_and_keeps_the_old_ones(void)
{
    char rep[LOGGER_REPORTED_MAX];
    apply("{\"report_ms\":500,\"limit_c\":\"cold\",\"window_s\":120,\"$version\":4}", rep);
    TEST_ASSERT_EQUAL_STRING(
        "{\"report_ms\":30000,\"limit_c\":8.00,\"window_s\":120,\"applied_version\":4,"
        "\"rejected\":\"report_ms,limit_c\"}", rep);
    TEST_ASSERT_EQUAL_STRING("EVT TWIN 4 rejected=report_ms,limit_c\r\n", st.uart);
}

static void test_twin_range_limits(void)
{
    char rep[LOGGER_REPORTED_MAX];
    apply("{\"limit_c\":-40,\"window_s\":10,\"report_ms\":3600000}", rep);
    TEST_ASSERT_NOT_NULL(strstr(rep, "\"rejected\":\"\""));
    apply("{\"limit_c\":85.01,\"window_s\":9,\"report_ms\":999}", rep);
    TEST_ASSERT_NOT_NULL(strstr(rep, "\"rejected\":\"report_ms,limit_c,window_s\""));
    TEST_ASSERT_NOT_NULL(strstr(rep, "\"limit_c\":-40.00,\"window_s\":10,"));
}

static void test_twin_without_version_keeps_the_last_version(void)
{
    char rep[LOGGER_REPORTED_MAX];
    apply("{\"window_s\":60,\"$version\":8}", rep);
    apply("{\"window_s\":90}", rep);
    TEST_ASSERT_NOT_NULL(strstr(rep, "\"window_s\":90,\"applied_version\":8,"));
}

static void test_twin_reported_buffer_too_small_returns_minus_one(void)
{
    twin_desired_t d;
    char small[20];
    twin_parse("{}", 2, &d);
    TEST_ASSERT_EQUAL(-1, logger_apply_twin(&lg, &d, small, sizeof small));
}

static void test_flush_queues_a_reading_and_replies_with_its_seq(void)
{
    char reply[64];
    send("FAULT NET ON");                                   /* hold it in the queue */
    TEST_ASSERT_EQUAL(200, logger_flush(&lg, reply, sizeof reply));
    TEST_ASSERT_EQUAL_STRING("{\"seq\":1,\"queued\":1}", reply);
    TEST_ASSERT_EQUAL(200, logger_flush(&lg, reply, sizeof reply));
    TEST_ASSERT_EQUAL_STRING("{\"seq\":2,\"queued\":2}", reply);
}

static void test_flush_with_a_failed_read_returns_500(void)
{
    char reply[64];
    st.fail_next = 1;
    TEST_ASSERT_EQUAL(500, logger_flush(&lg, reply, sizeof reply));
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"sensor\"}", reply);
}

static void test_fault_temp_overrides_the_sensor_until_off(void)
{
    TEST_ASSERT_EQUAL_STRING("OK FAULT TEMP 9.50\r\n", send("FAULT TEMP 9.5"));
    TEST_ASSERT_EQUAL_STRING("OK TEMP 9.50\r\n", send("READ"));
    unsigned reads = st.reads;
    send("SEND");
    TEST_ASSERT_EQUAL(reads, st.reads);                     /* no bus traffic */
    tick_at(10);
    TEST_ASSERT_NOT_NULL(strstr(fake_net_payload(&ns, 0), "\"temp\":9.50,"));
    TEST_ASSERT_EQUAL_STRING("OK FAULT TEMP -3.25\r\n", send("FAULT TEMP -3.25"));
    TEST_ASSERT_EQUAL_STRING("OK FAULT TEMP OFF\r\n", send("FAULT TEMP OFF"));
    TEST_ASSERT_EQUAL_STRING("OK TEMP 24.01\r\n", send("READ"));
}

static void test_fault_temp_rejects_bad_values(void)
{
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("FAULT TEMP"));
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("FAULT TEMP hot"));
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("FAULT TEMP 90"));
    TEST_ASSERT_EQUAL_STRING("ERR ARGS\r\n", send("FAULT TEMP 1.234"));
    TEST_ASSERT_FALSE(lg.fault_temp);
}

static void test_fault_temp_bypasses_the_bus_until_turned_off(void)
{
    send("FAULT TEMP 5");
    send("FAULT I2C ON");
    run_until(5000);                                        /* 3 failed scheduled reads */
    TEST_ASSERT_EQUAL(SENSOR_OK, lg.sensor);                /* override: no bus reads, stays OK */
    send("FAULT TEMP OFF");
    run_until(9000);
    TEST_ASSERT_EQUAL(SENSOR_LOST, lg.sensor);              /* real reads fail again */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cfg_shows_defaults);
    RUN_TEST(test_twin_applies_settings_and_reports_them);
    RUN_TEST(test_twin_settings_reach_the_cloud_payload);
    RUN_TEST(test_twin_rejects_bad_values_and_keeps_the_old_ones);
    RUN_TEST(test_twin_range_limits);
    RUN_TEST(test_twin_without_version_keeps_the_last_version);
    RUN_TEST(test_twin_reported_buffer_too_small_returns_minus_one);
    RUN_TEST(test_flush_queues_a_reading_and_replies_with_its_seq);
    RUN_TEST(test_flush_with_a_failed_read_returns_500);
    RUN_TEST(test_fault_temp_overrides_the_sensor_until_off);
    RUN_TEST(test_fault_temp_rejects_bad_values);
    RUN_TEST(test_fault_temp_bypasses_the_bus_until_turned_off);
    RUN_TEST(test_without_network_net_reports_none_and_send_is_refused);
    RUN_TEST(test_first_sample_is_reported_then_one_per_report_interval);
    RUN_TEST(test_report_interval_is_settable_within_range);
    RUN_TEST(test_faster_report_interval_reports_every_sample);
    RUN_TEST(test_lost_sensor_reports_nothing);
    RUN_TEST(test_send_reads_now_and_publishes_on_the_next_pass);
    RUN_TEST(test_send_with_a_failed_read_answers_err_sensor);
    RUN_TEST(test_ack_prints_evt_ack_with_the_seq);
    RUN_TEST(test_link_changes_print_net_up_and_net_down_once);
    RUN_TEST(test_net_reports_queue_and_counters);
    RUN_TEST(test_fault_net_takes_the_link_down_and_off_sends_the_backlog);
    RUN_TEST(test_fault_noack_makes_the_device_send_a_reading_twice);
    RUN_TEST(test_fault_net_and_noack_reject_bad_args);
    RUN_TEST(test_existing_commands_are_unchanged_by_cloud_reporting);
    return UNITY_END();
}
