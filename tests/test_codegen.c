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

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target exists");

    uint8_t code[4096];
    size_t code_len = 0;
    int rc = target->compile_func(m->first_func, m, code, sizeof(code), &code_len, arena);
    TEST_ASSERT_EQ(rc, 0, "compile succeeds");
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

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target exists");

    uint8_t code[4096];
    size_t code_len = 0;
    int rc = target->compile_func(m->first_func, m, code, sizeof(code), &code_len, arena);
    TEST_ASSERT_EQ(rc, 0, "compile succeeds");
    TEST_ASSERT(code_len > 0, "generated some code");

    lr_arena_destroy(arena);
    return 0;
}

static int has_immediate_store_reload_pair(const uint8_t *code, size_t code_len) {
    for (size_t i = 0; i + 7 < code_len; i++) {
        if (code[i + 0] == 0x48 && code[i + 1] == 0x89 && code[i + 2] == 0x45 &&
            code[i + 4] == 0x48 && code[i + 5] == 0x8B && code[i + 6] == 0x45 &&
            code[i + 3] == code[i + 7]) {
            return 1;
        }
    }
    for (size_t i = 0; i + 13 < code_len; i++) {
        if (code[i + 0] == 0x48 && code[i + 1] == 0x89 && code[i + 2] == 0x85 &&
            code[i + 7] == 0x48 && code[i + 8] == 0x8B && code[i + 9] == 0x85 &&
            memcmp(&code[i + 3], &code[i + 10], 4) == 0) {
            return 1;
        }
    }
    return 0;
}

static int count_rax_store_to_rbp(const uint8_t *code, size_t code_len) {
    int count = 0;
    for (size_t i = 0; i + 3 < code_len; i++) {
        if (code[i + 0] == 0x48 && code[i + 1] == 0x89 &&
            (code[i + 2] == 0x45 || code[i + 2] == 0x85)) {
            count++;
        }
    }
    return count;
}

static int has_xor_eax_eax(const uint8_t *code, size_t code_len) {
    for (size_t i = 0; i + 1 < code_len; i++) {
        if (code[i + 0] == 0x31 && code[i + 1] == 0xC0)
            return 1;
    }
    return 0;
}

static int has_mov_imm_zero_rax(const uint8_t *code, size_t code_len) {
    for (size_t i = 0; i + 6 < code_len; i++) {
        if (code[i + 0] == 0x48 && code[i + 1] == 0xC7 && code[i + 2] == 0xC0 &&
            code[i + 3] == 0x00 && code[i + 4] == 0x00 &&
            code[i + 5] == 0x00 && code[i + 6] == 0x00) {
            return 1;
        }
    }
    return 0;
}

int test_codegen_skip_redundant_immediate_reload(void) {
    const char *src =
        "define i64 @f(i64 %a, i64 %b, i64 %c) {\n"
        "entry:\n"
        "  %t = add i64 %a, %b\n"
        "  %u = mul i64 %t, %c\n"
        "  ret i64 %u\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target exists");
    if (strcmp(target->name, "x86_64") != 0) {
        lr_arena_destroy(arena);
        return 0;
    }

    uint8_t code[4096];
    size_t code_len = 0;
    int rc = target->compile_func(m->first_func, m, code, sizeof(code), &code_len, arena);
    TEST_ASSERT_EQ(rc, 0, "compile succeeds");
    TEST_ASSERT(code_len > 0, "generated some code");
    TEST_ASSERT(!has_immediate_store_reload_pair(code, code_len),
                "no immediate store+reload for same stack slot");
    TEST_ASSERT_EQ(count_rax_store_to_rbp(code, code_len), 1,
                   "single-use intermediate temporary avoids one RAX spill");

    lr_arena_destroy(arena);
    return 0;
}

int test_codegen_keep_store_for_next_inst_multiuse_vreg(void) {
    const char *src =
        "define i64 @f(i64 %a, i64 %b) {\n"
        "entry:\n"
        "  %t = add i64 %a, %b\n"
        "  %u = mul i64 %t, %t\n"
        "  ret i64 %u\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target exists");
    if (strcmp(target->name, "x86_64") != 0) {
        lr_arena_destroy(arena);
        return 0;
    }

    uint8_t code[4096];
    size_t code_len = 0;
    int rc = target->compile_func(m->first_func, m, code, sizeof(code), &code_len, arena);
    TEST_ASSERT_EQ(rc, 0, "compile succeeds");
    TEST_ASSERT(code_len > 0, "generated some code");
    TEST_ASSERT(count_rax_store_to_rbp(code, code_len) >= 1,
                "multi-use temporaries keep required stack spill");

    lr_arena_destroy(arena);
    return 0;
}

int test_codegen_zero_immediate_uses_xor_when_flags_dead(void) {
    const char *src =
        "define i64 @f() {\n"
        "entry:\n"
        "  ret i64 0\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target exists");
    if (strcmp(target->name, "x86_64") != 0) {
        lr_arena_destroy(arena);
        return 0;
    }

    uint8_t code[4096];
    size_t code_len = 0;
    int rc = target->compile_func(m->first_func, m, code, sizeof(code), &code_len, arena);
    TEST_ASSERT_EQ(rc, 0, "compile succeeds");
    TEST_ASSERT(code_len > 0, "generated some code");
    TEST_ASSERT(has_xor_eax_eax(code, code_len), "ret i64 0 uses xor zeroing");
    TEST_ASSERT(!has_mov_imm_zero_rax(code, code_len),
                "ret i64 0 avoids mov imm zero in dead-flags context");

    lr_arena_destroy(arena);
    return 0;
}

int test_codegen_select_zero_keeps_mov_for_flags(void) {
    const char *src =
        "define i64 @f(i64 %x) {\n"
        "entry:\n"
        "  %cond = icmp ne i64 %x, 0\n"
        "  %r = select i1 %cond, i64 7, i64 0\n"
        "  ret i64 %r\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    const lr_target_t *target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target exists");
    if (strcmp(target->name, "x86_64") != 0) {
        lr_arena_destroy(arena);
        return 0;
    }

    uint8_t code[4096];
    size_t code_len = 0;
    int rc = target->compile_func(m->first_func, m, code, sizeof(code), &code_len, arena);
    TEST_ASSERT_EQ(rc, 0, "compile succeeds");
    TEST_ASSERT(code_len > 0, "generated some code");
    TEST_ASSERT(has_mov_imm_zero_rax(code, code_len),
                "select keeps mov imm zero so condition flags stay intact");

    lr_arena_destroy(arena);
    return 0;
}
