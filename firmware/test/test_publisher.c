/* Store-and-forward publisher tests, with a fake network and a hand-moved clock. */
#include <string.h>

#include "unity.h"
#include "publisher.h"
#include "fake_net.h"

static fake_net_state_t ns;
static net_t net;
static pub_t p;
static uint32_t now;

void setUp(void)
{
    net = fake_net_init(&ns);
    pub_init(&p, &net, "coldtrace-01", 0x1a2b3c4du);
    now = 1000;
}

void tearDown(void) {}

/* One loop pass at time t. */
static void tick_at(uint32_t t)
{
    now = t;
    pub_tick(&p, now);
}

/* Ack the message that was published last. */
static uint32_t ack_last(void)
{
    return pub_on_ack(&p, fake_net_last_id(&ns));
}

/* Tick and ack until the queue is empty (at most n rounds, 1 ms apart). */
static void drain(unsigned n)
{
    while (n-- > 0 && pub_queued(&p) > 0) {
        tick_at(now + 1u);
        ack_last();
    }
}

/* ---- payload and numbering ---------------------------------------------- */

static void test_payload_is_small_json_with_device_boot_and_seq(void)
{
    sample_t s = { .t_ms = 123456, .centi_c = 2401, .seq = 12 };
    char buf[PUB_PAYLOAD_MAX];
    int n = pub_format(&p, &s, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING(
        "{\"dev\":\"coldtrace-01\",\"boot\":\"1a2b3c4d\",\"seq\":12,\"t_ms\":123456,\"temp\":24.01}", buf);
    TEST_ASSERT_EQUAL((int)strlen(buf), n);
    TEST_ASSERT_LESS_THAN(512, n);   /* one IoT Hub metering block */
}

static void test_payload_formats_negative_and_small_temperatures(void)
{
    char buf[PUB_PAYLOAD_MAX];
    sample_t a = { .t_ms = 0, .centi_c = -89, .seq = 1 };
    sample_t b = { .t_ms = 0, .centi_c = -2005, .seq = 1 };
    pub_format(&p, &a, buf, sizeof buf);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"temp\":-0.89}"));
    pub_format(&p, &b, buf, sizeof buf);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"temp\":-20.05}"));
}

static void test_payload_refuses_a_buffer_too_small_rather_than_cutting(void)
{
    sample_t s = { .t_ms = 1, .centi_c = 1, .seq = 1 };
    char buf[20];
    TEST_ASSERT_EQUAL(-1, pub_format(&p, &s, buf, sizeof buf));
}

static void test_sequence_numbers_start_at_1_and_count_up(void)
{
    TEST_ASSERT_EQUAL_UINT32(1, pub_enqueue(&p, 10, 2400));
    TEST_ASSERT_EQUAL_UINT32(2, pub_enqueue(&p, 20, 2400));
    TEST_ASSERT_EQUAL_UINT32(3, pub_enqueue(&p, 30, 2400));
    TEST_ASSERT_EQUAL(3, pub_queued(&p));
}

/* ---- normal send ------------------------------------------------------------- */

static void test_reading_is_sent_once_and_dropped_on_its_ack(void)
{
    pub_enqueue(&p, 10, 2401);
    tick_at(1000);
    TEST_ASSERT_EQUAL(1, ns.published);
    TEST_ASSERT_EQUAL_UINT32(1, fake_net_seq(&ns, 0));

    tick_at(1500);                                   /* awaiting the ack: nothing new */
    TEST_ASSERT_EQUAL(1, ns.published);
    TEST_ASSERT_EQUAL(1, pub_queued(&p));            /* kept until acknowledged */

    TEST_ASSERT_EQUAL_UINT32(1, ack_last());
    TEST_ASSERT_EQUAL(0, pub_queued(&p));
    TEST_ASSERT_EQUAL_UINT32(1, p.acked);
    TEST_ASSERT_EQUAL_UINT32(0, p.resends);
}

static void test_one_message_in_flight_at_a_time_oldest_first(void)
{
    pub_enqueue(&p, 10, 2401);
    pub_enqueue(&p, 20, 2402);
    tick_at(1000);
    tick_at(1001);
    TEST_ASSERT_EQUAL(1, ns.published);              /* second waits for the first ack */
    ack_last();
    tick_at(1002);
    TEST_ASSERT_EQUAL(2, ns.published);
    TEST_ASSERT_EQUAL_UINT32(2, fake_net_seq(&ns, 1));
}

static void test_ack_for_another_message_is_ignored(void)
{
    pub_enqueue(&p, 10, 2401);
    tick_at(1000);
    TEST_ASSERT_EQUAL_UINT32(0, pub_on_ack(&p, fake_net_last_id(&ns) + 1));
    TEST_ASSERT_EQUAL_UINT32(0, pub_on_ack(&p, -1));
    TEST_ASSERT_EQUAL(1, pub_queued(&p));
    TEST_ASSERT_EQUAL_UINT32(0, p.acked);
}

