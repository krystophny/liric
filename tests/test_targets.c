#include "../src/jit.h"
#include "../src/liric.h"
#include "../src/target.h"
#include "../src/arena.h"
#include "../src/bc_decode.h"
#include <stdint.h>
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

static int code_contains_u32_le(const uint8_t *buf, size_t len, uint32_t word) {
    for (size_t i = 0; i + 4 <= len; i += 4) {
        uint32_t got = (uint32_t)buf[i] |
                       ((uint32_t)buf[i + 1] << 8) |
                       ((uint32_t)buf[i + 2] << 16) |
                       ((uint32_t)buf[i + 3] << 24);
        if (got == word)
            return 1;
    }
    return 0;
}

static int noop_compile_begin(void **compile_ctx,
                              const lr_compile_func_meta_t *func_meta,
                              lr_module_t *mod,
                              uint8_t *buf, size_t buflen,
                              lr_arena_t *arena) {
    (void)func_meta;
    (void)mod;
    (void)buf;
    (void)buflen;
    (void)arena;
    if (!compile_ctx)
        return -1;
    *compile_ctx = (void *)(uintptr_t)1;
    return 0;
}

static int noop_compile_emit(void *compile_ctx,
                             const lr_compile_inst_desc_t *inst_desc) {
    (void)compile_ctx;
    (void)inst_desc;
    return 0;
}

static int noop_compile_set_block(void *compile_ctx, uint32_t block_id) {
    (void)compile_ctx;
    (void)block_id;
    return 0;
}

static int noop_compile_end(void *compile_ctx, size_t *out_len) {
    if (!compile_ctx || !out_len)
        return -1;
    *out_len = 0;
    return 0;
}

int test_host_target_name(void) {
    const char *name = lr_jit_host_target_name();
    TEST_ASSERT(name != NULL, "host target name exists");
    TEST_ASSERT(name[0] != '\0', "host target name non-empty");
    TEST_ASSERT(strcmp(name, "x86_64") == 0 ||
                strcmp(name, "aarch64") == 0 ||
                strcmp(name, "riscv64gc") == 0 ||
                strcmp(name, "riscv64im") == 0,
                "host target is known");
    return 0;
}

int test_create_host_target(void) {
    const char *name = lr_jit_host_target_name();
    lr_jit_t *jit = lr_jit_create_for_target(name);
    TEST_ASSERT(jit != NULL, "create host target jit");

    const char *selected = lr_jit_target_name(jit);
    TEST_ASSERT(selected != NULL, "jit target name exists");
    TEST_ASSERT(strcmp(selected, name) == 0, "jit uses requested host target");

    lr_jit_destroy(jit);
    return 0;
}

int test_create_unknown_target_fails(void) {
    lr_jit_t *jit = lr_jit_create_for_target("unknown-target");
    TEST_ASSERT(jit == NULL, "unknown target rejected");
    return 0;
}

int test_non_host_target_fails(void) {
    const char *host = lr_jit_host_target_name();
    const char *other = strcmp(host, "x86_64") == 0 ? "aarch64" : "x86_64";

    lr_jit_t *jit = lr_jit_create_for_target(other);
    TEST_ASSERT(jit == NULL, "non-host target rejected");
    return 0;
}

int test_load_missing_runtime_library_fails(void) {
    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");
    int rc = lr_jit_load_library(jit, "/definitely/not/a/real/library/path.so");
    TEST_ASSERT(rc != 0, "missing library rejected");
    lr_jit_destroy(jit);
    return 0;
}

int test_target_alias_arm64_resolves(void) {
    const lr_target_t *canonical = lr_target_by_name("aarch64");
    const lr_target_t *alias = lr_target_by_name("arm64");
    TEST_ASSERT(canonical != NULL, "aarch64 target exists");
    TEST_ASSERT(alias != NULL, "arm64 alias exists");
    TEST_ASSERT(strcmp(canonical->name, alias->name) == 0, "arm64 alias maps to aarch64");
    return 0;
}

int test_target_riscv64_split_resolves(void) {
    const lr_target_t *def = lr_target_by_name("riscv64");
    const lr_target_t *gc = lr_target_by_name("riscv64gc");
    const lr_target_t *im = lr_target_by_name("riscv64im");
    const lr_target_t *rv64gc = lr_target_by_name("rv64gc");
    const lr_target_t *rv64im = lr_target_by_name("rv64im");

    TEST_ASSERT(def != NULL, "riscv64 target exists");
    TEST_ASSERT(gc != NULL, "riscv64gc target exists");
    TEST_ASSERT(im != NULL, "riscv64im target exists");
    TEST_ASSERT(rv64gc != NULL, "rv64gc alias exists");
    TEST_ASSERT(rv64im != NULL, "rv64im alias exists");

    TEST_ASSERT(strcmp(gc->name, "riscv64gc") == 0, "gc canonical target name");
    TEST_ASSERT(strcmp(im->name, "riscv64im") == 0, "im canonical target name");
    TEST_ASSERT(strcmp(rv64gc->name, gc->name) == 0, "rv64gc alias maps to riscv64gc");
    TEST_ASSERT(strcmp(rv64im->name, im->name) == 0, "rv64im alias maps to riscv64im");
    return 0;
}

