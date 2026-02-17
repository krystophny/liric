#include "../src/arena.h"
#include "../src/ir.h"
#include "../src/jit.h"
#include "../src/ll_parser.h"
#include "../src/objfile.h"
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
    int rc = lr_target_compile(target, LR_COMPILE_ISEL, m->first_func, m, code, sizeof(code), &code_len, arena);
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
    int rc = lr_target_compile(target, LR_COMPILE_ISEL, m->first_func, m, code, sizeof(code), &code_len, arena);
    TEST_ASSERT_EQ(rc, 0, "compile succeeds");
    TEST_ASSERT(code_len > 0, "generated some code");

    lr_arena_destroy(arena);
    return 0;
}

static int operand_to_desc_codegen(const lr_operand_t *op,
                                   lr_operand_desc_t *out) {
    if (!op || !out)
        return -1;
    memset(out, 0, sizeof(*out));
    out->type = op->type;
    out->global_offset = op->global_offset;
    switch (op->kind) {
    case LR_VAL_VREG:
        out->kind = LR_OP_KIND_VREG;
        out->vreg = op->vreg;
        return 0;
    case LR_VAL_IMM_I64:
        out->kind = LR_OP_KIND_IMM_I64;
        out->imm_i64 = op->imm_i64;
        return 0;
    case LR_VAL_IMM_F64:
        out->kind = LR_OP_KIND_IMM_F64;
        out->imm_f64 = op->imm_f64;
        return 0;
    case LR_VAL_BLOCK:
        out->kind = LR_OP_KIND_BLOCK;
        out->block_id = op->block_id;
        return 0;
    case LR_VAL_GLOBAL:
        out->kind = LR_OP_KIND_GLOBAL;
        out->global_id = op->global_id;
        return 0;
    case LR_VAL_NULL:
        out->kind = LR_OP_KIND_NULL;
        return 0;
    case LR_VAL_UNDEF:
        out->kind = LR_OP_KIND_UNDEF;
        return 0;
    default:
        return -1;
    }
}

int test_codegen_x86_global_reloc_uses_abs64_when_jit_and_objctx(void) {
    const char *src =
        "@g = external global i64\n"
        "define i64 @f(i64 %x) {\n"
        "entry:\n"
        "  store i64 %x, ptr @g\n"
        "  %v = load i64, ptr @g\n"
        "  ret i64 %v\n"
        "}\n";
    const lr_target_t *target;
    lr_arena_t *arena;
    lr_module_t *m;
    lr_func_t *f;
    lr_objfile_ctx_t obj_ctx;
    lr_jit_t *jit;
    lr_compile_func_meta_t meta;
    void *compile_ctx = NULL;
    size_t code_len = 0;
    char err[256] = {0};
    int rc;
    uint32_t abs64_for_g = 0;
    uint32_t disallowed_for_g = 0;
    const uintptr_t far_addr = (uintptr_t)0x700000000000ULL;

    target = lr_target_host();
    TEST_ASSERT(target != NULL, "host target exists");
    if (strcmp(target->name, "x86_64") != 0)
        return 0;

    arena = lr_arena_create(0);
    TEST_ASSERT(arena != NULL, "arena create");

    m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);
    f = m->first_func;
    TEST_ASSERT(f != NULL, "parsed function exists");
    TEST_ASSERT(lr_func_finalize(f, arena) == 0, "func finalize succeeds");

    memset(&obj_ctx, 0, sizeof(obj_ctx));
    TEST_ASSERT(lr_obj_build_symbol_cache(&obj_ctx, m) == 0,
                "build obj symbol cache");
    m->obj_ctx = &obj_ctx;

    jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");
    lr_jit_begin_update(jit);

    memset(&meta, 0, sizeof(meta));
    meta.func = f;
    meta.ret_type = f->ret_type;
    meta.param_types = f->param_types;
    meta.num_params = f->num_params;
    meta.vararg = f->vararg;
    meta.num_blocks = f->num_blocks;
    meta.next_vreg = f->next_vreg;
    meta.mode = LR_COMPILE_ISEL;
    meta.jit = jit;

    rc = target->compile_begin(&compile_ctx, &meta, m, jit->code_buf, jit->code_cap, arena);
    TEST_ASSERT_EQ(rc, 0, "compile_begin succeeds");
    TEST_ASSERT(compile_ctx != NULL, "compile context allocated");

    for (uint32_t bi = 0; bi < f->num_blocks; bi++) {
        lr_block_t *b = f->block_array[bi];
        TEST_ASSERT(b != NULL, "block exists");
        rc = target->compile_set_block(compile_ctx, b->id);
        TEST_ASSERT_EQ(rc, 0, "set block succeeds");
        for (uint32_t ii = 0; ii < b->num_insts; ii++) {
            lr_inst_t *inst = b->inst_array[ii];
            lr_compile_inst_desc_t desc;
            lr_operand_desc_t *ops = NULL;
            memset(&desc, 0, sizeof(desc));
            TEST_ASSERT(inst != NULL, "instruction exists");
            if (inst->num_operands > 0) {
                ops = lr_arena_array(arena, lr_operand_desc_t, inst->num_operands);
                TEST_ASSERT(ops != NULL, "operand desc allocation");
                for (uint32_t oi = 0; oi < inst->num_operands; oi++) {
                    TEST_ASSERT(operand_to_desc_codegen(&inst->operands[oi],
                                                        &ops[oi]) == 0,
                                "operand conversion succeeds");
                }
            }

            desc.op = inst->op;
            desc.type = inst->type;
            desc.dest = inst->dest;
            desc.operands = ops;
            desc.num_operands = inst->num_operands;
            desc.indices = inst->num_indices ? inst->indices : NULL;
            desc.num_indices = inst->num_indices;
            desc.icmp_pred = (int)inst->icmp_pred;
            desc.fcmp_pred = (int)inst->fcmp_pred;
            desc.call_external_abi = inst->call_external_abi;
            desc.call_vararg = inst->call_vararg;
            desc.call_fixed_args = inst->call_fixed_args;

            rc = target->compile_emit(compile_ctx, &desc);
            TEST_ASSERT_EQ(rc, 0, "compile_emit succeeds");
        }
    }

    rc = target->compile_end(compile_ctx, &code_len);
    TEST_ASSERT_EQ(rc, 0, "compile_end succeeds");
    TEST_ASSERT(code_len > 0, "generated code size");
    TEST_ASSERT(obj_ctx.num_relocs > 0, "relocations captured");
    jit->code_size = code_len;

    for (uint32_t i = 0; i < obj_ctx.num_relocs; i++) {
        lr_obj_reloc_t *r = &obj_ctx.relocs[i];
        const char *name;
        TEST_ASSERT(r->symbol_idx < obj_ctx.num_symbols, "reloc symbol index in range");
        name = obj_ctx.symbols[r->symbol_idx].name;
        if (!name || strcmp(name, "g") != 0)
            continue;
        if (r->type == LR_RELOC_X86_64_64)
            abs64_for_g++;
        if (r->type == LR_RELOC_X86_64_PC32 ||
            r->type == LR_RELOC_X86_64_GOTPCREL)
            disallowed_for_g++;
    }

    TEST_ASSERT(abs64_for_g >= 2, "global load/store use abs64 relocations");
    TEST_ASSERT_EQ(disallowed_for_g, 0, "no rel32-style global relocations");
    lr_jit_add_symbol(jit, "g", (void *)far_addr);
    TEST_ASSERT_EQ(lr_jit_patch_relocs(jit, &obj_ctx), 0,
                   "patch relocs succeeds with far global address");
    lr_jit_end_update(jit);

    m->obj_ctx = NULL;
    lr_objfile_ctx_destroy(&obj_ctx);
    lr_jit_destroy(jit);
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

