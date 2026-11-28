// test_check.h
// Minimal test helpers shared by the r5capture unit tests.
// CHECK keeps going after a failure so one run shows every problem.
#ifndef TEST_CHECK_H
#define TEST_CHECK_H

#include <stdio.h>

static int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                    \
        }                                                                  \
    } while (0)

// Prints the summary and returns the process exit code
static inline int TEST_finish(const char *name)
{
    if (failures == 0) {
        printf("%s: all checks passed\n", name);
        return 0;
    }
    printf("%s: %d check(s) failed\n", name, failures);
    return 1;
}

#endif