int test_target_copy_patch_entrypoints_available(void) {
    const char *names[] = {"x86_64", "aarch64", "riscv64gc", "riscv64im"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const lr_target_t *t = lr_target_by_name(names[i]);
        TEST_ASSERT(t != NULL, "target exists");
        TEST_ASSERT(t->compile_begin != NULL, "target has compile_begin");
        TEST_ASSERT(t->compile_emit != NULL, "target has compile_emit");
        TEST_ASSERT(t->compile_set_block != NULL, "target has compile_set_block");
        TEST_ASSERT(t->compile_end != NULL, "target has compile_end");
        TEST_ASSERT(lr_target_can_compile(t, LR_COMPILE_ISEL), "target supports isel mode");
        TEST_ASSERT(lr_target_can_compile(t, LR_COMPILE_COPY_PATCH), "target supports copy_patch mode");
    }
    return 0;
}

int test_target_requires_full_streaming_hooks(void) {
    lr_target_t t;
    memset(&t, 0, sizeof(t));
    t.name = "stub";
    t.compile_begin = noop_compile_begin;
    t.compile_end = noop_compile_end;

    TEST_ASSERT(!lr_target_can_compile(&t, LR_COMPILE_ISEL),
                "target without emit/set_block hooks is rejected");

    t.compile_emit = noop_compile_emit;
    TEST_ASSERT(!lr_target_can_compile(&t, LR_COMPILE_ISEL),
                "target without set_block hook is rejected");

    t.compile_set_block = noop_compile_set_block;
    TEST_ASSERT(lr_target_can_compile(&t, LR_COMPILE_ISEL),
                "target with full streaming hooks supports isel");
    TEST_ASSERT(lr_target_can_compile(&t, LR_COMPILE_COPY_PATCH),
                "target with full streaming hooks supports copy_patch");
    TEST_ASSERT(!lr_target_can_compile(&t, LR_COMPILE_LLVM),
                "target compile contract rejects llvm mode");

    t.compile_emit = NULL;
    TEST_ASSERT(!lr_target_can_compile(&t, LR_COMPILE_COPY_PATCH),
                "target without emit hook is rejected");
    return 0;
}

int test_target_copy_patch_fallback_matches_isel_for_non_x86(void) {
    const char *src =
        "define i64 @sum(i64 %a, i64 %b) {\n"
        "entry:\n"
        "  %c = add i64 %a, %b\n"
        "  ret i64 %c\n"
        "}\n";
    const char *targets[] = {"aarch64", "riscv64gc", "riscv64im"};
    char err[256] = {0};
    lr_module_t *m = lr_parse_ll(src, strlen(src), err, sizeof(err));
    TEST_ASSERT(m != NULL, "parse module");
    TEST_ASSERT(m->first_func != NULL, "module has function");

    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        const lr_target_t *t = lr_target_by_name(targets[i]);
        lr_arena_t *a_isel = NULL;
        lr_arena_t *a_cp = NULL;
        uint8_t isel_buf[4096];
        uint8_t cp_buf[4096];
        size_t isel_len = 0;
        size_t cp_len = 0;
        int rc_isel;
        int rc_cp;

        TEST_ASSERT(t != NULL, "target exists");
        TEST_ASSERT(lr_target_can_compile(t, LR_COMPILE_ISEL), "target supports isel mode");
        TEST_ASSERT(lr_target_can_compile(t, LR_COMPILE_COPY_PATCH), "target supports copy_patch mode");

        a_isel = lr_arena_create(0);
        a_cp = lr_arena_create(0);
        TEST_ASSERT(a_isel != NULL && a_cp != NULL, "arena create");

        rc_isel = lr_target_compile(t, LR_COMPILE_ISEL, m->first_func, m,
                                    isel_buf, sizeof(isel_buf), &isel_len, a_isel);
        rc_cp = lr_target_compile(t, LR_COMPILE_COPY_PATCH, m->first_func, m,
                                  cp_buf, sizeof(cp_buf), &cp_len, a_cp);

        lr_arena_destroy(a_isel);
        lr_arena_destroy(a_cp);

        TEST_ASSERT(rc_isel == 0, "isel compile succeeds");
        TEST_ASSERT(rc_cp == 0, "copy-patch compile succeeds");
        TEST_ASSERT(isel_len == cp_len, "fallback length matches isel");
        TEST_ASSERT(memcmp(isel_buf, cp_buf, cp_len) == 0, "fallback bytes match isel");
    }

    lr_module_free(m);
    return 0;
}

