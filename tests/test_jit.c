#include "../src/arena.h"
#include "../src/ir.h"
#include "../src/ll_parser.h"
#include "../src/jit.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

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

int test_jit_ret_42(void) {
    const char *src = "define i32 @f() {\nentry:\n  ret i32 42\n}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "f");
    TEST_ASSERT(fn != NULL, "function lookup");

    int result = fn();
    TEST_ASSERT_EQ(result, 42, "f() returns 42");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_add_args(void) {
    const char *src =
        "define i32 @add(i32 %a, i32 %b) {\n"
        "entry:\n"
        "  %c = add i32 %a, %b\n"
        "  ret i32 %c\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int, int);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "add");
    TEST_ASSERT(fn != NULL, "function lookup");

    TEST_ASSERT_EQ(fn(10, 32), 42, "add(10, 32) == 42");
    TEST_ASSERT_EQ(fn(-5, 5), 0, "add(-5, 5) == 0");
    TEST_ASSERT_EQ(fn(0, 0), 0, "add(0, 0) == 0");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_arithmetic(void) {
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

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int, int);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "arith");
    TEST_ASSERT(fn != NULL, "function lookup");

    /* arith(3, 4) = (3+4)*4 - 3 = 28 - 3 = 25 */
    TEST_ASSERT_EQ(fn(3, 4), 25, "arith(3,4) == 25");
    /* arith(10, 2) = (10+2)*2 - 10 = 24 - 10 = 14 */
    TEST_ASSERT_EQ(fn(10, 2), 14, "arith(10,2) == 14");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_icmp(void) {
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

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int, int);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "max");
    TEST_ASSERT(fn != NULL, "function lookup");

    TEST_ASSERT_EQ(fn(5, 3), 5, "max(5,3) == 5");
    TEST_ASSERT_EQ(fn(3, 5), 5, "max(3,5) == 5");
    TEST_ASSERT_EQ(fn(7, 7), 7, "max(7,7) == 7");
    TEST_ASSERT_EQ(fn(-1, -5), -1, "max(-1,-5) == -1");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_branch(void) {
    const char *src =
        "define i32 @abs(i32 %x) {\n"
        "entry:\n"
        "  %cmp = icmp slt i32 %x, 0\n"
        "  br i1 %cmp, label %neg, label %pos\n"
        "neg:\n"
        "  %negx = sub i32 0, %x\n"
        "  br label %done\n"
        "pos:\n"
        "  br label %done\n"
        "done:\n"
        "  %r = phi i32 [%negx, %neg], [%x, %pos]\n"
        "  ret i32 %r\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "abs");
    TEST_ASSERT(fn != NULL, "function lookup");

    TEST_ASSERT_EQ(fn(5), 5, "abs(5) == 5");
    TEST_ASSERT_EQ(fn(-5), 5, "abs(-5) == 5");
    TEST_ASSERT_EQ(fn(0), 0, "abs(0) == 0");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_loop(void) {
    const char *src =
        "define i32 @sum(i32 %n) {\n"
        "entry:\n"
        "  br label %loop\n"
        "loop:\n"
        "  %i = phi i32 [0, %entry], [%i_next, %loop]\n"
        "  %acc = phi i32 [0, %entry], [%acc_next, %loop]\n"
        "  %i_next = add i32 %i, 1\n"
        "  %acc_next = add i32 %acc, %i_next\n"
        "  %done = icmp eq i32 %i_next, %n\n"
        "  br i1 %done, label %exit, label %loop\n"
        "exit:\n"
        "  ret i32 %acc_next\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "sum");
    TEST_ASSERT(fn != NULL, "function lookup");

    /* sum(10) = 1+2+...+10 = 55 */
    TEST_ASSERT_EQ(fn(10), 55, "sum(10) == 55");
    TEST_ASSERT_EQ(fn(1), 1, "sum(1) == 1");
    TEST_ASSERT_EQ(fn(100), 5050, "sum(100) == 5050");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_alloca_load_store(void) {
    const char *src =
        "define i32 @swap_add(i32 %a, i32 %b) {\n"
        "entry:\n"
        "  %pa = alloca i32\n"
        "  %pb = alloca i32\n"
        "  store i32 %a, ptr %pa\n"
        "  store i32 %b, ptr %pb\n"
        "  %va = load i32, ptr %pa\n"
        "  %vb = load i32, ptr %pb\n"
        "  %sum = add i32 %va, %vb\n"
        "  ret i32 %sum\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int, int);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "swap_add");
    TEST_ASSERT(fn != NULL, "function lookup");

    TEST_ASSERT_EQ(fn(10, 20), 30, "swap_add(10,20) == 30");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_forward_typed_call(void) {
    const char *src =
        "define i32 @f() {\n"
        ".entry:\n"
        "  %0 = call i32 () @g()\n"
        "  %1 = add i32 %0, 2\n"
        "  ret i32 %1\n"
        "}\n"
        "define i32 @g() {\n"
        ".entry:\n"
        "  ret i32 40\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "f");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 42, "forward typed call returns 42");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_fadd_double_bits(void) {
    const char *src =
        "define double @fadd64(double %a, double %b) {\n"
        "entry:\n"
        "  %c = fadd double %a, %b\n"
        "  ret double %c\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef uint64_t (*fn_t)(uint64_t, uint64_t);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "fadd64");
    TEST_ASSERT(fn != NULL, "function lookup");

    double a = 1.25;
    double b = 2.5;
    uint64_t a_bits = 0;
    uint64_t b_bits = 0;
    memcpy(&a_bits, &a, sizeof(a));
    memcpy(&b_bits, &b, sizeof(b));

    uint64_t out_bits = fn(a_bits, b_bits);
    double out = 0.0;
    memcpy(&out, &out_bits, sizeof(out));
    TEST_ASSERT(out > 3.74 && out < 3.76, "fadd64 result is 3.75");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_fmul_float_bits(void) {
    const char *src =
        "define float @fmul32(float %a, float %b) {\n"
        "entry:\n"
        "  %c = fmul float %a, %b\n"
        "  ret float %c\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef uint64_t (*fn_t)(uint64_t, uint64_t);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "fmul32");
    TEST_ASSERT(fn != NULL, "function lookup");

    float a = 3.5f;
    float b = 2.0f;
    uint32_t a_bits32 = 0;
    uint32_t b_bits32 = 0;
    memcpy(&a_bits32, &a, sizeof(a));
    memcpy(&b_bits32, &b, sizeof(b));

    uint64_t out_bits = fn((uint64_t)a_bits32, (uint64_t)b_bits32);
    uint32_t out_bits32 = (uint32_t)out_bits;
    float out = 0.0f;
    memcpy(&out, &out_bits32, sizeof(out));
    TEST_ASSERT(out > 6.99f && out < 7.01f, "fmul32 result is 7.0");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_phi_select_nested(void) {
    const char *src =
        "define i32 @score(i32 %x, i32 %y) {\n"
        "entry:\n"
        "  %cmpx = icmp sgt i32 %x, 0\n"
        "  br i1 %cmpx, label %pos, label %neg\n"
        "pos:\n"
        "  %a = add i32 %x, %y\n"
        "  br label %merge\n"
        "neg:\n"
        "  %a2 = sub i32 %y, %x\n"
        "  br label %merge\n"
        "merge:\n"
        "  %p = phi i32 [%a, %pos], [%a2, %neg]\n"
        "  %cmp2 = icmp sgt i32 %p, 10\n"
        "  %s = select i1 %cmp2, i32 %p, i32 10\n"
        "  ret i32 %s\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int, int);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "score");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(4, 3), 10, "score(4,3) == 10");
    TEST_ASSERT_EQ(fn(8, 5), 13, "score(8,5) == 13");
    TEST_ASSERT_EQ(fn(-2, 5), 10, "score(-2,5) == 10");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_phi_select_loop_carried(void) {
    const char *src =
        "define i32 @clamp_sum(i32 %n) {\n"
        "entry:\n"
        "  br label %loop\n"
        "loop:\n"
        "  %i = phi i32 [0, %entry], [%i1, %loop]\n"
        "  %acc = phi i32 [0, %entry], [%acc1, %loop]\n"
        "  %i1 = add i32 %i, 1\n"
        "  %raw = sub i32 %i1, 5\n"
        "  %is_neg = icmp slt i32 %raw, 0\n"
        "  %term = select i1 %is_neg, i32 0, i32 %raw\n"
        "  %acc1 = add i32 %acc, %term\n"
        "  %done = icmp eq i32 %i1, %n\n"
        "  br i1 %done, label %exit, label %loop\n"
        "exit:\n"
        "  ret i32 %acc1\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "clamp_sum");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(4), 0, "clamp_sum(4) == 0");
    TEST_ASSERT_EQ(fn(7), 3, "clamp_sum(7) == 3");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_internal_global_load_store(void) {
    const char *src =
        "@g = global i32 zeroinitializer\n"
        "define i32 @setget() {\n"
        "entry:\n"
        "  store i32 42, ptr @g\n"
        "  %v = load i32, ptr @g\n"
        "  ret i32 %v\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "setget");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 42, "internal global load/store");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_internal_global_address_relocation(void) {
    const char *src =
        "@buf = global [8 x i8] zeroinitializer\n"
        "define i64 @addr() {\n"
        "entry:\n"
        "  %p = ptrtoint ptr @buf to i64\n"
        "  ret i64 %p\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef uint64_t (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "addr");
    TEST_ASSERT(fn != NULL, "function lookup");
    uint64_t addr = fn();
    TEST_ASSERT(addr != 0, "internal global address is non-zero");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}
