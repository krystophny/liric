#include "../src/arena.h"
#include "../src/ir.h"
#include "../src/ll_parser.h"
#include "../src/target.h"
#include <stdio.h>
#include <string.h>

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

int test_codegen_ret_42(void) {
    const char *src = "define i32 @f() {\nentry:\n  ret i32 42\n}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    const lr_target_t *target = lr_target_x86_64();
    lr_mfunc_t mf = {0};
    mf.arena = arena;

    int rc = target->isel_func(m->first_func, &mf, m);
    TEST_ASSERT_EQ(rc, 0, "isel succeeds");

    uint8_t code[4096];
    size_t code_len = 0;
    rc = target->encode_func(&mf, code, sizeof(code), &code_len);
    TEST_ASSERT_EQ(rc, 0, "encoding succeeds");
    TEST_ASSERT(code_len > 0, "generated some code");
    TEST_ASSERT(code_len < 100, "code is reasonably small");

    lr_arena_destroy(arena);
    return 0;
}

int test_codegen_add(void) {
    const char *src =
        "define i32 @add(i32 %a, i32 %b) {\n"
        "entry:\n"
        "  %c = add i32 %a, %b\n"
        "  ret i32 %c\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    const lr_target_t *target = lr_target_x86_64();
    lr_mfunc_t mf = {0};
    mf.arena = arena;

    int rc = target->isel_func(m->first_func, &mf, m);
    TEST_ASSERT_EQ(rc, 0, "isel succeeds");

    uint8_t code[4096];
    size_t code_len = 0;
    rc = target->encode_func(&mf, code, sizeof(code), &code_len);
    TEST_ASSERT_EQ(rc, 0, "encoding succeeds");
    TEST_ASSERT(code_len > 0, "generated some code");

    lr_arena_destroy(arena);
    return 0;
}
