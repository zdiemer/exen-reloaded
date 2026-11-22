/* A tiny assertion framework for the exen-reloaded unit tests.
 *
 * Each test program defines test functions and lists them in main() with RUN(fn). A failed
 * CHECK prints the location and keeps going (so one run reports every failure); a failed
 * REQUIRE also returns from the current test function. TEST_MAIN_END returns non-zero if
 * anything failed, which is what CTest looks at.
 */
#ifndef EXEN_TEST_H
#define EXEN_TEST_H

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int t_failures;
static int t_checks;
static const char *t_current = "?";

#define T_FAIL(...)                                                                           \
    do {                                                                                      \
        t_failures++;                                                                         \
        fprintf(stderr, "%s:%d: [%s] ", __FILE__, __LINE__, t_current);                       \
        fprintf(stderr, __VA_ARGS__);                                                         \
        fputc('\n', stderr);                                                                  \
    } while (0)

#define CHECK(cond)                                                                           \
    do {                                                                                      \
        t_checks++;                                                                           \
        if (!(cond))                                                                          \
            T_FAIL("CHECK failed: %s", #cond);                                                \
    } while (0)

#define REQUIRE(cond)                                                                         \
    do {                                                                                      \
        t_checks++;                                                                           \
        if (!(cond)) {                                                                        \
            T_FAIL("REQUIRE failed: %s", #cond);                                              \
            return;                                                                           \
        }                                                                                     \
    } while (0)

/* Integer equality with both values printed. Evaluates each argument once. */
#define CHECK_EQ(a, b)                                                                        \
    do {                                                                                      \
        long long t_a_ = (long long)(a), t_b_ = (long long)(b);                               \
        t_checks++;                                                                           \
        if (t_a_ != t_b_)                                                                     \
            T_FAIL("CHECK_EQ failed: %s == %s (%lld vs %lld, 0x%llx vs 0x%llx)", #a, #b,      \
                   t_a_, t_b_, (unsigned long long)t_a_, (unsigned long long)t_b_);           \
    } while (0)

#define CHECK_STR(a, b)                                                                       \
    do {                                                                                      \
        const char *t_a_ = (a), *t_b_ = (b);                                                  \
        t_checks++;                                                                           \
        if (!t_a_ || !t_b_ || strcmp(t_a_, t_b_) != 0)                                        \
            T_FAIL("CHECK_STR failed: %s == %s (\"%s\" vs \"%s\")", #a, #b,                   \
                   t_a_ ? t_a_ : "(null)", t_b_ ? t_b_ : "(null)");                           \
    } while (0)

/* Substring check, for halt reasons and error messages. */
#define CHECK_CONTAINS(hay, needle)                                                           \
    do {                                                                                      \
        const char *t_h_ = (hay), *t_n_ = (needle);                                           \
        t_checks++;                                                                           \
        if (!t_h_ || !strstr(t_h_, t_n_))                                                     \
            T_FAIL("CHECK_CONTAINS failed: \"%s\" does not contain \"%s\"",                   \
                   t_h_ ? t_h_ : "(null)", t_n_);                                             \
    } while (0)

#define RUN(fn)                                                                               \
    do {                                                                                      \
        int t_before_ = t_failures;                                                           \
        t_current = #fn;                                                                      \
        fn();                                                                                 \
        printf("%-44s %s\n", #fn, t_failures == t_before_ ? "ok" : "FAILED");                 \
    } while (0)

#define TEST_MAIN_END()                                                                       \
    do {                                                                                      \
        printf("%d checks, %d failures\n", t_checks, t_failures);                             \
        return t_failures ? 1 : 0;                                                            \
    } while (0)

#endif