int test_target_copy_patch_matches_isel_for_x86_streaming(void) {
#if !defined(__x86_64__) && !defined(_M_X64)
    return 0;
#else
    const char *src =
        "define i64 @mix(i64 %a, i64 %b) {\n"
        "entry:\n"
        "  %sum = add i64 %a, %b\n"
        "  %cmp = icmp sgt i64 %a, %b\n"
        "  %sel = select i1 %cmp, i64 %sum, i64 %b\n"
        "  ret i64 %sel\n"
        "}\n";
    const lr_target_t *t = lr_target_by_name("x86_64");
    char err[256] = {0};
    lr_module_t *m = lr_parse_ll(src, strlen(src), err, sizeof(err));
    lr_arena_t *a_isel = NULL;
    lr_arena_t *a_cp = NULL;
    uint8_t isel_buf[4096];
    uint8_t cp_buf[4096];
    size_t isel_len = 0;
    size_t cp_len = 0;
    int rc_isel;
    int rc_cp;

    TEST_ASSERT(t != NULL, "x86_64 target exists");
    TEST_ASSERT(m != NULL, "parse module");
    TEST_ASSERT(m->first_func != NULL, "module has function");
    TEST_ASSERT(lr_target_can_compile(t, LR_COMPILE_ISEL), "x86_64 supports isel");
    TEST_ASSERT(lr_target_can_compile(t, LR_COMPILE_COPY_PATCH), "x86_64 supports copy_patch");

    a_isel = lr_arena_create(0);
    a_cp = lr_arena_create(0);
    TEST_ASSERT(a_isel != NULL && a_cp != NULL, "arena create");

    rc_isel = lr_target_compile(t, LR_COMPILE_ISEL, m->first_func, m,
                                isel_buf, sizeof(isel_buf), &isel_len, a_isel);
    rc_cp = lr_target_compile(t, LR_COMPILE_COPY_PATCH, m->first_func, m,
                              cp_buf, sizeof(cp_buf), &cp_len, a_cp);

    lr_arena_destroy(a_isel);
    lr_arena_destroy(a_cp);

    TEST_ASSERT(rc_isel == 0, "isel compile succeeds");
    TEST_ASSERT(rc_cp == 0, "copy-patch compile succeeds");
    TEST_ASSERT(isel_len == cp_len, "copy_patch length matches isel");
    TEST_ASSERT(memcmp(isel_buf, cp_buf, cp_len) == 0,
                "copy_patch bytes match isel (streaming parity)");

    lr_module_free(m);
    return 0;
#endif
}

int test_target_x86_streaming_hooks_isel_smoke(void) {
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = NULL;
    const lr_target_t *t = lr_target_by_name("x86_64");
    lr_type_t *params[2];
    lr_compile_func_meta_t meta;
    lr_operand_desc_t add_ops[2];
    lr_operand_desc_t ret_ops[1];
    lr_compile_inst_desc_t add_desc;
    lr_compile_inst_desc_t ret_desc;
    void *compile_ctx = NULL;
    uint8_t code[4096];
    size_t code_len = 0;
    int rc;

    TEST_ASSERT(arena != NULL, "arena create");
    TEST_ASSERT(t != NULL, "x86_64 target exists");

    m = lr_module_create(arena);
    TEST_ASSERT(m != NULL, "module create");

    params[0] = m->type_i32;
    params[1] = m->type_i32;
    memset(&meta, 0, sizeof(meta));
    meta.ret_type = m->type_i32;
    meta.param_types = params;
    meta.num_params = 2;
    meta.next_vreg = 4;
    meta.mode = LR_COMPILE_ISEL;

    rc = t->compile_begin(&compile_ctx, &meta, m, code, sizeof(code), arena);
    TEST_ASSERT_EQ(rc, 0, "compile_begin succeeds");
    TEST_ASSERT(compile_ctx != NULL, "compile ctx exists");
    TEST_ASSERT_EQ(t->compile_set_block(compile_ctx, 0), 0, "set block 0");

    memset(add_ops, 0, sizeof(add_ops));
    add_ops[0].kind = LR_OP_KIND_VREG;
    add_ops[0].type = m->type_i32;
    add_ops[0].vreg = 1;
    add_ops[1].kind = LR_OP_KIND_VREG;
    add_ops[1].type = m->type_i32;
    add_ops[1].vreg = 2;
    memset(&add_desc, 0, sizeof(add_desc));
    add_desc.op = LR_OP_ADD;
    add_desc.type = m->type_i32;
    add_desc.dest = 3;
    add_desc.operands = add_ops;
    add_desc.num_operands = 2;
    TEST_ASSERT_EQ(t->compile_emit(compile_ctx, &add_desc), 0, "emit add");

    memset(ret_ops, 0, sizeof(ret_ops));
    ret_ops[0].kind = LR_OP_KIND_VREG;
    ret_ops[0].type = m->type_i32;
    ret_ops[0].vreg = 3;
    memset(&ret_desc, 0, sizeof(ret_desc));
    ret_desc.op = LR_OP_RET;
    ret_desc.type = m->type_i32;
    ret_desc.operands = ret_ops;
    ret_desc.num_operands = 1;
    TEST_ASSERT_EQ(t->compile_emit(compile_ctx, &ret_desc), 0, "emit ret");

    TEST_ASSERT_EQ(t->compile_end(compile_ctx, &code_len), 0, "compile_end succeeds");
    TEST_ASSERT(code_len > 0, "generated code");

    lr_arena_destroy(arena);
    return 0;
}

