#include "../src/arena.h"
#include "../src/ir.h"
#include "../src/ll_parser.h"
#include "../src/jit.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>

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

static int append_ir(char *buf, size_t cap, size_t *pos, const char *fmt, ...) {
    if (*pos >= cap)
        return -1;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *pos, cap - *pos, fmt, ap);
    va_end(ap);

    if (n < 0)
        return -1;
    if ((size_t)n >= cap - *pos)
        return -1;

    *pos += (size_t)n;
    return 0;
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

int test_jit_select_immediate_zero(void) {
    const char *src =
        "define i64 @pick(i64 %x) {\n"
        "entry:\n"
        "  %cond = icmp ne i64 %x, 0\n"
        "  %r = select i1 %cond, i64 7, i64 0\n"
        "  ret i64 %r\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef long long (*fn_t)(long long);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "pick");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(0), 0, "pick(0) == 0");
    TEST_ASSERT_EQ(fn(1), 7, "pick(1) == 7");
    TEST_ASSERT_EQ(fn(-3), 7, "pick(-3) == 7");

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

int test_jit_alloca_many_static_slots(void) {
    enum { NUM_ALLOCA = 256 };
    char src[65536];
    size_t pos = 0;

    TEST_ASSERT(append_ir(src, sizeof(src), &pos,
                          "define i32 @many_static_alloca() {\n"
                          "entry:\n") == 0, "emit test header");

    for (int i = 0; i < NUM_ALLOCA; i++) {
        TEST_ASSERT(append_ir(src, sizeof(src), &pos,
                              "  %%p%d = alloca i32\n", i) == 0, "emit alloca");
    }

    for (int i = 0; i < NUM_ALLOCA; i++) {
        TEST_ASSERT(append_ir(src, sizeof(src), &pos,
                              "  store i32 %d, ptr %%p%d\n", i, i) == 0, "emit store");
    }

    TEST_ASSERT(append_ir(src, sizeof(src), &pos,
                          "  %%v0 = load i32, ptr %%p0\n"
                          "  %%v1 = load i32, ptr %%p127\n"
                          "  %%v2 = load i32, ptr %%p255\n"
                          "  %%sum01 = add i32 %%v0, %%v1\n"
                          "  %%sum = add i32 %%sum01, %%v2\n"
                          "  ret i32 %%sum\n"
                          "}\n") == 0, "emit test body");

    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "many_static_alloca");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 382, "many static allocas keep distinct slots");

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

int test_jit_forward_call_chain(void) {
    enum { CHAIN_LEN = 64 };
    char src[32768];
    size_t pos = 0;

    for (int i = 0; i < CHAIN_LEN; i++) {
        TEST_ASSERT(append_ir(src, sizeof(src), &pos,
                              "define i32 @chain_%d() {\n"
                              "entry:\n"
                              "  %%v = call i32 @chain_%d()\n"
                              "  %%r = add i32 %%v, 1\n"
                              "  ret i32 %%r\n"
                              "}\n",
                              i, i + 1) == 0,
                    "emit forward chain function");
    }
    TEST_ASSERT(append_ir(src, sizeof(src), &pos,
                          "define i32 @chain_%d() {\n"
                          "entry:\n"
                          "  ret i32 0\n"
                          "}\n",
                          CHAIN_LEN) == 0,
                "emit chain leaf");

    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "chain_0");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), CHAIN_LEN, "forward chain computes expected depth");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_batched_module_updates(void) {
    const char *decl_src = "declare i32 @inc(i32)\n";
    const char *inc_src =
        "define i32 @inc(i32 %x) {\n"
        "entry:\n"
        "  %y = add i32 %x, 1\n"
        "  ret i32 %y\n"
        "}\n";
    const char *use_src =
        "declare i32 @inc(i32)\n"
        "define i32 @use_inc(i32 %x) {\n"
        "entry:\n"
        "  %a = call i32 @inc(i32 %x)\n"
        "  %b = call i32 @inc(i32 %a)\n"
        "  ret i32 %b\n"
        "}\n";

    lr_arena_t *decl_arena = lr_arena_create(0);
    lr_arena_t *inc_arena = lr_arena_create(0);
    lr_arena_t *use_arena = lr_arena_create(0);
    lr_module_t *decl_mod = parse(decl_src, decl_arena);
    lr_module_t *inc_mod = parse(inc_src, inc_arena);
    lr_module_t *use_mod = parse(use_src, use_arena);
    TEST_ASSERT(decl_mod != NULL, "parse declaration-only module");
    TEST_ASSERT(inc_mod != NULL, "parse definition module");
    TEST_ASSERT(use_mod != NULL, "parse use module");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    lr_jit_begin_update(jit);
    int rc = lr_jit_add_module(jit, decl_mod);
    TEST_ASSERT_EQ(rc, 0, "add declaration-only module in batch");
    rc = lr_jit_add_module(jit, inc_mod);
    TEST_ASSERT_EQ(rc, 0, "add definition module in batch");
    rc = lr_jit_add_module(jit, use_mod);
    TEST_ASSERT_EQ(rc, 0, "add use module in batch");
    lr_jit_end_update(jit);

    typedef int (*fn_t)(int);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "use_inc");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(40), 42, "batched module updates resolve cross-module calls");

    lr_jit_destroy(jit);
    lr_arena_destroy(use_arena);
    lr_arena_destroy(inc_arena);
    lr_arena_destroy(decl_arena);
    return 0;
}

