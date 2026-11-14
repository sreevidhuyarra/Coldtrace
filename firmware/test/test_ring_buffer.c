#include "unity.h"
#include "ring_buffer.h"

static ring_buffer_t rb;

void setUp(void) { rb_init(&rb); }
void tearDown(void) {}

static sample_t mk(uint32_t t) { sample_t s = { t, (int16_t)(t * 10u) }; return s; }

static void push_n(uint32_t first_t, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        rb_push(&rb, mk(first_t + i));
    }
}

static void test_new_buffer_is_empty(void)
{
    sample_t s;
    TEST_ASSERT_EQUAL_UINT8(0, rb_count(&rb));
    TEST_ASSERT_EQUAL_UINT32(0, rb_overflows(&rb));
    TEST_ASSERT_FALSE(rb_get(&rb, 0, &s));
}

static void test_get_returns_samples_oldest_first(void)
{
    sample_t s;
    push_n(100, 3);
    TEST_ASSERT_EQUAL_UINT8(3, rb_count(&rb));
    TEST_ASSERT_TRUE(rb_get(&rb, 0, &s));
    TEST_ASSERT_EQUAL_UINT32(100, s.t_ms);
    TEST_ASSERT_EQUAL_INT16(1000, s.centi_c);
    TEST_ASSERT_TRUE(rb_get(&rb, 2, &s));
    TEST_ASSERT_EQUAL_UINT32(102, s.t_ms);
}

static void test_get_past_count_fails(void)
{
    sample_t s;
    push_n(0, 2);
    TEST_ASSERT_FALSE(rb_get(&rb, 2, &s));
}

static void test_exactly_capacity_fills_without_overflow(void)
{
    sample_t s;
    push_n(0, RB_CAPACITY);
    TEST_ASSERT_EQUAL_UINT8(RB_CAPACITY, rb_count(&rb));
    TEST_ASSERT_EQUAL_UINT32(0, rb_overflows(&rb));
    TEST_ASSERT_TRUE(rb_get(&rb, 0, &s));
    TEST_ASSERT_EQUAL_UINT32(0, s.t_ms);
    TEST_ASSERT_TRUE(rb_get(&rb, RB_CAPACITY - 1u, &s));
    TEST_ASSERT_EQUAL_UINT32(RB_CAPACITY - 1u, s.t_ms);
}

static void test_full_buffer_overwrites_oldest(void)
{
    sample_t s;
    push_n(0, RB_CAPACITY + 5u);           /* samples 0..4 are overwritten */
    TEST_ASSERT_EQUAL_UINT8(RB_CAPACITY, rb_count(&rb));
    TEST_ASSERT_TRUE(rb_get(&rb, 0, &s));
    TEST_ASSERT_EQUAL_UINT32(5, s.t_ms);
    TEST_ASSERT_TRUE(rb_get(&rb, RB_CAPACITY - 1u, &s));
    TEST_ASSERT_EQUAL_UINT32(RB_CAPACITY + 4u, s.t_ms);
}

static void test_overflow_counts_each_lost_sample(void)
{
    push_n(0, RB_CAPACITY + 5u);
    TEST_ASSERT_EQUAL_UINT32(5, rb_overflows(&rb));
    push_n(1000, RB_CAPACITY * 3u);        /* many full laps of the buffer */
    TEST_ASSERT_EQUAL_UINT32(5u + RB_CAPACITY * 3u, rb_overflows(&rb));
}

static void test_clear_empties_but_keeps_overflow_count(void)
{
    sample_t s;
    push_n(0, RB_CAPACITY + 2u);
    rb_clear(&rb);
    TEST_ASSERT_EQUAL_UINT8(0, rb_count(&rb));
    TEST_ASSERT_FALSE(rb_get(&rb, 0, &s));
    TEST_ASSERT_EQUAL_UINT32(2, rb_overflows(&rb));
    rb_push(&rb, mk(77));                  /* still usable after clear */
    TEST_ASSERT_TRUE(rb_get(&rb, 0, &s));
    TEST_ASSERT_EQUAL_UINT32(77, s.t_ms);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_new_buffer_is_empty);
    RUN_TEST(test_get_returns_samples_oldest_first);
    RUN_TEST(test_get_past_count_fails);
    RUN_TEST(test_exactly_capacity_fills_without_overflow);
    RUN_TEST(test_full_buffer_overwrites_oldest);
    RUN_TEST(test_overflow_counts_each_lost_sample);
    RUN_TEST(test_clear_empties_but_keeps_overflow_count);
    return UNITY_END();
}