int test_target_x86_streaming_hooks_copy_patch_smoke(void) {
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = NULL;
    const lr_target_t *t = lr_target_by_name("x86_64");
    lr_type_t *params[2];
    lr_compile_func_meta_t meta;
    lr_operand_desc_t add_ops[2];
    lr_operand_desc_t ret_ops[1];
    lr_compile_inst_desc_t add_desc;
    lr_compile_inst_desc_t ret_desc;
    void *compile_ctx = NULL;
    uint8_t code[4096];
    size_t code_len = 0;
    int rc;

    TEST_ASSERT(arena != NULL, "arena create");
    TEST_ASSERT(t != NULL, "x86_64 target exists");

    m = lr_module_create(arena);
    TEST_ASSERT(m != NULL, "module create");

    params[0] = m->type_i32;
    params[1] = m->type_i32;
    memset(&meta, 0, sizeof(meta));
    meta.ret_type = m->type_i32;
    meta.param_types = params;
    meta.num_params = 2;
    meta.next_vreg = 4;
    meta.mode = LR_COMPILE_COPY_PATCH;

    rc = t->compile_begin(&compile_ctx, &meta, m, code, sizeof(code), arena);
    TEST_ASSERT_EQ(rc, 0, "compile_begin succeeds");
    TEST_ASSERT(compile_ctx != NULL, "compile ctx exists");
    TEST_ASSERT_EQ(t->compile_set_block(compile_ctx, 0), 0, "set block 0");

    memset(add_ops, 0, sizeof(add_ops));
    add_ops[0].kind = LR_OP_KIND_VREG;
    add_ops[0].type = m->type_i32;
    add_ops[0].vreg = 1;
    add_ops[1].kind = LR_OP_KIND_VREG;
    add_ops[1].type = m->type_i32;
    add_ops[1].vreg = 2;
    memset(&add_desc, 0, sizeof(add_desc));
    add_desc.op = LR_OP_ADD;
    add_desc.type = m->type_i32;
    add_desc.dest = 3;
    add_desc.operands = add_ops;
    add_desc.num_operands = 2;
    TEST_ASSERT_EQ(t->compile_emit(compile_ctx, &add_desc), 0, "emit add");

    memset(ret_ops, 0, sizeof(ret_ops));
    ret_ops[0].kind = LR_OP_KIND_VREG;
    ret_ops[0].type = m->type_i32;
    ret_ops[0].vreg = 3;
    memset(&ret_desc, 0, sizeof(ret_desc));
    ret_desc.op = LR_OP_RET;
    ret_desc.type = m->type_i32;
    ret_desc.operands = ret_ops;
    ret_desc.num_operands = 1;
    TEST_ASSERT_EQ(t->compile_emit(compile_ctx, &ret_desc), 0, "emit ret");

#if defined(__x86_64__) || defined(_M_X64)
    TEST_ASSERT_EQ(t->compile_end(compile_ctx, &code_len), 0, "compile_end succeeds");
    TEST_ASSERT(code_len > 0, "generated code");
#else
    TEST_ASSERT_EQ(t->compile_end(compile_ctx, &code_len), -1,
                   "compile_end reports unsupported x86 copy-patch backend");
#endif

    lr_arena_destroy(arena);
    return 0;
}

int test_target_x86_streaming_hooks_phi_smoke(void) {
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = NULL;
    const lr_target_t *t = lr_target_by_name("x86_64");
    lr_compile_func_meta_t meta;
    lr_operand_desc_t br_ops[1];
    lr_operand_desc_t phi_ops[2];
    lr_operand_desc_t ret_ops[1];
    lr_compile_inst_desc_t br_desc;
    lr_compile_inst_desc_t phi_desc;
    lr_compile_inst_desc_t ret_desc;
    void *compile_ctx = NULL;
    uint8_t code[4096];
    size_t code_len = 0;
    int rc;

    TEST_ASSERT(arena != NULL, "arena create");
    TEST_ASSERT(t != NULL, "x86_64 target exists");

    m = lr_module_create(arena);
    TEST_ASSERT(m != NULL, "module create");

    memset(&meta, 0, sizeof(meta));
    meta.ret_type = m->type_i32;
    meta.next_vreg = 2;
    meta.mode = LR_COMPILE_ISEL;

    rc = t->compile_begin(&compile_ctx, &meta, m, code, sizeof(code), arena);
    TEST_ASSERT_EQ(rc, 0, "compile_begin succeeds");
    TEST_ASSERT(compile_ctx != NULL, "compile ctx exists");
    TEST_ASSERT_EQ(t->compile_set_block(compile_ctx, 0), 0, "set block 0");

    memset(br_ops, 0, sizeof(br_ops));
    br_ops[0].kind = LR_OP_KIND_BLOCK;
    br_ops[0].block_id = 1;
    memset(&br_desc, 0, sizeof(br_desc));
    br_desc.op = LR_OP_BR;
    br_desc.type = m->type_void;
    br_desc.operands = br_ops;
    br_desc.num_operands = 1;
    TEST_ASSERT_EQ(t->compile_emit(compile_ctx, &br_desc), 0, "emit br");

    TEST_ASSERT_EQ(t->compile_set_block(compile_ctx, 1), 0, "set block 1");

    memset(phi_ops, 0, sizeof(phi_ops));
    phi_ops[0].kind = LR_OP_KIND_IMM_I64;
    phi_ops[0].type = m->type_i32;
    phi_ops[0].imm_i64 = 7;
    phi_ops[1].kind = LR_OP_KIND_BLOCK;
    phi_ops[1].block_id = 0;
    memset(&phi_desc, 0, sizeof(phi_desc));
    phi_desc.op = LR_OP_PHI;
    phi_desc.type = m->type_i32;
    phi_desc.dest = 1;
    phi_desc.operands = phi_ops;
    phi_desc.num_operands = 2;
    TEST_ASSERT_EQ(t->compile_emit(compile_ctx, &phi_desc), 0, "emit phi");

    memset(ret_ops, 0, sizeof(ret_ops));
    ret_ops[0].kind = LR_OP_KIND_VREG;
    ret_ops[0].type = m->type_i32;
    ret_ops[0].vreg = 1;
    memset(&ret_desc, 0, sizeof(ret_desc));
    ret_desc.op = LR_OP_RET;
    ret_desc.type = m->type_i32;
    ret_desc.operands = ret_ops;
    ret_desc.num_operands = 1;
    TEST_ASSERT_EQ(t->compile_emit(compile_ctx, &ret_desc), 0, "emit ret");

    TEST_ASSERT_EQ(t->compile_end(compile_ctx, &code_len), 0, "compile_end succeeds");
    TEST_ASSERT(code_len > 0, "generated code");

    lr_arena_destroy(arena);
    return 0;
}

