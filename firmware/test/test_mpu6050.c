#include "unity.h"
#include "mpu6050.h"
#include "fake_hal.h"

static fake_hal_state_t st;
static hal_t hal;

void setUp(void) { hal = fake_hal_init(&st); }
void tearDown(void) {}

static void test_init_probes_and_wakes_a_real_mpu6050(void)
{
    TEST_ASSERT_EQUAL(MPU_OK, mpu6050_init(&hal));
    TEST_ASSERT_EQUAL_HEX8(MPU6050_REG_PWR_MGMT_1, st.last_write_reg);
    TEST_ASSERT_EQUAL_HEX8(0x00, st.last_write_val);   /* SLEEP cleared */
}

static void test_init_rejects_wrong_who_am_i_without_writing(void)
{
    st.regs[MPU6050_REG_WHO_AM_I] = 0x70;              /* some other chip */
    TEST_ASSERT_EQUAL(MPU_ERR_WHOAMI, mpu6050_init(&hal));
    TEST_ASSERT_EQUAL_UINT(0, st.writes);
}

static void test_init_reports_i2c_failure_on_probe(void)
{
    st.fail_next = 1;
    TEST_ASSERT_EQUAL(MPU_ERR_I2C, mpu6050_init(&hal));
}

static void test_init_reports_i2c_failure_on_wake(void)
{
    st.fail_writes = true;                             /* probe works, wake fails */
    TEST_ASSERT_EQUAL(MPU_ERR_I2C, mpu6050_init(&hal));
    TEST_ASSERT_EQUAL_UINT(1, st.reads);
    TEST_ASSERT_EQUAL_UINT(1, st.writes);
}

static void test_read_raw_assembles_big_endian_signed(void)
{
    int16_t raw = 0;
    st.regs[0x41] = 0xEF;                              /* 0xEF5C = -4260 */
    st.regs[0x42] = 0x5C;
    TEST_ASSERT_EQUAL(MPU_OK, mpu6050_read_raw_temp(&hal, &raw));
    TEST_ASSERT_EQUAL_INT16(-4260, raw);
}

static void test_read_reports_i2c_failure_and_leaves_output(void)
{
    int16_t centi = 1234;
    fake_hal_set_raw_temp(&st, 0);
    st.fail_next = 1;
    TEST_ASSERT_EQUAL(MPU_ERR_I2C, mpu6050_read_centi(&hal, &centi));
    TEST_ASSERT_EQUAL_INT16(1234, centi);              /* untouched on error */
}

/* Expected values were checked against Wokwi in the Day 1 spike
 * (24 C -> raw -4260 -> 2401, 85 C -> raw 16480 -> 8500). */
static void test_conversion_at_known_points(void)
{
    TEST_ASSERT_EQUAL_INT16(1,     mpu6050_raw_to_centi(-12420));  /*   0 C */
    TEST_ASSERT_EQUAL_INT16(2401,  mpu6050_raw_to_centi(-4260));   /*  24 C */
    TEST_ASSERT_EQUAL_INT16(-3999, mpu6050_raw_to_centi(-26020));  /* -40 C */
    TEST_ASSERT_EQUAL_INT16(8500,  mpu6050_raw_to_centi(16480));   /*  85 C */
}

static void test_conversion_is_safe_at_raw_extremes(void)
{
    TEST_ASSERT_EQUAL_INT16(-5984, mpu6050_raw_to_centi(INT16_MIN));
    TEST_ASSERT_EQUAL_INT16(13290, mpu6050_raw_to_centi(INT16_MAX));
}

static void test_conversion_within_0_01_of_datasheet_formula(void)
{
    /* exact centi = raw * 10 / 34 + 3653 with no rounding; multiply through
     * by 34 to compare in integers: |got*34 - (raw*10 + 3653*34)| < 34 */
    for (int32_t raw = INT16_MIN; raw <= INT16_MAX; raw++) {
        int32_t got34 = (int32_t)mpu6050_raw_to_centi((int16_t)raw) * 34;
        int32_t exact34 = raw * 10 + 3653 * 34;
        int32_t diff = got34 - exact34;
        if (diff <= -34 || diff >= 34) {
            TEST_FAIL_MESSAGE("conversion off by 0.01 C or more");
        }
    }
}

static void test_read_centi_end_to_end(void)
{
    int16_t centi = 0;
    fake_hal_set_raw_temp(&st, 16480);
    TEST_ASSERT_EQUAL(MPU_OK, mpu6050_read_centi(&hal, &centi));
    TEST_ASSERT_EQUAL_INT16(8500, centi);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_probes_and_wakes_a_real_mpu6050);
    RUN_TEST(test_init_rejects_wrong_who_am_i_without_writing);
    RUN_TEST(test_init_reports_i2c_failure_on_probe);
    RUN_TEST(test_init_reports_i2c_failure_on_wake);
    RUN_TEST(test_read_raw_assembles_big_endian_signed);
    RUN_TEST(test_read_reports_i2c_failure_and_leaves_output);
    RUN_TEST(test_conversion_at_known_points);
    RUN_TEST(test_conversion_is_safe_at_raw_extremes);
    RUN_TEST(test_conversion_within_0_01_of_datasheet_formula);
    RUN_TEST(test_read_centi_end_to_end);
    return UNITY_END();
}
