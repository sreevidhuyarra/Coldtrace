#include <string.h>

#include "unity.h"
#include "cmd_parser.h"

static cmd_parser_t p;

void setUp(void) { cmd_parser_init(&p); }
void tearDown(void) {}

/* Feed a whole string; return the result of the last byte. */
static cmd_result_t feed(const char *s)
{
    cmd_result_t r = CMD_NONE;
    for (; *s; s++) {
        r = cmd_parser_feed(&p, *s);
    }
    return r;
}

static void feed_chars(char c, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL(CMD_NONE, cmd_parser_feed(&p, c));
    }
}

static void test_simple_command_line(void)
{
    TEST_ASSERT_EQUAL(CMD_NONE, feed("PING"));
    TEST_ASSERT_EQUAL(CMD_LINE, feed("\n"));
    TEST_ASSERT_EQUAL_STRING("PING", cmd_parser_line(&p));
}

static void test_crlf_gives_exactly_one_line(void)
{
    TEST_ASSERT_EQUAL(CMD_NONE, feed("VERSION"));
    TEST_ASSERT_EQUAL(CMD_LINE, cmd_parser_feed(&p, '\r'));
    TEST_ASSERT_EQUAL(CMD_NONE, cmd_parser_feed(&p, '\n'));      /* not a second line */
    TEST_ASSERT_EQUAL_STRING("VERSION", cmd_parser_line(&p));    /* buffer untouched */
}

static void test_bare_cr_ends_a_line(void)
{
    /* what a terminal sends when you press Enter */
    TEST_ASSERT_EQUAL(CMD_LINE, feed("PING\r"));
    TEST_ASSERT_EQUAL_STRING("PING", cmd_parser_line(&p));
}

static void test_empty_lines_are_ignored(void)
{
    TEST_ASSERT_EQUAL(CMD_NONE, feed("\n"));
    TEST_ASSERT_EQUAL(CMD_NONE, feed("\r\n"));
    TEST_ASSERT_EQUAL(CMD_LINE, feed("PING\n"));
}

static void test_exactly_64_chars_is_accepted(void)
{
    feed_chars('A', CMD_LINE_MAX);
    TEST_ASSERT_EQUAL(CMD_LINE, cmd_parser_feed(&p, '\n'));
    TEST_ASSERT_EQUAL_size_t(CMD_LINE_MAX, strlen(cmd_parser_line(&p)));
}

static void test_exactly_65_chars_is_toolong(void)
{
    /* the boundary itself: an off-by-one here would also write past buf */
    feed_chars('A', CMD_LINE_MAX + 1u);
    TEST_ASSERT_EQUAL(CMD_TOOLONG, cmd_parser_feed(&p, '\n'));
}

static void test_very_long_line_is_one_toolong_and_next_line_recovers(void)
{
    feed_chars('A', 300);                   /* keeps discarding */
    TEST_ASSERT_EQUAL(CMD_TOOLONG, cmd_parser_feed(&p, '\n'));
    TEST_ASSERT_EQUAL(CMD_LINE, feed("PING\n"));
    TEST_ASSERT_EQUAL_STRING("PING", cmd_parser_line(&p));
}

static void test_consecutive_lines_do_not_mix(void)
{
    TEST_ASSERT_EQUAL(CMD_LINE, feed("STATUS\n"));
    TEST_ASSERT_EQUAL(CMD_LINE, feed("READ\n"));
    TEST_ASSERT_EQUAL_STRING("READ", cmd_parser_line(&p));
}

static void test_split_command_without_args(void)
{
    char line[] = "PING";
    cmd_t c;
    TEST_ASSERT_TRUE(cmd_split(line, &c));
    TEST_ASSERT_EQUAL_STRING("PING", c.name);
    TEST_ASSERT_EQUAL_UINT8(0, c.argc);
}

static void test_split_command_with_args(void)
{
    char line[] = "FAULT I2C ON";
    cmd_t c;
    TEST_ASSERT_TRUE(cmd_split(line, &c));
    TEST_ASSERT_EQUAL_STRING("FAULT", c.name);
    TEST_ASSERT_EQUAL_UINT8(2, c.argc);
    TEST_ASSERT_EQUAL_STRING("I2C", c.args[0]);
    TEST_ASSERT_EQUAL_STRING("ON", c.args[1]);
}

static void test_split_rejects_empty_tokens(void)
{
    char dbl[] = "RATE  500";
    char lead[] = " PING";
    char trail[] = "PING ";
    cmd_t c;
    TEST_ASSERT_FALSE(cmd_split(dbl, &c));
    TEST_ASSERT_FALSE(cmd_split(lead, &c));
    TEST_ASSERT_FALSE(cmd_split(trail, &c));
}

static void test_split_rejects_too_many_args(void)
{
    char ok[] = "A 1 2 3";
    char bad[] = "A 1 2 3 4";
    cmd_t c;
    TEST_ASSERT_TRUE(cmd_split(ok, &c));
    TEST_ASSERT_EQUAL_UINT8(3, c.argc);
    TEST_ASSERT_FALSE(cmd_split(bad, &c));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_simple_command_line);
    RUN_TEST(test_bare_cr_ends_a_line);
    RUN_TEST(test_crlf_gives_exactly_one_line);
    RUN_TEST(test_empty_lines_are_ignored);
    RUN_TEST(test_exactly_64_chars_is_accepted);
    RUN_TEST(test_exactly_65_chars_is_toolong);
    RUN_TEST(test_very_long_line_is_one_toolong_and_next_line_recovers);
    RUN_TEST(test_consecutive_lines_do_not_mix);
    RUN_TEST(test_split_command_without_args);
    RUN_TEST(test_split_command_with_args);
    RUN_TEST(test_split_rejects_empty_tokens);
    RUN_TEST(test_split_rejects_too_many_args);
    return UNITY_END();
}