int test_target_aarch64_streaming_hooks_smoke(void) {
    lr_arena_t *module_arena = lr_arena_create(0);
    lr_module_t *m = NULL;
    const lr_target_t *t = lr_target_by_name("aarch64");
    lr_type_t *params[2];
    lr_operand_desc_t add_ops[2];
    lr_operand_desc_t ret_ops[1];
    lr_compile_inst_desc_t add_desc;
    lr_compile_inst_desc_t ret_desc;
    lr_compile_mode_t modes[2] = {LR_COMPILE_ISEL, LR_COMPILE_COPY_PATCH};
    uint8_t isel_code[4096];
    uint8_t cp_code[4096];
    size_t isel_len = 0;
    size_t cp_len = 0;

    TEST_ASSERT(module_arena != NULL, "arena create");
    TEST_ASSERT(t != NULL, "aarch64 target exists");

    m = lr_module_create(module_arena);
    TEST_ASSERT(m != NULL, "module create");

    params[0] = m->type_i32;
    params[1] = m->type_i32;

    for (size_t i = 0; i < 2; i++) {
        lr_arena_t *compile_arena = lr_arena_create(0);
        lr_compile_func_meta_t meta;
        void *compile_ctx = NULL;
        uint8_t *out_buf = (i == 0) ? isel_code : cp_code;
        size_t *out_len = (i == 0) ? &isel_len : &cp_len;
        int rc;

        TEST_ASSERT(compile_arena != NULL, "compile arena create");

        memset(&meta, 0, sizeof(meta));
        meta.ret_type = m->type_i32;
        meta.param_types = params;
        meta.num_params = 2;
        meta.next_vreg = 4;
        meta.mode = modes[i];

        rc = t->compile_begin(&compile_ctx, &meta, m, out_buf, 4096, compile_arena);
        TEST_ASSERT_EQ(rc, 0, "compile_begin succeeds");
        TEST_ASSERT(compile_ctx != NULL, "compile ctx exists");
        TEST_ASSERT_EQ(t->compile_set_block(compile_ctx, 0), 0, "set block 0");

        memset(add_ops, 0, sizeof(add_ops));
        add_ops[0].kind = LR_OP_KIND_VREG;
        add_ops[0].type = m->type_i32;
        add_ops[0].vreg = 1;
        add_ops[1].kind = LR_OP_KIND_VREG;
        add_ops[1].type = m->type_i32;
        add_ops[1].vreg = 2;
        memset(&add_desc, 0, sizeof(add_desc));
        add_desc.op = LR_OP_ADD;
        add_desc.type = m->type_i32;
        add_desc.dest = 3;
        add_desc.operands = add_ops;
        add_desc.num_operands = 2;
        TEST_ASSERT_EQ(t->compile_emit(compile_ctx, &add_desc), 0, "emit add");

        memset(ret_ops, 0, sizeof(ret_ops));
        ret_ops[0].kind = LR_OP_KIND_VREG;
        ret_ops[0].type = m->type_i32;
        ret_ops[0].vreg = 3;
        memset(&ret_desc, 0, sizeof(ret_desc));
        ret_desc.op = LR_OP_RET;
        ret_desc.type = m->type_i32;
        ret_desc.operands = ret_ops;
        ret_desc.num_operands = 1;
        TEST_ASSERT_EQ(t->compile_emit(compile_ctx, &ret_desc), 0, "emit ret");

        TEST_ASSERT_EQ(t->compile_end(compile_ctx, out_len), 0, "compile_end succeeds");
        TEST_ASSERT(*out_len > 0, "generated code");

        lr_arena_destroy(compile_arena);
    }

    TEST_ASSERT(isel_len == cp_len, "copy-patch fallback length matches isel");
    TEST_ASSERT(memcmp(isel_code, cp_code, isel_len) == 0,
                "copy-patch fallback bytes match isel");

    lr_arena_destroy(module_arena);
    return 0;
}

