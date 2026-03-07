/*
 * test_utils.c — Tests for parse_int utility
 *
 * Matches: tests/api/test_utils.py (TestUtils)
 */
#include "test_harness.h"
#include "utils.h"

TEST(test_parse_int_null_is_false) {
    int val;
    ASSERT_FALSE(parse_int(NULL, &val));
}

TEST(test_parse_int_empty_is_false) {
    int val;
    ASSERT_FALSE(parse_int("", &val));
}

TEST(test_parse_int_whitespace_is_false) {
    int val;
    ASSERT_FALSE(parse_int("  \t ", &val));
}

TEST(test_parse_int_float_is_false) {
    int val;
    ASSERT_FALSE(parse_int("3.5", &val));
}

TEST(test_parse_int_decimal_zero) {
    int val;
    ASSERT_TRUE(parse_int("  0 ", &val));
    ASSERT_EQ_INT(val, 0);
}

TEST(test_parse_int_decimal_one) {
    int val;
    ASSERT_TRUE(parse_int("1", &val));
    ASSERT_EQ_INT(val, 1);
}

TEST(test_parse_int_hex_0x) {
    int val;
    ASSERT_TRUE(parse_int("  0xFF", &val));
    ASSERT_EQ_INT(val, 0xFF);
}

TEST(test_parse_int_hex_0x_with_h_suffix_fails) {
    int val;
    /* 0xFFh — the 'h' suffix is invalid with 0x prefix */
    ASSERT_FALSE(parse_int("  0xFFh", &val));
}

TEST(test_parse_int_hex_dollar) {
    int val;
    ASSERT_TRUE(parse_int(" $FF", &val));
    ASSERT_EQ_INT(val, 255);
}

TEST(test_parse_int_hex_h_suffix_letter_start_fails) {
    int val;
    /* FFh — starts with a letter, ambiguous (could be a label) */
    ASSERT_FALSE(parse_int("FFh", &val));
}

TEST(test_parse_int_hex_h_suffix_digit_start) {
    int val;
    ASSERT_TRUE(parse_int("0FFh", &val));
    ASSERT_EQ_INT(val, 255);
}

TEST(test_parse_int_binary_b_suffix) {
    int val;
    ASSERT_TRUE(parse_int("111b", &val));
    ASSERT_EQ_INT(val, 7);
}

TEST(test_parse_int_binary_percent) {
    int val;
    ASSERT_TRUE(parse_int("%111", &val));
    ASSERT_EQ_INT(val, 7);
}

TEST(test_parse_int_hex_0xC000) {
    /* Matches test_org_allows_0xnnnn_format */
    int val;
    ASSERT_TRUE(parse_int("0xC000", &val));
    ASSERT_EQ_INT(val, 0xC000);
}

int main(void) {
    printf("test_utils (matching tests/api/test_utils.py):\n");
    RUN_TEST(test_parse_int_null_is_false);
    RUN_TEST(test_parse_int_empty_is_false);
    RUN_TEST(test_parse_int_whitespace_is_false);
    RUN_TEST(test_parse_int_float_is_false);
    RUN_TEST(test_parse_int_decimal_zero);
    RUN_TEST(test_parse_int_decimal_one);
    RUN_TEST(test_parse_int_hex_0x);
    RUN_TEST(test_parse_int_hex_0x_with_h_suffix_fails);
    RUN_TEST(test_parse_int_hex_dollar);
    RUN_TEST(test_parse_int_hex_h_suffix_letter_start_fails);
    RUN_TEST(test_parse_int_hex_h_suffix_digit_start);
    RUN_TEST(test_parse_int_binary_b_suffix);
    RUN_TEST(test_parse_int_binary_percent);
    RUN_TEST(test_parse_int_hex_0xC000);
    REPORT();
}
