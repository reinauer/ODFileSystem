/*
 * test_harness.h — minimal unit test framework
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Usage:
 *   TEST(test_name) { ... assertions ... }
 *   TEST_MAIN()   // generates main(), runs all registered tests
 *
 * Assertions:
 *   ASSERT(expr)
 *   ASSERT_EQ(a, b)
 *   ASSERT_NE(a, b)
 *   ASSERT_STR_EQ(a, b)
 *   ASSERT_OK(err)         // err == ODFS_OK
 *   ASSERT_ERR(err, code)  // err == code
 */

#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* test registry */
typedef void (*test_fn)(int *_fail_count);

typedef struct test_entry {
    const char *test_name;
    test_fn     fn;
} test_entry_t;

#define TEST_MAX_TESTS 512
static test_entry_t _tests[TEST_MAX_TESTS];
static int _test_count = 0;

#define TEST(name) \
    static void test_##name(int *_fail_count); \
    __attribute__((constructor)) static void _register_##name(void) { \
        _tests[_test_count].test_name = #name; \
        _tests[_test_count].fn = test_##name; \
        _test_count++; \
    } \
    static void test_##name(int *_fail_count)

#define ASSERT(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        (*_fail_count)++; \
        return; \
    } \
} while (0)

#define ASSERT_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "  FAIL: %s:%d: %s == %s (%lld != %lld)\n", \
                __FILE__, __LINE__, #a, #b, _a, _b); \
        (*_fail_count)++; \
        return; \
    } \
} while (0)

#define ASSERT_NE(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a == _b) { \
        fprintf(stderr, "  FAIL: %s:%d: %s != %s (both %lld)\n", \
                __FILE__, __LINE__, #a, #b, _a); \
        (*_fail_count)++; \
        return; \
    } \
} while (0)

#define ASSERT_STR_EQ(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b) != 0) { \
        fprintf(stderr, "  FAIL: %s:%d: \"%s\" != \"%s\"\n", \
                __FILE__, __LINE__, _a, _b); \
        (*_fail_count)++; \
        return; \
    } \
} while (0)

#define ASSERT_OK(err) ASSERT_EQ((err), ODFS_OK)
#define ASSERT_ERR(err, code) ASSERT_EQ((err), (code))

#define TEST_MAIN() \
int main(void) { \
    int passed = 0, failed = 0; \
    printf("Running %d test(s)...\n", _test_count); \
    for (int i = 0; i < _test_count; i++) { \
        int fails = 0; \
        _tests[i].fn(&fails); \
        if (fails == 0) { \
            printf("  PASS: %s\n", _tests[i].test_name); \
            passed++; \
        } else { \
            printf("  FAIL: %s\n", _tests[i].test_name); \
            failed++; \
        } \
    } \
    printf("\n%d passed, %d failed, %d total\n", passed, failed, _test_count); \
    return failed > 0 ? 1 : 0; \
}

#endif /* TEST_HARNESS_H */
