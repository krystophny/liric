#include "../src/liric.h"
#include "../src/jit.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>

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

static int run_jit_i32(const char *src, const char *fname) {
    char err[256] = {0};
    lr_module_t *m = lr_parse_ll(src, strlen(src), err, sizeof(err));
    if (!m) { fprintf(stderr, "  parse error: %s\n", err); return -99999; }

    lr_jit_t *jit = lr_jit_create();
    if (!jit) { lr_module_free(m); return -99999; }

    if (lr_jit_add_module(jit, m) != 0) {
        lr_jit_destroy(jit);
        lr_module_free(m);
        return -99999;
    }

    typedef int (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, fname);
    if (!fn) {
        lr_jit_destroy(jit);
        lr_module_free(m);
        return -99999;
    }

    int result = fn();
    lr_jit_destroy(jit);
    lr_module_free(m);
    return result;
}

int test_e2e_ret_42(void) {
    const char *src = "define i32 @f() {\nentry:\n  ret i32 42\n}\n";
    int result = run_jit_i32(src, "f");
    TEST_ASSERT_EQ(result, 42, "ret 42");
    return 0;
}

int test_e2e_add_i32(void) {
    const char *src =
        "define i32 @f() {\n"
        "entry:\n"
        "  %a = add i32 10, 32\n"
        "  ret i32 %a\n"
        "}\n";
    int result = run_jit_i32(src, "f");
    TEST_ASSERT_EQ(result, 42, "10 + 32 = 42");
    return 0;
}

int test_e2e_branch(void) {
    const char *src =
        "define i32 @f() {\n"
        "entry:\n"
        "  %cmp = icmp sgt i32 5, 3\n"
        "  br i1 %cmp, label %then, label %else\n"
        "then:\n"
        "  ret i32 1\n"
        "else:\n"
        "  ret i32 0\n"
        "}\n";
    int result = run_jit_i32(src, "f");
    TEST_ASSERT_EQ(result, 1, "5 > 3 -> 1");
    return 0;
}

int test_e2e_loop(void) {
    const char *src =
        "define i32 @f() {\n"
        "entry:\n"
        "  br label %loop\n"
        "loop:\n"
        "  %i = phi i32 [0, %entry], [%next, %loop]\n"
        "  %sum = phi i32 [0, %entry], [%sum_next, %loop]\n"
        "  %next = add i32 %i, 1\n"
        "  %sum_next = add i32 %sum, %next\n"
        "  %done = icmp eq i32 %next, 10\n"
        "  br i1 %done, label %exit, label %loop\n"
        "exit:\n"
        "  ret i32 %sum_next\n"
        "}\n";
    int result = run_jit_i32(src, "f");
    TEST_ASSERT_EQ(result, 55, "sum 1..10 = 55");
    return 0;
}
