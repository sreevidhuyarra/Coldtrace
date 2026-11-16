/* Device-twin JSON reader: both message shapes, unknown and nested keys, bad input. */
#include <string.h>

#include "unity.h"
#include "twin.h"

static twin_desired_t d;

void setUp(void) {}
void tearDown(void) {}

static bool parse(const char *json)
{
    return twin_parse(json, strlen(json), &d);
}

static void test_desired_patch_is_read_directly(void)
{
    TEST_ASSERT_TRUE(parse("{\"report_ms\":5000,\"limit_c\":6.5,\"window_s\":120,\"$version\":4}"));
    TEST_ASSERT_EQUAL(TWIN_OK, d.report_ms_state);
    TEST_ASSERT_EQUAL_UINT32(5000, d.report_ms);
    TEST_ASSERT_EQUAL(TWIN_OK, d.limit_state);
    TEST_ASSERT_EQUAL_INT32(650, d.limit_centi);
    TEST_ASSERT_EQUAL_UINT32(120, d.window_s);
    TEST_ASSERT_TRUE(d.has_version);
    TEST_ASSERT_EQUAL_UINT32(4, d.version);
}

static void test_full_twin_reads_desired_not_reported(void)
{
    TEST_ASSERT_TRUE(parse(
        "{\"desired\":{\"limit_c\":2,\"$metadata\":{\"limit_c\":{\"$lastUpdated\":\"2026\"}},\"$version\":7},"
        "\"reported\":{\"limit_c\":9.99,\"report_ms\":1000,\"$version\":3}}"));
    TEST_ASSERT_EQUAL_INT32(200, d.limit_centi);
    TEST_ASSERT_EQUAL(TWIN_ABSENT, d.report_ms_state);     /* only in reported: ignored */
    TEST_ASSERT_EQUAL_UINT32(7, d.version);                /* desired's $version, not reported's */
}

static void test_keys_inside_nested_values_are_not_matched(void)
{
    TEST_ASSERT_TRUE(parse("{\"note\":{\"report_ms\":1},\"list\":[{\"window_s\":5}],"
                           "\"s\":\"\\\"report_ms\\\":2\"}"));
    TEST_ASSERT_EQUAL(TWIN_ABSENT, d.report_ms_state);
    TEST_ASSERT_EQUAL(TWIN_ABSENT, d.window_state);
}

static void test_whitespace_and_key_order_do_not_matter(void)
{
    TEST_ASSERT_TRUE(parse(" {\r\n \"$version\" : 9 ,\n\t\"window_s\" :\t600 } "));
    TEST_ASSERT_EQUAL_UINT32(600, d.window_s);
    TEST_ASSERT_EQUAL_UINT32(9, d.version);
}

static void test_limit_decimals_and_sign(void)
{
    parse("{\"limit_c\":-2.05}");
    TEST_ASSERT_EQUAL_INT32(-205, d.limit_centi);
    parse("{\"limit_c\":8}");
    TEST_ASSERT_EQUAL_INT32(800, d.limit_centi);
    parse("{\"limit_c\":0.5}");
    TEST_ASSERT_EQUAL_INT32(50, d.limit_centi);
}

static void test_unusable_values_are_marked_bad(void)
{
    const char *bad[] = {
        "{\"report_ms\":\"5000\"}",      /* string */
        "{\"report_ms\":null}",
        "{\"report_ms\":-5}",
        "{\"report_ms\":5.5}",
        "{\"report_ms\":1e4}",
        "{\"report_ms\":99999999999}",   /* overflows uint32 */
        "{\"limit_c\":1.234}",           /* 3 decimals */
        "{\"limit_c\":\"cold\"}",
        "{\"limit_c\":-}",
        "{\"limit_c\":3.}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_TRUE_MESSAGE(parse(bad[i]), bad[i]);
        TEST_ASSERT_TRUE_MESSAGE(d.report_ms_state == TWIN_BAD || d.limit_state == TWIN_BAD, bad[i]);
    }
}

static void test_malformed_json_is_rejected_without_overrun(void)
{
    const char *bad[] = {
        "", "   ", "[1,2]", "5000", "{", "{\"report_ms\":", "{\"report_ms\" 5}",
        "{\"report_ms\":5000", "{\"s\":\"unterminated}", "{\"a\":{\"b\":1}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_FALSE_MESSAGE(parse(bad[i]), bad[i]);
        TEST_ASSERT_EQUAL(TWIN_ABSENT, d.report_ms_state);
    }
}

static void test_reads_only_len_bytes(void)
{
    const char *json = "{\"report_ms\":5000}GARBAGE";
    TEST_ASSERT_TRUE(twin_parse(json, 18, &d));
    TEST_ASSERT_EQUAL_UINT32(5000, d.report_ms);
    TEST_ASSERT_FALSE(twin_parse(json, 10, &d));          /* cut off mid-object */
}

static void test_empty_object_has_nothing(void)
{
    TEST_ASSERT_TRUE(parse("{}"));
    TEST_ASSERT_EQUAL(TWIN_ABSENT, d.report_ms_state);
    TEST_ASSERT_FALSE(d.has_version);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_desired_patch_is_read_directly);
    RUN_TEST(test_full_twin_reads_desired_not_reported);
    RUN_TEST(test_keys_inside_nested_values_are_not_matched);
    RUN_TEST(test_whitespace_and_key_order_do_not_matter);
    RUN_TEST(test_limit_decimals_and_sign);
    RUN_TEST(test_unusable_values_are_marked_bad);
    RUN_TEST(test_malformed_json_is_rejected_without_overrun);
    RUN_TEST(test_reads_only_len_bytes);
    RUN_TEST(test_empty_object_has_nothing);
    return UNITY_END();
}