int test_target_aarch64_streaming_fp_convert_ops(void) {
    lr_arena_t *module_arena = lr_arena_create(0);
    lr_module_t *m = NULL;
    const lr_target_t *t = lr_target_by_name("aarch64");
    lr_type_t *params[1];
    lr_operand_desc_t uitofp_ops[1];
    lr_operand_desc_t fptoui_ops[1];
    lr_operand_desc_t ret_ops[1];
    lr_compile_inst_desc_t uitofp_desc;
    lr_compile_inst_desc_t fptoui_desc;
    lr_compile_inst_desc_t ret_desc;
    lr_compile_mode_t modes[2] = {LR_COMPILE_ISEL, LR_COMPILE_COPY_PATCH};
    uint8_t isel_code[4096];
    uint8_t cp_code[4096];
    size_t isel_len = 0;
    size_t cp_len = 0;
    const uint32_t ucvtf_d0_x9 = 0x9E630120u;
    const uint32_t fcvtzu_x9_d0 = 0x9E790009u;

    TEST_ASSERT(module_arena != NULL, "arena create");
    TEST_ASSERT(t != NULL, "aarch64 target exists");

    m = lr_module_create(module_arena);
    TEST_ASSERT(m != NULL, "module create");

    params[0] = m->type_i32;

    for (size_t i = 0; i < 2; i++) {
        lr_arena_t *compile_arena = lr_arena_create(0);
        lr_compile_func_meta_t meta;
        void *compile_ctx = NULL;
        uint8_t *out_buf = (i == 0) ? isel_code : cp_code;
        size_t *out_len = (i == 0) ? &isel_len : &cp_len;
        int rc;

        TEST_ASSERT(compile_arena != NULL, "compile arena create");

        memset(&meta, 0, sizeof(meta));
        meta.ret_type = m->type_i32;
        meta.param_types = params;
        meta.num_params = 1;
        meta.next_vreg = 4;
        meta.mode = modes[i];

        rc = t->compile_begin(&compile_ctx, &meta, m, out_buf, 4096, compile_arena);
        TEST_ASSERT_EQ(rc, 0, "compile_begin succeeds");
        TEST_ASSERT(compile_ctx != NULL, "compile ctx exists");
        TEST_ASSERT_EQ(t->compile_set_block(compile_ctx, 0), 0, "set block 0");

        memset(uitofp_ops, 0, sizeof(uitofp_ops));
        uitofp_ops[0].kind = LR_OP_KIND_VREG;
        uitofp_ops[0].type = m->type_i32;
        uitofp_ops[0].vreg = 1;
        memset(&uitofp_desc, 0, sizeof(uitofp_desc));
        uitofp_desc.op = LR_OP_UITOFP;
        uitofp_desc.type = m->type_double;
        uitofp_desc.dest = 2;
        uitofp_desc.operands = uitofp_ops;
        uitofp_desc.num_operands = 1;
        TEST_ASSERT_EQ(t->compile_emit(compile_ctx, &uitofp_desc), 0, "emit uitofp");

        memset(fptoui_ops, 0, sizeof(fptoui_ops));
        fptoui_ops[0].kind = LR_OP_KIND_VREG;
        fptoui_ops[0].type = m->type_double;
        fptoui_ops[0].vreg = 2;
        memset(&fptoui_desc, 0, sizeof(fptoui_desc));
        fptoui_desc.op = LR_OP_FPTOUI;
        fptoui_desc.type = m->type_i32;
        fptoui_desc.dest = 3;
        fptoui_desc.operands = fptoui_ops;
        fptoui_desc.num_operands = 1;
        TEST_ASSERT_EQ(t->compile_emit(compile_ctx, &fptoui_desc), 0, "emit fptoui");

        memset(ret_ops, 0, sizeof(ret_ops));
        ret_ops[0].kind = LR_OP_KIND_VREG;
        ret_ops[0].type = m->type_i32;
        ret_ops[0].vreg = 3;
        memset(&ret_desc, 0, sizeof(ret_desc));
        ret_desc.op = LR_OP_RET;
        ret_desc.type = m->type_i32;
        ret_desc.operands = ret_ops;
        ret_desc.num_operands = 1;
        TEST_ASSERT_EQ(t->compile_emit(compile_ctx, &ret_desc), 0, "emit ret");

        TEST_ASSERT_EQ(t->compile_end(compile_ctx, out_len), 0, "compile_end succeeds");
        TEST_ASSERT(*out_len > 0, "generated code");
        TEST_ASSERT(code_contains_u32_le(out_buf, *out_len, ucvtf_d0_x9),
                    "contains ucvtf d0, x9");
        TEST_ASSERT(code_contains_u32_le(out_buf, *out_len, fcvtzu_x9_d0),
                    "contains fcvtzu x9, d0");

        lr_arena_destroy(compile_arena);
    }

    TEST_ASSERT(isel_len == cp_len, "copy-patch fallback length matches isel");
    TEST_ASSERT(memcmp(isel_code, cp_code, isel_len) == 0,
                "copy-patch fallback bytes match isel");

    lr_arena_destroy(module_arena);
    return 0;
}

