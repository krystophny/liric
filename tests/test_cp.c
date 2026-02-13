#include "jit.h"
#include "ir.h"
#include "ll_parser.h"
#include <stdio.h>
#include <stdlib.h>
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

static lr_module_t *parse(const char *src, lr_arena_t *arena) {
    char err[256] = {0};
    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    if (!m) fprintf(stderr, "  parse error: %s\n", err);
    return m;
}

static size_t count_pattern(const uint8_t *buf, size_t buf_len,
                            const uint8_t *pat, size_t pat_len) {
    size_t count = 0;
    size_t i;
    if (!buf || !pat || pat_len == 0 || buf_len < pat_len)
        return 0;
    for (i = 0; i + pat_len <= buf_len; i++) {
        if (memcmp(buf + i, pat, pat_len) == 0)
            count++;
    }
    return count;
}

/*
 * Create a JIT instance in copy-and-patch mode by temporarily setting
 * the LIRIC_COMPILE_MODE env var.
 */
static lr_jit_t *create_cp_jit(void) {
    const char *old = getenv("LIRIC_COMPILE_MODE");
    char saved[64] = {0};
    if (old) {
        size_t n = strlen(old);
        if (n < sizeof(saved)) memcpy(saved, old, n + 1);
    }
    setenv("LIRIC_COMPILE_MODE", "copy_patch", 1);
    lr_jit_t *jit = lr_jit_create();
    if (saved[0])
        setenv("LIRIC_COMPILE_MODE", saved, 1);
    else
        unsetenv("LIRIC_COMPILE_MODE");
    return jit;
}