static void test_duplicate_ack_does_not_drop_a_second_reading(void)
{
    pub_enqueue(&p, 10, 2401);
    pub_enqueue(&p, 20, 2402);
    tick_at(1000);
    int first = fake_net_last_id(&ns);
    pub_on_ack(&p, first);
    TEST_ASSERT_EQUAL_UINT32(0, pub_on_ack(&p, first));   /* same ack again */
    TEST_ASSERT_EQUAL(1, pub_queued(&p));
}

/* ---- missing acknowledgement --------------------------------------------- */

static void test_missing_ack_resends_the_same_reading_after_the_timeout(void)
{
    pub_enqueue(&p, 10, 2401);
    tick_at(1000);
    tick_at(1000 + PUB_ACK_TIMEOUT_MS - 1u);
    TEST_ASSERT_EQUAL(1, ns.published);

    tick_at(1000 + PUB_ACK_TIMEOUT_MS);
    TEST_ASSERT_EQUAL(2, ns.published);
    TEST_ASSERT_EQUAL_UINT32(1, fake_net_seq(&ns, 1));    /* same seq: cloud de-duplicates */
    TEST_ASSERT_EQUAL_UINT32(1, p.resends);

    TEST_ASSERT_EQUAL_UINT32(0, pub_on_ack(&p, fake_net_msg_id(&ns, 0)));  /* late ack of the first copy */
    TEST_ASSERT_EQUAL_UINT32(1, ack_last());
    TEST_ASSERT_EQUAL(0, pub_queued(&p));
}

static void test_ack_timeout_survives_millis_wraparound(void)
{
    pub_enqueue(&p, 10, 2401);
    tick_at(UINT32_MAX - 100u);
    tick_at(50);                                          /* 151 ms later, after the wrap */
    TEST_ASSERT_EQUAL(1, ns.published);
    tick_at(UINT32_MAX - 100u + PUB_ACK_TIMEOUT_MS);      /* wraps to the timeout exactly */
    TEST_ASSERT_EQUAL(2, ns.published);
}

static void test_refused_publish_is_retried_after_a_pause(void)
{
    ns.refuse = true;
    pub_enqueue(&p, 10, 2401);
    tick_at(1000);
    TEST_ASSERT_EQUAL(1, ns.refused);
    tick_at(1000 + PUB_RETRY_MS - 1u);
    TEST_ASSERT_EQUAL(1, ns.refused);                     /* no hammering every loop pass */

    ns.refuse = false;
    tick_at(1000 + PUB_RETRY_MS);
    TEST_ASSERT_EQUAL(1, ns.published);
    TEST_ASSERT_EQUAL(1, pub_queued(&p));
}

/* ---- outage ---------------------------------------------------------------------- */

static void test_nothing_is_sent_while_the_link_is_down(void)
{
    ns.up = false;
    pub_enqueue(&p, 10, 2401);
    tick_at(1000);
    tick_at(60000);
    TEST_ASSERT_EQUAL(0, ns.published);
    TEST_ASSERT_EQUAL(0, ns.refused);                     /* not even attempted */
    TEST_ASSERT_FALSE(pub_link_up(&p));
    TEST_ASSERT_EQUAL(1, pub_queued(&p));
}

static void test_two_minute_outage_backlog_is_sent_in_order_after_reconnect(void)
{
    ns.up = false;
    for (uint32_t t = 30000; t <= 120000; t += 30000) {   /* 4 readings, 30 s apart */
        pub_enqueue(&p, t, (int16_t)(2400 + t / 30000));
        tick_at(t);
    }
    TEST_ASSERT_EQUAL(4, pub_queued(&p));
    TEST_ASSERT_EQUAL(0, ns.published);

    ns.up = true;
    drain(10);
    TEST_ASSERT_EQUAL(4, ns.published);
    for (unsigned i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT32(i + 1u, fake_net_seq(&ns, i));   /* every seq once, in order */
    }
    TEST_ASSERT_EQUAL(0, pub_queued(&p));
    TEST_ASSERT_EQUAL_UINT32(0, pub_dropped(&p));
}

static void test_link_lost_while_in_flight_resends_after_reconnect(void)
{
    pub_enqueue(&p, 10, 2401);
    tick_at(1000);
    ns.up = false;
    tick_at(1100);
    TEST_ASSERT_EQUAL_UINT32(1, p.resends);
    TEST_ASSERT_EQUAL_UINT32(0, ack_last());               /* old session's ack can't count */

    ns.up = true;
    tick_at(1200);                                         /* no ack-timeout wait */
    TEST_ASSERT_EQUAL(2, ns.published);
    TEST_ASSERT_EQUAL_UINT32(1, fake_net_seq(&ns, 1));
}

/* ---- overflow ---------------------------------------------------------------- */

