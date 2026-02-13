#include "platform/platform_os.h"

#include <stdint.h>
#include <stdio.h>

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
        return 1; \
    } \
} while (0)

#define TEST_ASSERT_EQ(a, b, msg) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "  FAIL: %s: got %lld, expected %lld (line %d)\n", \
                msg, _a, _b, __LINE__); \
        return 1; \
    } \
} while (0)

int test_platform_jit_page_transitions(void) {
    bool map_jit = false;
    uint8_t *code = (uint8_t *)lr_platform_alloc_jit_code(4096, &map_jit);
    TEST_ASSERT(code != NULL, "alloc jit code");

    TEST_ASSERT_EQ(lr_platform_jit_make_writable(code, 4096, map_jit), 0,
                   "set writable");
    code[0] = 0xC3; /* ret */

    TEST_ASSERT_EQ(lr_platform_jit_make_executable(code, 4096, map_jit, code, code + 1), 0,
                   "set executable");
    TEST_ASSERT_EQ(lr_platform_jit_make_writable(code, 4096, map_jit), 0,
                   "set writable again");
    TEST_ASSERT_EQ(lr_platform_free_pages(code, 4096), 0, "free jit code");

    return 0;
}

int test_platform_time_ns_monotonic(void) {
    uint64_t t0 = lr_platform_time_ns();
    uint64_t t1 = lr_platform_time_ns();
    TEST_ASSERT(t0 > 0, "time ns returns non-zero");
    TEST_ASSERT(t1 >= t0, "monotonic time");
    return 0;
}

int test_platform_dlsym_default_malloc(void) {
#if defined(__unix__) || defined(__APPLE__)
    void *sym = lr_platform_dlsym_default("malloc");
    TEST_ASSERT(sym != NULL, "resolve malloc from process");
#endif
    return 0;
}

int test_platform_run_process_exit_status(void) {
#if defined(__unix__) || defined(__APPLE__)
    char *const ok_argv[] = { (char *)"sh", (char *)"-c", (char *)"exit 0", NULL };
    char *const fail_argv[] = { (char *)"sh", (char *)"-c", (char *)"exit 7", NULL };
    int status = -1;

    TEST_ASSERT_EQ(lr_platform_run_process(ok_argv, false, &status), 0,
                   "run process success");
    TEST_ASSERT_EQ(status, 0, "success exit code");
    TEST_ASSERT_EQ(lr_platform_run_process(fail_argv, false, &status), 0,
                   "run process failure");
    TEST_ASSERT_EQ(status, 7, "failure exit code");
#endif
    return 0;
}