int test_target_riscv64_streaming_hooks_smoke(void) {
    const char *targets[] = {"riscv64gc", "riscv64im"};

    for (size_t ti = 0; ti < sizeof(targets) / sizeof(targets[0]); ti++) {
        lr_arena_t *module_arena = lr_arena_create(0);
        lr_module_t *m = NULL;
        const lr_target_t *t = lr_target_by_name(targets[ti]);
        lr_type_t *params[2];
        lr_operand_desc_t add_ops[2];
        lr_operand_desc_t ret_ops[1];
        lr_compile_inst_desc_t add_desc;
        lr_compile_inst_desc_t ret_desc;
        lr_compile_mode_t modes[2] = {LR_COMPILE_ISEL, LR_COMPILE_COPY_PATCH};
        uint8_t isel_code[4096];
        uint8_t cp_code[4096];
        size_t isel_len = 0;
        size_t cp_len = 0;

        TEST_ASSERT(module_arena != NULL, "arena create");
        TEST_ASSERT(t != NULL, "riscv target exists");

        m = lr_module_create(module_arena);
        TEST_ASSERT(m != NULL, "module create");

        params[0] = m->type_i32;
        params[1] = m->type_i32;

        for (size_t i = 0; i < 2; i++) {
            lr_arena_t *compile_arena = lr_arena_create(0);
            lr_compile_func_meta_t meta;
            void *compile_ctx = NULL;
            uint8_t *out_buf = (i == 0) ? isel_code : cp_code;
            size_t *out_len = (i == 0) ? &isel_len : &cp_len;
            int rc;

            TEST_ASSERT(compile_arena != NULL, "compile arena create");

            memset(&meta, 0, sizeof(meta));
            meta.ret_type = m->type_i32;
            meta.param_types = params;
            meta.num_params = 2;
            meta.next_vreg = 4;
            meta.mode = modes[i];

            rc = t->compile_begin(&compile_ctx, &meta, m, out_buf, 4096, compile_arena);
            TEST_ASSERT_EQ(rc, 0, "compile_begin succeeds");
            TEST_ASSERT(compile_ctx != NULL, "compile ctx exists");
            TEST_ASSERT_EQ(t->compile_set_block(compile_ctx, 0), 0, "set block 0");

            memset(add_ops, 0, sizeof(add_ops));
            add_ops[0].kind = LR_OP_KIND_VREG;
            add_ops[0].type = m->type_i32;
            add_ops[0].vreg = 1;
            add_ops[1].kind = LR_OP_KIND_VREG;
            add_ops[1].type = m->type_i32;
            add_ops[1].vreg = 2;
            memset(&add_desc, 0, sizeof(add_desc));
            add_desc.op = LR_OP_ADD;
            add_desc.type = m->type_i32;
            add_desc.dest = 3;
            add_desc.operands = add_ops;
            add_desc.num_operands = 2;
            TEST_ASSERT_EQ(t->compile_emit(compile_ctx, &add_desc), 0, "emit add");

            memset(ret_ops, 0, sizeof(ret_ops));
            ret_ops[0].kind = LR_OP_KIND_VREG;
            ret_ops[0].type = m->type_i32;
            ret_ops[0].vreg = 3;
            memset(&ret_desc, 0, sizeof(ret_desc));
            ret_desc.op = LR_OP_RET;
            ret_desc.type = m->type_i32;
            ret_desc.operands = ret_ops;
            ret_desc.num_operands = 1;
            TEST_ASSERT_EQ(t->compile_emit(compile_ctx, &ret_desc), 0, "emit ret");

            TEST_ASSERT_EQ(t->compile_end(compile_ctx, out_len), 0, "compile_end succeeds");
            TEST_ASSERT(*out_len > 0, "generated code");

            lr_arena_destroy(compile_arena);
        }

        TEST_ASSERT(isel_len == cp_len, "copy-patch fallback length matches isel");
        TEST_ASSERT(memcmp(isel_code, cp_code, isel_len) == 0,
                    "copy-patch fallback bytes match isel");

        lr_arena_destroy(module_arena);
    }

    return 0;
}

