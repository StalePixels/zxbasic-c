/*
 * test_types.c — Tests for the type system
 *
 * Matches: type-related assertions from tests/symbols/test_symbolBASICTYPE.py
 *          and tests/symbols/test_symbolTYPE.py
 */
#include "test_harness.h"
#include "types.h"

/* ---- Basic type sizes (from Python: BASICTYPE(TYPE.xxx).size) ---- */
TEST(test_basictype_sizes) {
    ASSERT_EQ_INT(basictype_size(TYPE_unknown), 0);
    ASSERT_EQ_INT(basictype_size(TYPE_byte), 1);
    ASSERT_EQ_INT(basictype_size(TYPE_ubyte), 1);
    ASSERT_EQ_INT(basictype_size(TYPE_integer), 2);
    ASSERT_EQ_INT(basictype_size(TYPE_uinteger), 2);
    ASSERT_EQ_INT(basictype_size(TYPE_long), 4);
    ASSERT_EQ_INT(basictype_size(TYPE_ulong), 4);
    ASSERT_EQ_INT(basictype_size(TYPE_fixed), 4);
    ASSERT_EQ_INT(basictype_size(TYPE_float), 5);
    ASSERT_EQ_INT(basictype_size(TYPE_string), 2);
    ASSERT_EQ_INT(basictype_size(TYPE_boolean), 1);
}

/* ---- Signedness ---- */
TEST(test_basictype_signed) {
    ASSERT_TRUE(basictype_is_signed(TYPE_byte));
    ASSERT_TRUE(basictype_is_signed(TYPE_integer));
    ASSERT_TRUE(basictype_is_signed(TYPE_long));
    ASSERT_TRUE(basictype_is_signed(TYPE_fixed));
    ASSERT_TRUE(basictype_is_signed(TYPE_float));

    ASSERT_FALSE(basictype_is_signed(TYPE_ubyte));
    ASSERT_FALSE(basictype_is_signed(TYPE_uinteger));
    ASSERT_FALSE(basictype_is_signed(TYPE_ulong));
    ASSERT_FALSE(basictype_is_signed(TYPE_boolean));
    ASSERT_FALSE(basictype_is_signed(TYPE_string));
    ASSERT_FALSE(basictype_is_signed(TYPE_unknown));
}

TEST(test_basictype_unsigned) {
    ASSERT_TRUE(basictype_is_unsigned(TYPE_boolean));
    ASSERT_TRUE(basictype_is_unsigned(TYPE_ubyte));
    ASSERT_TRUE(basictype_is_unsigned(TYPE_uinteger));
    ASSERT_TRUE(basictype_is_unsigned(TYPE_ulong));

    ASSERT_FALSE(basictype_is_unsigned(TYPE_byte));
    ASSERT_FALSE(basictype_is_unsigned(TYPE_integer));
    ASSERT_FALSE(basictype_is_unsigned(TYPE_long));
    ASSERT_FALSE(basictype_is_unsigned(TYPE_fixed));
    ASSERT_FALSE(basictype_is_unsigned(TYPE_float));
    ASSERT_FALSE(basictype_is_unsigned(TYPE_string));
}

/* ---- Numeric predicates ---- */
TEST(test_basictype_integral) {
    ASSERT_TRUE(basictype_is_integral(TYPE_boolean));
    ASSERT_TRUE(basictype_is_integral(TYPE_byte));
    ASSERT_TRUE(basictype_is_integral(TYPE_ubyte));
    ASSERT_TRUE(basictype_is_integral(TYPE_integer));
    ASSERT_TRUE(basictype_is_integral(TYPE_uinteger));
    ASSERT_TRUE(basictype_is_integral(TYPE_long));
    ASSERT_TRUE(basictype_is_integral(TYPE_ulong));

    ASSERT_FALSE(basictype_is_integral(TYPE_fixed));
    ASSERT_FALSE(basictype_is_integral(TYPE_float));
    ASSERT_FALSE(basictype_is_integral(TYPE_string));
}