int test_cp_add_i32(void) {
    const char *src =
        "define i32 @add(i32 %a, i32 %b) {\n"
        "entry:\n"
        "  %c = add i32 %a, %b\n"
        "  ret i32 %c\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = create_cp_jit();
    TEST_ASSERT(jit != NULL, "create jit");
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int, int);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "add");
    TEST_ASSERT(fn != NULL, "function lookup");

    TEST_ASSERT_EQ(fn(10, 32), 42, "add(10, 32)");
    TEST_ASSERT_EQ(fn(-5, 5), 0, "add(-5, 5)");
    TEST_ASSERT_EQ(fn(0, 0), 0, "add(0, 0)");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_cp_arithmetic_chain_i32(void) {
    const char *src =
        "define i32 @arith(i32 %a, i32 %b) {\n"
        "entry:\n"
        "  %sum = add i32 %a, %b\n"
        "  %prod = mul i32 %sum, %b\n"
        "  %diff = sub i32 %prod, %a\n"
        "  ret i32 %diff\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = create_cp_jit();
    TEST_ASSERT(jit != NULL, "create jit");
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int, int);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "arith");
    TEST_ASSERT(fn != NULL, "function lookup");

    /* arith(3, 4) = (3+4)*4 - 3 = 25 */
    TEST_ASSERT_EQ(fn(3, 4), 25, "arith(3,4)");
    /* arith(10, 2) = (10+2)*2 - 10 = 14 */
    TEST_ASSERT_EQ(fn(10, 2), 14, "arith(10,2)");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_cp_all_alu_ops_i64(void) {
    const char *src =
        "define i64 @alu(i64 %a, i64 %b) {\n"
        "entry:\n"
        "  %add = add i64 %a, %b\n"
        "  %sub = sub i64 %add, %b\n"
        "  %and = and i64 %sub, %a\n"
        "  %or  = or  i64 %and, %b\n"
        "  %xor = xor i64 %or, %a\n"
        "  ret i64 %xor\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = create_cp_jit();
    TEST_ASSERT(jit != NULL, "create jit");
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef long long (*fn_t)(long long, long long);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "alu");
    TEST_ASSERT(fn != NULL, "function lookup");

    /* a=0xFF, b=0x0F:
     * add = 0x10E, sub = 0xFF, and = 0xFF, or = 0xFF, xor = 0x00 */
    TEST_ASSERT_EQ(fn(0xFF, 0x0F), 0x00, "alu(0xFF, 0x0F)");

    /* a=7, b=3:
     * add = 10, sub = 7, and = 7, or = 7, xor = 0 */
    TEST_ASSERT_EQ(fn(7, 3), 0, "alu(7, 3)");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_cp_shift_ops(void) {
    const char *src =
        "define i64 @shift(i64 %a, i64 %b) {\n"
        "entry:\n"
        "  %shl = shl i64 %a, %b\n"
        "  %lshr = lshr i64 %shl, %b\n"
        "  ret i64 %lshr\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = create_cp_jit();
    TEST_ASSERT(jit != NULL, "create jit");
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef long long (*fn_t)(long long, long long);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "shift");
    TEST_ASSERT(fn != NULL, "function lookup");

    TEST_ASSERT_EQ(fn(42, 3), 42, "shift(42, 3)");
    TEST_ASSERT_EQ(fn(1, 0), 1, "shift(1, 0)");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_cp_sdiv_srem(void) {
    const char *src =
        "define i64 @divmod(i64 %a, i64 %b) {\n"
        "entry:\n"
        "  %q = sdiv i64 %a, %b\n"
        "  %r = srem i64 %a, %b\n"
        "  %result = add i64 %q, %r\n"
        "  ret i64 %result\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = create_cp_jit();
    TEST_ASSERT(jit != NULL, "create jit");
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef long long (*fn_t)(long long, long long);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "divmod");
    TEST_ASSERT(fn != NULL, "function lookup");

    /* divmod(17, 5) = 17/5 + 17%5 = 3 + 2 = 5 */
    TEST_ASSERT_EQ(fn(17, 5), 5, "divmod(17, 5)");
    /* divmod(-17, 5) = -3 + (-2) = -5 */
    TEST_ASSERT_EQ(fn(-17, 5), -5, "divmod(-17, 5)");
    /* divmod(100, 10) = 10 + 0 = 10 */
    TEST_ASSERT_EQ(fn(100, 10), 10, "divmod(100, 10)");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_cp_fallback_to_isel(void) {
    /* icmp + select aren't supported by C&P; it should fall back to ISel. */
    const char *src =
        "define i32 @max(i32 %a, i32 %b) {\n"
        "entry:\n"
        "  %cmp = icmp sgt i32 %a, %b\n"
        "  %r = select i1 %cmp, i32 %a, i32 %b\n"
        "  ret i32 %r\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = create_cp_jit();
    TEST_ASSERT(jit != NULL, "create jit");
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int, int);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "max");
    TEST_ASSERT(fn != NULL, "function lookup (fallback to ISel)");

    TEST_ASSERT_EQ(fn(10, 5), 10, "max(10, 5)");
    TEST_ASSERT_EQ(fn(3, 7), 7, "max(3, 7)");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_cp_immediate_operand(void) {
    const char *src =
        "define i32 @add_imm(i32 %a) {\n"
        "entry:\n"
        "  %r = add i32 %a, 100\n"
        "  ret i32 %r\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = create_cp_jit();
    TEST_ASSERT(jit != NULL, "create jit");
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "add_imm");
    TEST_ASSERT(fn != NULL, "function lookup");

    TEST_ASSERT_EQ(fn(42), 142, "add_imm(42)");
    TEST_ASSERT_EQ(fn(-100), 0, "add_imm(-100)");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_cp_add_ret_supernode_i32(void) {
    const char *fused_src =
        "define i32 @fused(i32 %a, i32 %b) {\n"
        "entry:\n"
        "  %c = add i32 %a, %b\n"
        "  ret i32 %c\n"
        "}\n";
    const uint8_t mov_rdi_rbp[] = {0x48, 0x89, 0xEF};
    size_t code_off = 0;
    size_t code_len = 0;
    uint8_t *start = NULL;

    lr_arena_t *arena_fused = lr_arena_create(0);
    lr_module_t *m_fused = parse(fused_src, arena_fused);
    TEST_ASSERT(m_fused != NULL, "parse fused");

    lr_jit_t *jit_fused = create_cp_jit();
    TEST_ASSERT(jit_fused != NULL, "create fused jit");
    TEST_ASSERT_EQ(lr_jit_add_module(jit_fused, m_fused), 0, "add fused module");

    {
        typedef int (*fn_t)(int, int);
        fn_t fn;
        LR_JIT_GET_FN(fn, jit_fused, "fused");
        TEST_ASSERT(fn != NULL, "lookup fused");
        TEST_ASSERT_EQ(fn(20, 22), 42, "fused(20,22)");
    }
    start = (uint8_t *)lr_jit_get_function(jit_fused, "fused");
    TEST_ASSERT(start != NULL, "fused code start");
    code_off = (size_t)(start - jit_fused->code_buf);
    TEST_ASSERT(code_off < jit_fused->code_size, "fused code offset");
    code_len = jit_fused->code_size - code_off;
    TEST_ASSERT(count_pattern(start, code_len, mov_rdi_rbp, sizeof(mov_rdi_rbp)) == 0,
                "fused add+ret bypasses stencil move setup");

    lr_jit_destroy(jit_fused);
    lr_arena_destroy(arena_fused);
    return 0;
}

int test_cp_add_ret_supernode_i64(void) {
    const char *src =
        "define i64 @fused64(i64 %a, i64 %b) {\n"
        "entry:\n"
        "  %c = add i64 %a, %b\n"
        "  ret i64 %c\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = create_cp_jit();
    TEST_ASSERT(jit != NULL, "create jit");
    TEST_ASSERT_EQ(lr_jit_add_module(jit, m), 0, "jit add module");

    {
        const uint8_t store_rax_dest[] = {0x48, 0x89, 0x85};
        size_t code_off = 0;
        size_t code_len = 0;
        uint8_t *start = NULL;
        typedef long long (*fn_t)(long long, long long);
        fn_t fn;
        LR_JIT_GET_FN(fn, jit, "fused64");
        TEST_ASSERT(fn != NULL, "function lookup");
        TEST_ASSERT_EQ(fn(40, 2), 42, "fused64(40,2)");
        TEST_ASSERT_EQ(fn(-2, 2), 0, "fused64(-2,2)");
        start = (uint8_t *)lr_jit_get_function(jit, "fused64");
        TEST_ASSERT(start != NULL, "fused64 code start");
        code_off = (size_t)(start - jit->code_buf);
        TEST_ASSERT(code_off < jit->code_size, "fused64 code offset");
        code_len = jit->code_size - code_off;
        TEST_ASSERT(count_pattern(start, code_len, store_rax_dest, sizeof(store_rax_dest)) == 0,
                    "fused64 add+ret omits intermediate stack store");
    }

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}