int test_target_riscv64_streaming_reports_unsupported_ops(void) {
    const lr_opcode_t unsupported_ops[] = {
        LR_OP_ALLOCA,
        LR_OP_BR,
        LR_OP_CALL,
        LR_OP_CONDBR,
        LR_OP_EXTRACTVALUE,
        LR_OP_FCMP,
        LR_OP_FPTOUI,
        LR_OP_GEP,
        LR_OP_ICMP,
        LR_OP_INSERTVALUE,
        LR_OP_INTTOPTR,
        LR_OP_LOAD,
        LR_OP_PTRTOINT,
        LR_OP_SELECT,
        LR_OP_STORE,
        LR_OP_UITOFP,
        LR_OP_UNREACHABLE,
    };
    const char *targets[] = {"riscv64gc", "riscv64im"};

    for (size_t ti = 0; ti < sizeof(targets) / sizeof(targets[0]); ti++) {
        lr_arena_t *module_arena = lr_arena_create(0);
        lr_arena_t *compile_arena = lr_arena_create(0);
        lr_module_t *m = NULL;
        const lr_target_t *t = lr_target_by_name(targets[ti]);
        lr_compile_func_meta_t meta;
        void *compile_ctx = NULL;
        uint8_t code[4096];

        TEST_ASSERT(module_arena != NULL, "module arena create");
        TEST_ASSERT(compile_arena != NULL, "compile arena create");
        TEST_ASSERT(t != NULL, "riscv target exists");

        m = lr_module_create(module_arena);
        TEST_ASSERT(m != NULL, "module create");

        memset(&meta, 0, sizeof(meta));
        meta.ret_type = m->type_i32;
        meta.num_params = 0;
        meta.next_vreg = 8;
        meta.mode = LR_COMPILE_ISEL;

        TEST_ASSERT_EQ(t->compile_begin(&compile_ctx, &meta, m,
                                        code, sizeof(code), compile_arena),
                       0, "compile_begin succeeds");
        TEST_ASSERT(compile_ctx != NULL, "compile ctx exists");
        TEST_ASSERT_EQ(t->compile_set_block(compile_ctx, 0), 0, "set block 0");

        for (size_t oi = 0; oi < sizeof(unsupported_ops) / sizeof(unsupported_ops[0]); oi++) {
            lr_compile_inst_desc_t desc;
            int rc;

            memset(&desc, 0, sizeof(desc));
            desc.op = unsupported_ops[oi];
            desc.type = m->type_void;

            rc = t->compile_emit(compile_ctx, &desc);
            if (rc != -2) {
                fprintf(stderr,
                        "  FAIL: %s unsupported op %d returned %d, expected -2 (line %d)\n",
                        targets[ti], (int)unsupported_ops[oi], rc, __LINE__);
                lr_arena_destroy(compile_arena);
                lr_arena_destroy(module_arena);
                return 1;
            }
        }

        lr_arena_destroy(compile_arena);
        lr_arena_destroy(module_arena);
    }

    return 0;
}

int test_parse_auto_selects_ll_frontend(void) {
    const char *src =
        "define i32 @main() {\n"
        "entry:\n"
        "  ret i32 7\n"
        "}\n";
    char err[256] = {0};
    lr_module_t *m = lr_parse_auto((const uint8_t *)src, strlen(src), err, sizeof(err));
    TEST_ASSERT(m != NULL, "auto parser accepts LLVM IR text");
    TEST_ASSERT(m->first_func != NULL, "module has function");
    TEST_ASSERT(strcmp(m->first_func->name, "main") == 0, "parsed function name");
    lr_module_free(m);
    return 0;
}

int test_parse_auto_selects_wasm_frontend(void) {
    uint8_t wasm[] = {
        0x00, 0x61, 0x73, 0x6D,
        0x01, 0x00, 0x00, 0x00,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7F,
        0x03, 0x02, 0x01, 0x00,
        0x07, 0x05, 0x01, 0x01, 'f', 0x00, 0x00,
        0x0A, 0x06, 0x01, 0x04, 0x00, 0x41, 0x2A, 0x0B,
    };
    char err[256] = {0};
    lr_module_t *m = lr_parse_auto(wasm, sizeof(wasm), err, sizeof(err));
    TEST_ASSERT(m != NULL, "auto parser accepts WASM binary");
    TEST_ASSERT(m->first_func != NULL, "module has wasm function");
    TEST_ASSERT(strcmp(m->first_func->name, "f") == 0, "wasm export function name");
    lr_module_free(m);
    return 0;
}

int test_parse_auto_selects_bc_frontend(void) {
    const uint8_t bc_raw[] = {0x42, 0x43, 0xC0, 0xDE, 0x35, 0x14, 0x00, 0x00};
    if (lr_bc_parser_available()) {
        TEST_ASSERT(lr_bc_is_bitcode(bc_raw, sizeof(bc_raw)), "BC magic is detected");
        return 0;
    }

    char err[256] = {0};
    lr_module_t *m = lr_parse_auto(bc_raw, sizeof(bc_raw), err, sizeof(err));
    TEST_ASSERT(m == NULL, "invalid/truncated BC is rejected by BC frontend");
    TEST_ASSERT(strstr(err, "decoder support") != NULL, "error reports decoder support is unavailable");
    return 0;
}

static int fake_puts(const char *s) {
    (void)s;
    return 0;
}

int test_symbol_provider_prefers_jit_table(void) {
    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    lr_jit_add_symbol(jit, "puts", (void *)(uintptr_t)&fake_puts);
    void *sym = lr_jit_get_function(jit, "puts");
    TEST_ASSERT(sym == (void *)(uintptr_t)&fake_puts, "jit-table provider has precedence");

    lr_jit_destroy(jit);
    return 0;
}