static void test_overflow_drops_oldest_and_leaves_a_seq_gap(void)
{
    ns.up = false;
    for (unsigned i = 0; i < RB_CAPACITY + 6u; i++) {     /* 70 readings, 64 slots */
        pub_enqueue(&p, i, 2400);
    }
    TEST_ASSERT_EQUAL(RB_CAPACITY, pub_queued(&p));
    TEST_ASSERT_EQUAL_UINT32(6, pub_dropped(&p));

    ns.up = true;
    drain(RB_CAPACITY + 5u);
    TEST_ASSERT_EQUAL(RB_CAPACITY, ns.published);
    TEST_ASSERT_EQUAL_UINT32(7, fake_net_seq(&ns, 0));     /* 1-6 lost: the cloud sees the gap */
    TEST_ASSERT_EQUAL_UINT32(70, fake_net_seq(&ns, RB_CAPACITY - 1u));
}

static void test_ack_after_its_reading_was_overwritten_drops_nothing(void)
{
    pub_enqueue(&p, 0, 2400);                               /* seq 1 */
    tick_at(1000);                                          /* seq 1 in flight */
    for (unsigned i = 0; i < RB_CAPACITY; i++) {           /* seq 2..65 overwrite seq 1 */
        pub_enqueue(&p, i, 2400);
    }
    TEST_ASSERT_EQUAL_UINT32(1, ack_last());               /* seq 1 was delivered */
    TEST_ASSERT_EQUAL(RB_CAPACITY, pub_queued(&p));        /* but seq 2 is not dropped */
    tick_at(1001);
    TEST_ASSERT_EQUAL_UINT32(2, fake_net_seq(&ns, 1));
}

/* ---- test hooks and no network ----------------------------------------------- */

static void test_fault_net_holds_readings_like_a_real_outage(void)
{
    p.fault_net = true;
    pub_enqueue(&p, 10, 2401);
    tick_at(1000);
    TEST_ASSERT_FALSE(pub_link_up(&p));
    TEST_ASSERT_EQUAL(0, ns.published);

    p.fault_net = false;
    tick_at(1001);
    TEST_ASSERT_TRUE(pub_link_up(&p));
    TEST_ASSERT_EQUAL(1, ns.published);
}

static void test_fault_noack_forces_resends_until_turned_off(void)
{
    p.fault_noack = true;
    pub_enqueue(&p, 10, 2401);
    tick_at(1000);
    TEST_ASSERT_EQUAL_UINT32(0, ack_last());               /* ignored */
    tick_at(1000 + PUB_ACK_TIMEOUT_MS);
    TEST_ASSERT_EQUAL(2, ns.published);                    /* the cloud now has seq 1 twice */

    p.fault_noack = false;
    TEST_ASSERT_EQUAL_UINT32(1, ack_last());
    TEST_ASSERT_EQUAL(0, pub_queued(&p));
}

static void test_without_a_network_readings_just_queue(void)
{
    pub_init(&p, NULL, "coldtrace-01", 1);
    pub_enqueue(&p, 10, 2401);
    tick_at(1000);
    TEST_ASSERT_FALSE(pub_link_up(&p));
    TEST_ASSERT_EQUAL(1, pub_queued(&p));
}

static void test_over_long_device_id_is_cut_not_overrun(void)
{
    pub_init(&p, &net, "a-device-id-that-is-far-too-long-for-the-field", 1);
    TEST_ASSERT_EQUAL(PUB_DEV_ID_MAX - 1u, strlen(p.dev_id));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_payload_is_small_json_with_device_boot_and_seq);
    RUN_TEST(test_payload_formats_negative_and_small_temperatures);
    RUN_TEST(test_payload_refuses_a_buffer_too_small_rather_than_cutting);
    RUN_TEST(test_sequence_numbers_start_at_1_and_count_up);
    RUN_TEST(test_reading_is_sent_once_and_dropped_on_its_ack);
    RUN_TEST(test_one_message_in_flight_at_a_time_oldest_first);
    RUN_TEST(test_ack_for_another_message_is_ignored);
    RUN_TEST(test_duplicate_ack_does_not_drop_a_second_reading);
    RUN_TEST(test_missing_ack_resends_the_same_reading_after_the_timeout);
    RUN_TEST(test_ack_timeout_survives_millis_wraparound);
    RUN_TEST(test_refused_publish_is_retried_after_a_pause);
    RUN_TEST(test_nothing_is_sent_while_the_link_is_down);
    RUN_TEST(test_two_minute_outage_backlog_is_sent_in_order_after_reconnect);
    RUN_TEST(test_link_lost_while_in_flight_resends_after_reconnect);
    RUN_TEST(test_overflow_drops_oldest_and_leaves_a_seq_gap);
    RUN_TEST(test_ack_after_its_reading_was_overwritten_drops_nothing);
    RUN_TEST(test_fault_net_holds_readings_like_a_real_outage);
    RUN_TEST(test_fault_noack_forces_resends_until_turned_off);
    RUN_TEST(test_without_a_network_readings_just_queue);
    RUN_TEST(test_over_long_device_id_is_cut_not_overrun);
    return UNITY_END();
}
