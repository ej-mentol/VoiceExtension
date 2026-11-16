#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_testsRun = 0;
static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST_SUITE_START(name) \
    printf("============================================================\n"); \
    printf(" RUNNING SUITE: %s\n", name); \
    printf("============================================================\n");

#define TEST_SUITE_END() \
    printf("------------------------------------------------------------\n"); \
    printf(" SUMMARY: %d run, %d passed, %d failed\n", g_testsRun, g_testsPassed, g_testsFailed); \
    printf("============================================================\n"); \
    return (g_testsFailed == 0) ? 0 : 1;

#define ASSERT_TRUE(expr) do { \
    g_testsRun++; \
    if (expr) { \
        g_testsPassed++; \
    } else { \
        g_testsFailed++; \
        printf("  [FAIL] %s:%d: ASSERT_TRUE(%s)\n", __FILE__, __LINE__, #expr); \
    } \
} while(0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))
#define ASSERT_NE(a, b) ASSERT_TRUE((a) != (b))

#define ASSERT_EQ(a, b) do { \
    g_testsRun++; \
    if ((a) == (b)) { \
        g_testsPassed++; \
    } else { \
        g_testsFailed++; \
        printf("  [FAIL] %s:%d: ASSERT_EQ(%s, %s)\n", __FILE__, __LINE__, #a, #b); \
    } \
} while(0)

#define ASSERT_STREQ(a, b) do { \
    g_testsRun++; \
    if (strcmp((a), (b)) == 0) { \
        g_testsPassed++; \
    } else { \
        g_testsFailed++; \
        printf("  [FAIL] %s:%d: ASSERT_STREQ(\"%s\", \"%s\")\n", __FILE__, __LINE__, (a), (b)); \
    } \
} while(0)