static int has_mov_rcx_rax(const uint8_t *code, size_t code_len) {
    for (size_t i = 0; i + 2 < code_len; i++) {
        if (code[i + 0] == 0x48 &&
            ((code[i + 1] == 0x89 && code[i + 2] == 0xC1) ||
             (code[i + 1] == 0x8B && code[i + 2] == 0xC8))) {
            return 1;
        }
    }
    return 0;
}

static int count_rcx_loads_from_rbp(const uint8_t *code, size_t code_len) {
    int count = 0;
    for (size_t i = 0; i + 3 < code_len; i++) {
        if (code[i + 0] == 0x48 && code[i + 1] == 0x8B &&
            (code[i + 2] == 0x4D || code[i + 2] == 0x8D)) {
            count++;
        }
    }
    return count;
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
    int rc = lr_target_compile(target, LR_COMPILE_ISEL, m->first_func, m, code, sizeof(code), &code_len, arena);
    TEST_ASSERT_EQ(rc, 0, "compile succeeds");
    TEST_ASSERT(code_len > 0, "generated some code");
    TEST_ASSERT(!has_immediate_store_reload_pair(code, code_len),
                "no immediate store+reload for same stack slot");
    /* Streaming ISel (no lookahead) stores each intermediate to its stack
       slot, so both %t and %u produce an RAX spill (2 total). */
    TEST_ASSERT_EQ(count_rax_store_to_rbp(code, code_len), 2,
                   "streaming ISel spills both intermediates to stack");

    lr_arena_destroy(arena);
    return 0;
}

int test_codegen_reuse_cached_vreg_across_scratch_regs(void) {
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
    int rc = lr_target_compile(target, LR_COMPILE_ISEL, m->first_func, m, code, sizeof(code), &code_len, arena);
    TEST_ASSERT_EQ(rc, 0, "compile succeeds");
    TEST_ASSERT(code_len > 0, "generated some code");
    TEST_ASSERT(has_mov_rcx_rax(code, code_len),
                "reuses cached vreg with mov rcx, rax");
    TEST_ASSERT(count_rcx_loads_from_rbp(code, code_len) <= 1,
                "cached vreg copy keeps rcx stack reloads minimal");

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
    int rc = lr_target_compile(target, LR_COMPILE_ISEL, m->first_func, m, code, sizeof(code), &code_len, arena);
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
    int rc = lr_target_compile(target, LR_COMPILE_ISEL, m->first_func, m, code, sizeof(code), &code_len, arena);
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
    int rc = lr_target_compile(target, LR_COMPILE_ISEL, m->first_func, m, code, sizeof(code), &code_len, arena);
    TEST_ASSERT_EQ(rc, 0, "compile succeeds");
    TEST_ASSERT(code_len > 0, "generated some code");
    TEST_ASSERT(has_mov_imm_zero_rax(code, code_len),
                "select keeps mov imm zero so condition flags stay intact");

    lr_arena_destroy(arena);
    return 0;
}
