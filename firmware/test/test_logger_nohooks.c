/* The production build: compiled WITHOUT -DLOGGER_TEST_HOOKS.
 * Fault-injection commands must not exist there. */
#include "unity.h"
#include "logger.h"
#include "fake_hal.h"

static fake_hal_state_t st;
static hal_t hal;
static logger_t lg;

void setUp(void)
{
    hal = fake_hal_init(&st);
    logger_init(&lg, &hal, RESET_POWERON);
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

static void test_fault_commands_are_unknown_without_hooks(void)
{
    TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN\r\n", send("FAULT I2C ON"));
    TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN\r\n", send("FAULT HANG"));
    TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN\r\n", send("FAULT NET ON"));
    TEST_ASSERT_EQUAL_STRING("ERR UNKNOWN\r\n", send("FAULT NOACK ON"));
    TEST_ASSERT_EQUAL(ACT_NONE, logger_take_action(&lg));
    TEST_ASSERT_FALSE(lg.fault_i2c);
    TEST_ASSERT_FALSE(lg.pub.fault_net);
    TEST_ASSERT_FALSE(lg.pub.fault_noack);
}

static void test_normal_commands_still_work_without_hooks(void)
{
    TEST_ASSERT_EQUAL_STRING("OK PONG\r\n", send("PING"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fault_commands_are_unknown_without_hooks);
    RUN_TEST(test_normal_commands_still_work_without_hooks);
    return UNITY_END();
}