TEST(test_basictype_decimal) {
    ASSERT_TRUE(basictype_is_decimal(TYPE_fixed));
    ASSERT_TRUE(basictype_is_decimal(TYPE_float));

    ASSERT_FALSE(basictype_is_decimal(TYPE_byte));
    ASSERT_FALSE(basictype_is_decimal(TYPE_integer));
    ASSERT_FALSE(basictype_is_decimal(TYPE_string));
}

TEST(test_basictype_numeric) {
    ASSERT_TRUE(basictype_is_numeric(TYPE_byte));
    ASSERT_TRUE(basictype_is_numeric(TYPE_ubyte));
    ASSERT_TRUE(basictype_is_numeric(TYPE_integer));
    ASSERT_TRUE(basictype_is_numeric(TYPE_uinteger));
    ASSERT_TRUE(basictype_is_numeric(TYPE_long));
    ASSERT_TRUE(basictype_is_numeric(TYPE_ulong));
    ASSERT_TRUE(basictype_is_numeric(TYPE_fixed));
    ASSERT_TRUE(basictype_is_numeric(TYPE_float));
    ASSERT_TRUE(basictype_is_numeric(TYPE_boolean));

    ASSERT_FALSE(basictype_is_numeric(TYPE_string));
    ASSERT_FALSE(basictype_is_numeric(TYPE_unknown));
}

/* ---- Type name round-tripping ---- */
TEST(test_basictype_name_roundtrip) {
    for (int i = 0; i < TYPE_COUNT; i++) {
        const char *name = basictype_to_string((BasicType)i);
        BasicType bt = basictype_from_string(name);
        ASSERT_EQ_INT(bt, i);
    }
}

/* ---- Signed conversion ---- */
TEST(test_basictype_to_signed) {
    ASSERT_EQ(basictype_to_signed(TYPE_ubyte), TYPE_byte);
    ASSERT_EQ(basictype_to_signed(TYPE_uinteger), TYPE_integer);
    ASSERT_EQ(basictype_to_signed(TYPE_ulong), TYPE_long);
    ASSERT_EQ(basictype_to_signed(TYPE_boolean), TYPE_byte);
    /* Already signed stays the same */
    ASSERT_EQ(basictype_to_signed(TYPE_byte), TYPE_byte);
    ASSERT_EQ(basictype_to_signed(TYPE_integer), TYPE_integer);
    ASSERT_EQ(basictype_to_signed(TYPE_long), TYPE_long);
    ASSERT_EQ(basictype_to_signed(TYPE_fixed), TYPE_fixed);
    ASSERT_EQ(basictype_to_signed(TYPE_float), TYPE_float);
}

/* ---- Deprecated suffixes ---- */
TEST(test_suffix_to_type) {
    ASSERT_EQ(suffix_to_type('$'), TYPE_string);
    ASSERT_EQ(suffix_to_type('%'), TYPE_integer);
    ASSERT_EQ(suffix_to_type('&'), TYPE_long);
    ASSERT_EQ(suffix_to_type('x'), TYPE_unknown);
}

TEST(test_is_deprecated_suffix) {
    ASSERT_TRUE(is_deprecated_suffix('$'));
    ASSERT_TRUE(is_deprecated_suffix('%'));
    ASSERT_TRUE(is_deprecated_suffix('&'));
    ASSERT_FALSE(is_deprecated_suffix('x'));
    ASSERT_FALSE(is_deprecated_suffix('a'));
}

int main(void) {
    printf("test_types (matching tests/symbols/test_symbolBASICTYPE.py):\n");
    RUN_TEST(test_basictype_sizes);
    RUN_TEST(test_basictype_signed);
    RUN_TEST(test_basictype_unsigned);
    RUN_TEST(test_basictype_integral);
    RUN_TEST(test_basictype_decimal);
    RUN_TEST(test_basictype_numeric);
    RUN_TEST(test_basictype_name_roundtrip);
    RUN_TEST(test_basictype_to_signed);
    RUN_TEST(test_suffix_to_type);
    RUN_TEST(test_is_deprecated_suffix);
    REPORT();
}