int test_jit_self_recursive_call(void) {
    const char *src =
        "define i32 @sum_to_n(i32 %n) {\n"
        "entry:\n"
        "  %is_zero = icmp eq i32 %n, 0\n"
        "  br i1 %is_zero, label %base, label %rec\n"
        "base:\n"
        "  ret i32 0\n"
        "rec:\n"
        "  %n1 = sub i32 %n, 1\n"
        "  %tail = call i32 @sum_to_n(i32 %n1)\n"
        "  %sum = add i32 %n, %tail\n"
        "  ret i32 %sum\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "sum_to_n");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(0), 0, "sum_to_n(0) == 0");
    TEST_ASSERT_EQ(fn(1), 1, "sum_to_n(1) == 1");
    TEST_ASSERT_EQ(fn(5), 15, "sum_to_n(5) == 15");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

static int shadowed_sum_to_n_host(int n) {
    return 100000 + n;
}

int test_jit_self_recursive_call_ignores_prebound_symbol(void) {
    const char *src =
        "define i32 @shadowed_sum_to_n(i32 %n) {\n"
        "entry:\n"
        "  %is_zero = icmp eq i32 %n, 0\n"
        "  br i1 %is_zero, label %base, label %rec\n"
        "base:\n"
        "  ret i32 0\n"
        "rec:\n"
        "  %n1 = sub i32 %n, 1\n"
        "  %tail = call i32 @shadowed_sum_to_n(i32 %n1)\n"
        "  %sum = add i32 %n, %tail\n"
        "  ret i32 %sum\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");
    lr_jit_add_symbol(jit, "shadowed_sum_to_n",
                      (void *)(uintptr_t)&shadowed_sum_to_n_host);
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "shadowed_sum_to_n");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(0), 0, "shadowed_sum_to_n(0) == 0");
    TEST_ASSERT_EQ(fn(1), 1, "shadowed_sum_to_n(1) == 1");
    TEST_ASSERT_EQ(fn(5), 15, "shadowed_sum_to_n(5) == 15");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_unresolved_symbol_fails(void) {
    const char *src =
        "define i32 @f() {\n"
        "entry:\n"
        "  %0 = call i32 @missing()\n"
        "  ret i32 %0\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT(rc != 0, "jit add module fails for unresolved symbol");

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

int test_jit_external_call_abs(void) {
    const char *src =
        "declare i32 @abs(i32)\n"
        "define i32 @call_abs() {\n"
        "entry:\n"
        "  %r = call i32 @abs(i32 -5)\n"
        "  ret i32 %r\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");
    int (*abs_fn)(int) = abs;
    void *abs_addr = NULL;
    memcpy(&abs_addr, &abs_fn, sizeof(abs_addr));
    lr_jit_add_symbol(jit, "abs", abs_addr);

    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "call_abs");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 5, "external abs call returns 5");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_varargs_printf_call(void) {
    const char *src =
        "declare i32 @printf(ptr, ...)\n"
        "define i32 @call_printf() {\n"
        "entry:\n"
        "  %r = call i32 (ptr, ...) @printf(ptr @fmt, i32 7)\n"
        "  ret i32 %r\n"
        "}\n";
    static const char fmt[] = "v=%d\n";

    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    int (*printf_fn)(const char *, ...) = printf;
    void *printf_addr = NULL;
    memcpy(&printf_addr, &printf_fn, sizeof(printf_addr));
    lr_jit_add_symbol(jit, "printf", printf_addr);
    lr_jit_add_symbol(jit, "fmt", (void *)fmt);

    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "call_printf");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT(fn() > 0, "printf-style varargs call returns positive count");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_varargs_printf_double_call(void) {
    const char *src =
        "declare i32 @printf(ptr, ...)\n"
        "define i32 @call_printf_double() {\n"
        "entry:\n"
        "  %r = call i32 (ptr, ...) @printf(ptr @fmtf, double 1.5)\n"
        "  ret i32 %r\n"
        "}\n";
    static const char fmt[] = "vf=%f\n";

    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    int (*printf_fn)(const char *, ...) = printf;
    void *printf_addr = NULL;
    memcpy(&printf_addr, &printf_fn, sizeof(printf_addr));
    lr_jit_add_symbol(jit, "printf", printf_addr);
    lr_jit_add_symbol(jit, "fmtf", (void *)fmt);

    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "call_printf_double");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT(fn() > 0, "printf-style double varargs call returns positive count");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_const_gep_vtable_function_ptr(void) {
    const char *src =
        "@vt = private unnamed_addr constant { [3 x ptr] } { [3 x ptr] [ptr null, ptr bitcast (i32 (ptr)* @f to ptr), ptr bitcast (i32 (ptr)* @g to ptr)] }, align 8\n"
        "define i32 @f(ptr %this) {\n"
        "entry:\n"
        "  ret i32 7\n"
        "}\n"
        "define i32 @g(ptr %this) {\n"
        "entry:\n"
        "  ret i32 42\n"
        "}\n"
        "define i32 @call_vmethod() {\n"
        "entry:\n"
        "  %obj = alloca { ptr }, align 8\n"
        "  %slot = getelementptr { ptr }, ptr %obj, i32 0, i32 0\n"
        "  store ptr getelementptr inbounds ({ [3 x ptr] }, ptr @vt, i32 0, i32 0, i32 1), ptr %slot, align 8\n"
        "  %vptr = load ptr, ptr %slot, align 8\n"
        "  %meth_slot = getelementptr ptr, ptr %vptr, i32 1\n"
        "  %meth = load ptr, ptr %meth_slot, align 8\n"
        "  %r = call i32 %meth(ptr %obj)\n"
        "  ret i32 %r\n"
        "}\n";

    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "call_vmethod");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 42, "const-gep vtable call resolves to g()");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_llvm_intrinsic_fabs_f32(void) {
    const char *src =
        "declare float @llvm.fabs.f32(float)\n"
        "define i32 @call_fabs_bits() {\n"
        "entry:\n"
        "  %r = call float @llvm.fabs.f32(float -3.5)\n"
        "  %bits = bitcast float %r to i32\n"
        "  ret i32 %bits\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "call_fabs_bits");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 0x40600000, "fabs(-3.5f) bits");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_llvm_intrinsic_memcpy_memset(void) {
    const char *src =
        "declare void @llvm.memset.p0i8.i32(ptr, i8, i32, i1)\n"
        "declare void @llvm.memcpy.p0i8.p0i8.i32(ptr, ptr, i32, i1)\n"
        "define i32 @copy_fill() {\n"
        "entry:\n"
        "  %dst = alloca i32, align 4\n"
        "  %src = alloca i32, align 4\n"
        "  call void @llvm.memset.p0i8.i32(ptr %src, i8 65, i32 4, i1 false)\n"
        "  call void @llvm.memcpy.p0i8.p0i8.i32(ptr %dst, ptr %src, i32 4, i1 false)\n"
        "  %v = load i8, ptr %dst\n"
        "  %z = zext i8 %v to i32\n"
        "  ret i32 %z\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "copy_fill");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 65, "memset/memcpy wrappers set byte to 'A'");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_llvm_intrinsic_memmove(void) {
    const char *src =
        "declare void @llvm.memset.p0i8.i32(ptr, i8, i32, i1)\n"
        "declare void @llvm.memmove.p0i8.p0i8.i32(ptr, ptr, i32, i1)\n"
        "define i32 @move_fill() {\n"
        "entry:\n"
        "  %dst = alloca i32, align 4\n"
        "  %src = alloca i32, align 4\n"
        "  call void @llvm.memset.p0i8.i32(ptr %src, i8 90, i32 4, i1 false)\n"
        "  call void @llvm.memmove.p0i8.p0i8.i32(ptr %dst, ptr %src, i32 4, i1 false)\n"
        "  %v = load i8, ptr %dst\n"
        "  %z = zext i8 %v to i32\n"
        "  ret i32 %z\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "move_fill");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 90, "memmove wrapper copies byte value");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_gep_struct_field(void) {
    const char *src =
        "%my_struct = type <{ i32, i64 }>\n"
        "define i64 @gep_struct() {\n"
        "entry:\n"
        "  %s = alloca %my_struct, align 8\n"
        "  %p0 = getelementptr %my_struct, %my_struct* %s, i32 0, i32 0\n"
        "  store i32 10, i32* %p0, align 4\n"
        "  %p1 = getelementptr %my_struct, %my_struct* %s, i32 0, i32 1\n"
        "  store i64 32, i64* %p1, align 8\n"
        "  %v0 = load i32, i32* %p0, align 4\n"
        "  %v1 = load i64, i64* %p1, align 8\n"
        "  %ext = sext i32 %v0 to i64\n"
        "  %sum = add i64 %ext, %v1\n"
        "  ret i64 %sum\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int64_t (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "gep_struct");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 42, "struct field GEP: 10 + 32 = 42");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_gep_array_index(void) {
    const char *src =
        "define i64 @gep_array() {\n"
        "entry:\n"
        "  %arr = alloca [3 x i64], align 8\n"
        "  %p0 = getelementptr [3 x i64], [3 x i64]* %arr, i32 0, i32 0\n"
        "  store i64 10, i64* %p0, align 8\n"
        "  %p1 = getelementptr [3 x i64], [3 x i64]* %arr, i32 0, i32 1\n"
        "  store i64 20, i64* %p1, align 8\n"
        "  %p2 = getelementptr [3 x i64], [3 x i64]* %arr, i32 0, i32 2\n"
        "  store i64 12, i64* %p2, align 8\n"
        "  %v0 = load i64, i64* %p0, align 8\n"
        "  %v2 = load i64, i64* %p2, align 8\n"
        "  %sum = add i64 %v0, %v2\n"
        "  ret i64 %sum\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int64_t (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "gep_array");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 22, "array GEP: arr[0] + arr[2] = 10 + 12 = 22");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_gep_negative_i32_index(void) {
    const char *src =
        "define i64 @gep_negative_i32() {\n"
        "entry:\n"
        "  %arr = alloca [3 x i64], align 8\n"
        "  %p0 = getelementptr [3 x i64], [3 x i64]* %arr, i32 0, i32 0\n"
        "  %p1 = getelementptr [3 x i64], [3 x i64]* %arr, i32 0, i32 1\n"
        "  store i64 40, i64* %p0, align 8\n"
        "  store i64 2, i64* %p1, align 8\n"
        "  %idx = add i32 0, -1\n"
        "  %back = getelementptr i64, i64* %p1, i32 %idx\n"
        "  %v = load i64, i64* %back, align 8\n"
        "  ret i64 %v\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int64_t (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "gep_negative_i32");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 40, "GEP i32 index -1 must sign-extend");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_global_string_constant(void) {
    const char *src =
        "@hello = private unnamed_addr constant [5 x i8] c\"Hello\", align 1\n"
        "define i32 @read_char() {\n"
        "entry:\n"
        "  %p = getelementptr [5 x i8], [5 x i8]* @hello, i32 0, i32 0\n"
        "  %c = load i8, i8* %p, align 1\n"
        "  %v = zext i8 %c to i32\n"
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
    fn_t fn; LR_JIT_GET_FN(fn, jit, "read_char");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 72, "global string constant: 'H' = 72");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_global_struct_ptr_relocation(void) {
    /*
     * Exercises the string_descriptor pattern from lfortran:
     * a packed struct whose first field is a pointer (GEP) to another global,
     * and whose second field is the string length.
     * The function loads the pointer from the descriptor and reads the first byte.
     */
    const char *src =
        "%sd = type <{ ptr, i64 }>\n"
        "@str_data = private constant [5 x i8] c\"Hello\", align 1\n"
        "@str_desc = private global %sd <{ ptr getelementptr inbounds "
        "([5 x i8], [5 x i8]* @str_data, i32 0, i32 0), i64 5 }>, align 8\n"
        "define i64 @read_desc() {\n"
        "entry:\n"
        "  %pp = getelementptr %sd, %sd* @str_desc, i32 0, i32 0\n"
        "  %p = load ptr, ptr %pp, align 8\n"
        "  %c = load i8, i8* %p, align 1\n"
        "  %cv = zext i8 %c to i64\n"
        "  %lp = getelementptr %sd, %sd* @str_desc, i32 0, i32 1\n"
        "  %len = load i64, i64* %lp, align 8\n"
        "  %r = add i64 %cv, %len\n"
        "  ret i64 %r\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int64_t (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "read_desc");
    TEST_ASSERT(fn != NULL, "function lookup");
    /* 'H' (72) + length (5) = 77 */
    TEST_ASSERT_EQ(fn(), 77, "string descriptor: 'H' + len = 72 + 5 = 77");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_global_struct_integer_init(void) {
    /*
     * Exercises parsing integer fields in a struct initializer.
     * A packed struct with two i32 fields initialized to known values.
     */
    const char *src =
        "%pair = type <{ i32, i32 }>\n"
        "@vals = private global %pair <{ i32 10, i32 32 }>, align 4\n"
        "define i32 @read_pair() {\n"
        "entry:\n"
        "  %p0 = getelementptr %pair, %pair* @vals, i32 0, i32 0\n"
        "  %v0 = load i32, i32* %p0, align 4\n"
        "  %p1 = getelementptr %pair, %pair* @vals, i32 0, i32 1\n"
        "  %v1 = load i32, i32* %p1, align 4\n"
        "  %r = add i32 %v0, %v1\n"
        "  ret i32 %r\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "read_pair");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 42, "packed struct init: 10 + 32 = 42");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_aggregate_load_store_copy(void) {
    /*
     * Regression: aggregate values (>8 bytes) must not be truncated when
     * loaded into a vreg and stored back.
     */
    const char *src =
        "%pair = type <{ i64, i64 }>\n"
        "@vals = private global %pair <{ i64 10, i64 32 }>, align 8\n"
        "define i64 @copy_pair() {\n"
        "entry:\n"
        "  %v = load %pair, ptr @vals, align 1\n"
        "  %tmp = alloca %pair, align 8\n"
        "  store %pair %v, ptr %tmp, align 1\n"
        "  %p0 = getelementptr %pair, ptr %tmp, i32 0, i32 0\n"
        "  %a = load i64, ptr %p0, align 8\n"
        "  %p1 = getelementptr %pair, ptr %tmp, i32 0, i32 1\n"
        "  %b = load i64, ptr %p1, align 8\n"
        "  %r = add i64 %a, %b\n"
        "  ret i64 %r\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int64_t (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "copy_pair");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 42, "aggregate load/store copy preserves both fields");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_packed_struct_float_constant(void) {
    const char *src =
        "%complex_4 = type <{ float, float }>\n"
        "define i64 @test_complex() {\n"
        "entry:\n"
        "  %c = alloca %complex_4, align 4\n"
        "  store %complex_4 <{ float 3.0, float 4.0 }>, ptr %c, align 4\n"
        "  %p0 = getelementptr %complex_4, ptr %c, i32 0, i32 0\n"
        "  %re = load float, ptr %p0, align 4\n"
        "  %p1 = getelementptr %complex_4, ptr %c, i32 0, i32 1\n"
        "  %im = load float, ptr %p1, align 4\n"
        "  %re2 = fmul float %re, %re\n"
        "  %im2 = fmul float %im, %im\n"
        "  %sum = fadd float %re2, %im2\n"
        "  %res = fptosi float %sum to i64\n"
        "  ret i64 %res\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int64_t (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "test_complex");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 25, "3*3 + 4*4 = 25");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_packed_struct_double_constant(void) {
    const char *src =
        "%complex_8 = type <{ double, double }>\n"
        "define i64 @test_complex_d() {\n"
        "entry:\n"
        "  %c = alloca %complex_8, align 8\n"
        "  store %complex_8 <{ double 3.0, double 4.0 }>, ptr %c, align 8\n"
        "  %p0 = getelementptr %complex_8, ptr %c, i32 0, i32 0\n"
        "  %re = load double, ptr %p0, align 8\n"
        "  %p1 = getelementptr %complex_8, ptr %c, i32 0, i32 1\n"
        "  %im = load double, ptr %p1, align 8\n"
        "  %re2 = fmul double %re, %re\n"
        "  %im2 = fmul double %im, %im\n"
        "  %sum = fadd double %re2, %im2\n"
        "  %res = fptosi double %sum to i64\n"
        "  ret i64 %res\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int64_t (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "test_complex_d");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 25, "3*3 + 4*4 = 25 (double)");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

static int64_t sum8(int64_t a, int64_t b, int64_t c, int64_t d,
                    int64_t e, int64_t f, int64_t g, int64_t h) {
    return a + b + c + d + e + f + g + h;
}

int test_jit_call_stack_args(void) {
    const char *src =
        "declare i64 @sum8(i64, i64, i64, i64, i64, i64, i64, i64)\n"
        "define i64 @call_sum8() {\n"
        "entry:\n"
        "  %r = call i64 @sum8(i64 1, i64 2, i64 3, i64 4, i64 5, i64 6, i64 7, i64 8)\n"
        "  ret i64 %r\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    int64_t (*sum8_fn)(int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t, int64_t) = sum8;
    void *sum8_addr = NULL;
    memcpy(&sum8_addr, &sum8_fn, sizeof(sum8_addr));
    lr_jit_add_symbol(jit, "sum8", sum8_addr);

    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int64_t (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "call_sum8");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 36, "sum8(1..8) = 36 via stack args");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

static int64_t sum10(int64_t a, int64_t b, int64_t c, int64_t d,
                     int64_t e, int64_t f, int64_t g, int64_t h,
                     int64_t i, int64_t j) {
    return a + b + c + d + e + f + g + h + i + j;
}

int test_jit_call_many_stack_args(void) {
    const char *src =
        "declare i64 @sum10(i64, i64, i64, i64, i64, i64, i64, i64, i64, i64)\n"
        "define i64 @call_sum10() {\n"
        "entry:\n"
        "  %r = call i64 @sum10(i64 1, i64 2, i64 3, i64 4, i64 5,"
        " i64 6, i64 7, i64 8, i64 9, i64 10)\n"
        "  ret i64 %r\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    int64_t (*sum10_fn)(int64_t, int64_t, int64_t, int64_t, int64_t,
                        int64_t, int64_t, int64_t, int64_t, int64_t) = sum10;
    void *sum10_addr = NULL;
    memcpy(&sum10_addr, &sum10_fn, sizeof(sum10_addr));
    lr_jit_add_symbol(jit, "sum10", sum10_addr);

    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int64_t (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "call_sum10");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 55, "sum10(1..10) = 55 via stack args");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_fsub_double(void) {
    const char *src =
        "define double @fsub64(double %a, double %b) {\n"
        "entry:\n"
        "  %c = fsub double %a, %b\n"
        "  ret double %c\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef uint64_t (*fn_t)(uint64_t, uint64_t);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "fsub64");
    TEST_ASSERT(fn != NULL, "function lookup");

    double a = 10.5, b = 3.25;
    uint64_t a_bits = 0, b_bits = 0;
    memcpy(&a_bits, &a, sizeof(a));
    memcpy(&b_bits, &b, sizeof(b));

    uint64_t out_bits = fn(a_bits, b_bits);
    double out = 0.0;
    memcpy(&out, &out_bits, sizeof(out));
    TEST_ASSERT(out > 7.24 && out < 7.26, "fsub64 result is 7.25");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_fdiv_double(void) {
    const char *src =
        "define double @fdiv64(double %a, double %b) {\n"
        "entry:\n"
        "  %c = fdiv double %a, %b\n"
        "  ret double %c\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef uint64_t (*fn_t)(uint64_t, uint64_t);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "fdiv64");
    TEST_ASSERT(fn != NULL, "function lookup");

    double a = 15.0, b = 4.0;
    uint64_t a_bits = 0, b_bits = 0;
    memcpy(&a_bits, &a, sizeof(a));
    memcpy(&b_bits, &b, sizeof(b));

    uint64_t out_bits = fn(a_bits, b_bits);
    double out = 0.0;
    memcpy(&out, &out_bits, sizeof(out));
    TEST_ASSERT(out > 3.74 && out < 3.76, "fdiv64 result is 3.75");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_fneg_double(void) {
    const char *src =
        "define double @fneg64(double %a) {\n"
        "entry:\n"
        "  %c = fneg double %a\n"
        "  ret double %c\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef uint64_t (*fn_t)(uint64_t);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "fneg64");
    TEST_ASSERT(fn != NULL, "function lookup");

    double a = 42.5;
    uint64_t a_bits = 0;
    memcpy(&a_bits, &a, sizeof(a));

    uint64_t out_bits = fn(a_bits);
    double out = 0.0;
    memcpy(&out, &out_bits, sizeof(out));
    TEST_ASSERT(out < -42.49 && out > -42.51, "fneg64 result is -42.5");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_sitofp_i64_f64(void) {
    const char *src =
        "define double @i2d(i64 %x) {\n"
        "entry:\n"
        "  %d = sitofp i64 %x to double\n"
        "  ret double %d\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef uint64_t (*fn_t)(int64_t);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "i2d");
    TEST_ASSERT(fn != NULL, "function lookup");

    uint64_t out_bits = fn(42);
    double out = 0.0;
    memcpy(&out, &out_bits, sizeof(out));
    TEST_ASSERT(out > 41.99 && out < 42.01, "sitofp 42 -> 42.0");

    out_bits = fn(-7);
    memcpy(&out, &out_bits, sizeof(out));
    TEST_ASSERT(out < -6.99 && out > -7.01, "sitofp -7 -> -7.0");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_fptosi_f64_i64(void) {
    const char *src =
        "define i64 @d2i(double %x) {\n"
        "entry:\n"
        "  %i = fptosi double %x to i64\n"
        "  ret i64 %i\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int64_t (*fn_t)(uint64_t);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "d2i");
    TEST_ASSERT(fn != NULL, "function lookup");

    double val = 42.9;
    uint64_t val_bits = 0;
    memcpy(&val_bits, &val, sizeof(val));
    TEST_ASSERT_EQ(fn(val_bits), 42, "fptosi 42.9 -> 42");

    val = -7.1;
    memcpy(&val_bits, &val, sizeof(val));
    TEST_ASSERT_EQ(fn(val_bits), -7, "fptosi -7.1 -> -7");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_fpext_f32_f64(void) {
    const char *src =
        "define double @ext(float %x) {\n"
        "entry:\n"
        "  %d = fpext float %x to double\n"
        "  ret double %d\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef uint64_t (*fn_t)(uint64_t);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "ext");
    TEST_ASSERT(fn != NULL, "function lookup");

    float f = 3.5f;
    uint32_t f_bits = 0;
    memcpy(&f_bits, &f, sizeof(f));

    uint64_t out_bits = fn((uint64_t)f_bits);
    double out = 0.0;
    memcpy(&out, &out_bits, sizeof(out));
    TEST_ASSERT(out > 3.49 && out < 3.51, "fpext 3.5f -> 3.5");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_fptrunc_f64_f32(void) {
    const char *src =
        "define float @trunc(double %x) {\n"
        "entry:\n"
        "  %f = fptrunc double %x to float\n"
        "  ret float %f\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef uint64_t (*fn_t)(uint64_t);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "trunc");
    TEST_ASSERT(fn != NULL, "function lookup");

    double d = 2.75;
    uint64_t d_bits = 0;
    memcpy(&d_bits, &d, sizeof(d));

    uint64_t out_bits = fn(d_bits);
    uint32_t out_bits32 = (uint32_t)out_bits;
    float out = 0.0f;
    memcpy(&out, &out_bits32, sizeof(out));
    TEST_ASSERT(out > 2.74f && out < 2.76f, "fptrunc 2.75 -> 2.75f");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_fcmp_oeq(void) {
    const char *src =
        "define i1 @cmp_oeq(double %a, double %b) {\n"
        "entry:\n"
        "  %c = fcmp oeq double %a, %b\n"
        "  ret i1 %c\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef uint64_t (*fn_t)(uint64_t, uint64_t);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "cmp_oeq");
    TEST_ASSERT(fn != NULL, "function lookup");

    double a = 3.14, b = 3.14, c = 2.71;
    uint64_t a_bits = 0, b_bits = 0, c_bits = 0;
    memcpy(&a_bits, &a, sizeof(a));
    memcpy(&b_bits, &b, sizeof(b));
    memcpy(&c_bits, &c, sizeof(c));

    TEST_ASSERT_EQ(fn(a_bits, b_bits), 1, "3.14 oeq 3.14 = true");
    TEST_ASSERT_EQ(fn(a_bits, c_bits), 0, "3.14 oeq 2.71 = false");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_fp_arithmetic_chain(void) {
    const char *src =
        "define double @chain(double %a, double %b) {\n"
        "entry:\n"
        "  %sum = fadd double %a, %b\n"
        "  %prod = fmul double %sum, %a\n"
        "  %diff = fsub double %prod, %b\n"
        "  ret double %diff\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef uint64_t (*fn_t)(uint64_t, uint64_t);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "chain");
    TEST_ASSERT(fn != NULL, "function lookup");

    double a = 3.0, b = 2.0;
    uint64_t a_bits = 0, b_bits = 0;
    memcpy(&a_bits, &a, sizeof(a));
    memcpy(&b_bits, &b, sizeof(b));

    /* (3+2)*3 - 2 = 15 - 2 = 13 */
    uint64_t out_bits = fn(a_bits, b_bits);
    double out = 0.0;
    memcpy(&out, &out_bits, sizeof(out));
    TEST_ASSERT(out > 12.99 && out < 13.01, "chain(3,2) = 13.0");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_insert_extractvalue_struct_fields(void) {
    const char *src =
        "define i64 @ins_ext(i64 %x) {\n"
        "entry:\n"
        "  %ins0 = insertvalue { i64, i64 } undef, i64 11, 0\n"
        "  %ins1 = insertvalue { i64, i64 } %ins0, i64 %x, 1\n"
        "  %a = extractvalue { i64, i64 } %ins1, 0\n"
        "  %b = extractvalue { i64, i64 } %ins1, 1\n"
        "  %sum = add i64 %a, %b\n"
        "  ret i64 %sum\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int64_t (*fn_t)(int64_t);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "ins_ext");
    TEST_ASSERT(fn != NULL, "function lookup");

    TEST_ASSERT_EQ(fn(31), 42, "insert/extract keeps both fields");
    TEST_ASSERT_EQ(fn(-11), 0, "insert/extract signed value");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}

int test_jit_late_frame_patch_and_phi_slots(void) {
    const char *src =
        "define i32 @frame_phi(i32 %flag) {\n"
        "entry:\n"
        "  %is_zero = icmp eq i32 %flag, 0\n"
        "  br i1 %is_zero, label %fast, label %slow\n"
        "fast:\n"
        "  ret i32 7\n"
        "slow:\n"
        "  %is_one = icmp eq i32 %flag, 1\n"
        "  br i1 %is_one, label %s1, label %s2\n"
        "s1:\n"
        "  br label %join\n"
        "join:\n"
        "  %base = phi i64 [10, %s1], [20, %s2]\n"
        "  %p = alloca i64\n"
        "  store i64 %base, ptr %p\n"
        "  %v = load i64, ptr %p\n"
        "  %sum = add i64 %v, 32\n"
        "  %ret = trunc i64 %sum to i32\n"
        "  ret i32 %ret\n"
        "s2:\n"
        "  br label %join\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = parse(src, arena);
    TEST_ASSERT(m != NULL, "parse");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "frame_phi");
    TEST_ASSERT(fn != NULL, "function lookup");

    TEST_ASSERT_EQ(fn(0), 7, "fast return path");
    TEST_ASSERT_EQ(fn(1), 42, "phi path via s1");
    TEST_ASSERT_EQ(fn(2), 52, "phi path via late predecessor s2");

    lr_jit_destroy(jit);
    lr_arena_destroy(arena);
    return 0;
}
