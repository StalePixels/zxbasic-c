/*
 * test_harness.h — Minimal C test framework (no external dependencies)
 *
 * Usage:
 *   TEST(test_name) { ASSERT(...); ASSERT_EQ(a, b); }
 *   int main(void) { RUN_TEST(test_name); REPORT(); }
 */
#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>
#include <string.h>

static int _tests_run = 0;
static int _tests_passed = 0;
static int _tests_failed = 0;
static const char *_current_test = NULL;

#define TEST(name) static void name(void)

#define RUN_TEST(name) do { \
    _current_test = #name; \
    _tests_run++; \
    int _prev_failed = _tests_failed; \
    name(); \
    if (_tests_failed == _prev_failed) { \
        _tests_passed++; \
        printf("  PASS: %s\n", #name); \
    } \
} while(0)

#define ASSERT(expr) do { \
    if (!(expr)) { \
        printf("  FAIL: %s — %s:%d: %s\n", _current_test, __FILE__, __LINE__, #expr); \
        _tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("  FAIL: %s — %s:%d: %s != %s\n", _current_test, __FILE__, __LINE__, #a, #b); \
        _tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ_INT(a, b) do { \
    int _a = (a), _b = (b); \
    if (_a != _b) { \
        printf("  FAIL: %s — %s:%d: %s = %d, expected %s = %d\n", \
               _current_test, __FILE__, __LINE__, #a, _a, #b, _b); \
        _tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if ((_a == NULL) != (_b == NULL) || (_a && _b && strcmp(_a, _b) != 0)) { \
        printf("  FAIL: %s — %s:%d: \"%s\" != \"%s\"\n", \
               _current_test, __FILE__, __LINE__, _a ? _a : "(null)", _b ? _b : "(null)"); \
        _tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_NULL(ptr) do { \
    if ((ptr) != NULL) { \
        printf("  FAIL: %s — %s:%d: %s is not NULL\n", _current_test, __FILE__, __LINE__, #ptr); \
        _tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_NOT_NULL(ptr) do { \
    if ((ptr) == NULL) { \
        printf("  FAIL: %s — %s:%d: %s is NULL\n", _current_test, __FILE__, __LINE__, #ptr); \
        _tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_TRUE(expr) ASSERT(expr)
#define ASSERT_FALSE(expr) ASSERT(!(expr))

#define REPORT() do { \
    printf("\n%d tests: %d passed, %d failed\n", _tests_run, _tests_passed, _tests_failed); \
    return _tests_failed > 0 ? 1 : 0; \
} while(0)

#endif /* TEST_HARNESS_H */
