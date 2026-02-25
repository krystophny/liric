#include "../src/arena.h"
#include "../src/ir.h"
#include "../src/ll_parser.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
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

static int appendf(char **buf, size_t *len, size_t *cap, const char *fmt, ...) {
    va_list ap;
    int need;

    if (!buf || !len || !cap || !fmt)
        return -1;
    if (!*buf) {
        *cap = 1024;
        *buf = malloc(*cap);
        if (!*buf)
            return -1;
        (*buf)[0] = '\0';
        *len = 0;
    }

    while (1) {
        va_start(ap, fmt);
        need = vsnprintf(*buf + *len, *cap - *len, fmt, ap);
        va_end(ap);
        if (need < 0)
            return -1;
        if ((size_t)need < (*cap - *len)) {
            *len += (size_t)need;
            return 0;
        }

        size_t min_cap = *len + (size_t)need + 1;
        size_t new_cap = (*cap < 1024) ? 1024 : *cap;
        while (new_cap < min_cap)
            new_cap *= 2;
        char *new_buf = realloc(*buf, new_cap);
        if (!new_buf)
            return -1;
        *buf = new_buf;
        *cap = new_cap;
    }
}

typedef struct {
    char names[128];
    int calls;
    int fail_on_call;
    bool saw_global_before_first_callback;
} stream_cb_ctx_t;

static int collect_stream_callback(lr_func_t *func, lr_module_t *mod, void *ctx_ptr) {
    stream_cb_ctx_t *ctx = (stream_cb_ctx_t *)ctx_ptr;
    size_t used;
    int n;

    if (!ctx || !func || !mod)
        return -1;

    if (ctx->calls == 0 && mod->first_global != NULL)
        ctx->saw_global_before_first_callback = true;

    used = strlen(ctx->names);
    if (ctx->calls > 0 && used + 1 < sizeof(ctx->names)) {
        ctx->names[used] = ',';
        ctx->names[used + 1] = '\0';
        used++;
    }
    n = snprintf(ctx->names + used, sizeof(ctx->names) - used, "%s", func->name);
    if (n < 0 || (size_t)n >= sizeof(ctx->names) - used)
        return -1;

    ctx->calls++;
    if (ctx->fail_on_call > 0 && ctx->calls >= ctx->fail_on_call)
        return -1;
    return 0;
}

int test_parser_ret_i32(void) {
    const char *src = "define i32 @f() {\nentry:\n  ret i32 42\n}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "function exists");
    TEST_ASSERT(strcmp(f->name, "f") == 0, "function name is 'f'");
    TEST_ASSERT(f->ret_type->kind == LR_TYPE_I32, "return type is i32");
    TEST_ASSERT_EQ(f->num_params, 0, "no params");
    TEST_ASSERT(!f->is_decl, "not a declaration");

    lr_block_t *b = f->first_block;
    TEST_ASSERT(b != NULL, "has entry block");

    lr_inst_t *inst = b->first;
    TEST_ASSERT(inst != NULL, "has instruction");
    TEST_ASSERT_EQ(inst->op, LR_OP_RET, "instruction is ret");
    TEST_ASSERT_EQ(inst->num_operands, 1, "ret has 1 operand");
    TEST_ASSERT_EQ(inst->operands[0].kind, LR_VAL_IMM_I64, "operand is immediate");
    TEST_ASSERT_EQ(inst->operands[0].imm_i64, 42, "immediate value is 42");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_function_decl(void) {
    const char *src = "declare i32 @puts(ptr)\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "function exists");
    TEST_ASSERT(strcmp(f->name, "puts") == 0, "function name is 'puts'");
    TEST_ASSERT(f->is_decl, "is a declaration");
    TEST_ASSERT_EQ(f->num_params, 1, "1 param");
    TEST_ASSERT_EQ(f->param_types[0]->kind, LR_TYPE_PTR, "param is ptr");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_typed_pointer_decl_params(void) {
    const char *src =
        "declare i32 @puts(i8*)\n"
        "declare void @take_pp(i8**)\n"
        "declare void @take_arr_ptr([4 x i8]*)\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *puts_fn = m->first_func;
    TEST_ASSERT(puts_fn != NULL, "puts declaration exists");
    TEST_ASSERT(strcmp(puts_fn->name, "puts") == 0, "first declaration is puts");
    TEST_ASSERT_EQ(puts_fn->num_params, 1, "puts has one param");
    TEST_ASSERT_EQ(puts_fn->param_types[0]->kind, LR_TYPE_PTR, "i8* parsed as ptr");

    lr_func_t *pp_fn = puts_fn->next;
    TEST_ASSERT(pp_fn != NULL, "take_pp declaration exists");
    TEST_ASSERT(strcmp(pp_fn->name, "take_pp") == 0, "second declaration is take_pp");
    TEST_ASSERT_EQ(pp_fn->num_params, 1, "take_pp has one param");
    TEST_ASSERT_EQ(pp_fn->param_types[0]->kind, LR_TYPE_PTR, "i8** parsed as ptr");

    lr_func_t *arr_fn = pp_fn->next;
    TEST_ASSERT(arr_fn != NULL, "take_arr_ptr declaration exists");
    TEST_ASSERT(strcmp(arr_fn->name, "take_arr_ptr") == 0, "third declaration is take_arr_ptr");
    TEST_ASSERT_EQ(arr_fn->num_params, 1, "take_arr_ptr has one param");
    TEST_ASSERT_EQ(arr_fn->param_types[0]->kind, LR_TYPE_PTR, "[4 x i8]* parsed as ptr");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_add(void) {
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

    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "function exists");
    TEST_ASSERT_EQ(f->num_params, 2, "2 params");

    lr_block_t *b = f->first_block;
    TEST_ASSERT(b != NULL, "has entry block");

    lr_inst_t *add = b->first;
    TEST_ASSERT(add != NULL, "has add instruction");
    TEST_ASSERT_EQ(add->op, LR_OP_ADD, "instruction is add");
    TEST_ASSERT_EQ(add->num_operands, 2, "add has 2 operands");

    lr_inst_t *ret = add->next;
    TEST_ASSERT(ret != NULL, "has ret instruction");
    TEST_ASSERT_EQ(ret->op, LR_OP_RET, "second instruction is ret");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_rejects_mismatched_vreg_types(void) {
    const char *src =
        "define i32 @bad() {\n"
        "entry:\n"
        "  %a = add i32 0, 1\n"
        "  %b = add i64 0, 2\n"
        "  %c = add i32 %a, %b\n"
        "  ret i32 %c\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m == NULL, "type-mismatched IR must fail to parse");
    TEST_ASSERT(strstr(err, "type mismatch") != NULL, "error reports type mismatch");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_typed_call_and_dot_label(void) {
    const char *src =
        "declare i32 @g(i32)\n"
        "define i32 @f() {\n"
        ".entry:\n"
        "  %0 = call i32 (i32) @g(i32 41)\n"
        "  %1 = add i32 %0, 1\n"
        "  ret i32 %1\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    while (f && strcmp(f->name, "f") != 0)
        f = f->next;
    TEST_ASSERT(f != NULL, "function f exists");
    TEST_ASSERT(!f->is_decl, "f is definition");

    lr_block_t *b = f->first_block;
    TEST_ASSERT(b != NULL, "has entry block");

    lr_inst_t *call = b->first;
    TEST_ASSERT(call != NULL, "has call");
    TEST_ASSERT_EQ(call->op, LR_OP_CALL, "first op is call");
    TEST_ASSERT_EQ(call->operands[0].kind, LR_VAL_GLOBAL, "callee is global symbol");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_named_type_operand(void) {
    const char *src =
        "%string_descriptor = type <{ ptr, i64 }>\n"
        "define i32 @f() {\n"
        ".entry:\n"
        "  %d = alloca %string_descriptor, align 8\n"
        "  ret i32 0\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "function exists");
    lr_block_t *b = f->first_block;
    TEST_ASSERT(b != NULL, "entry block exists");

    lr_inst_t *alloca_inst = b->first;
    TEST_ASSERT(alloca_inst != NULL, "has alloca");
    TEST_ASSERT_EQ(alloca_inst->op, LR_OP_ALLOCA, "first op is alloca");
    TEST_ASSERT_EQ(alloca_inst->type->kind, LR_TYPE_STRUCT, "named type resolved to struct");
    TEST_ASSERT_EQ(alloca_inst->type->struc.packed, true, "struct is packed");
    TEST_ASSERT_EQ(alloca_inst->type->struc.num_fields, 2, "struct has 2 fields");
    TEST_ASSERT_EQ(lr_type_size(alloca_inst->type), 16, "packed struct is 16 bytes");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_forward_named_type_by_value(void) {
    const char *src =
        "%A = type { %B }\n"
        "%B = type { i64, i64 }\n"
        "define i32 @f() {\n"
        ".entry:\n"
        "  %a = alloca %A, align 8\n"
        "  %b = getelementptr %A, %A* %a, i32 0, i32 0\n"
        "  %x = getelementptr %B, %B* %b, i32 0, i32 1\n"
        "  store i64 7, i64* %x, align 8\n"
        "  ret i32 0\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "function exists");
    lr_block_t *b = f->first_block;
    TEST_ASSERT(b != NULL, "entry block exists");

    lr_inst_t *alloca_inst = b->first;
    TEST_ASSERT(alloca_inst != NULL, "has alloca");
    TEST_ASSERT_EQ(alloca_inst->op, LR_OP_ALLOCA, "first op is alloca");
    TEST_ASSERT_EQ(alloca_inst->type->kind, LR_TYPE_STRUCT, "A resolves to struct");
    TEST_ASSERT_EQ(lr_type_size(alloca_inst->type), 16, "A by-value size tracks forward B");

    lr_inst_t *gep_b = alloca_inst->next;
    TEST_ASSERT(gep_b != NULL, "first gep exists");
    TEST_ASSERT_EQ(gep_b->op, LR_OP_GEP, "second op is gep");
    TEST_ASSERT_EQ(gep_b->type->kind, LR_TYPE_STRUCT, "gep base type is struct A");
    TEST_ASSERT_EQ(lr_type_size(gep_b->type), 16, "struct A size is correct");

    lr_inst_t *gep_x = gep_b->next;
    TEST_ASSERT(gep_x != NULL, "second gep exists");
    TEST_ASSERT_EQ(gep_x->op, LR_OP_GEP, "third op is gep");
    TEST_ASSERT_EQ(gep_x->type->kind, LR_TYPE_STRUCT, "gep base type is struct B");
    TEST_ASSERT_EQ(lr_type_size(gep_x->type), 16, "struct B size is correct");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_gep_runtime_index_canonicalized_i64(void) {
    const char *src =
        "define ptr @f(i32 %idx) {\n"
        "entry:\n"
        "  %arr = alloca [4 x i64], align 8\n"
        "  %p = getelementptr [4 x i64], ptr %arr, i32 0, i32 %idx\n"
        "  ret ptr %p\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "function exists");
    TEST_ASSERT_EQ(f->num_params, 1, "one param");

    lr_block_t *b = f->first_block;
    TEST_ASSERT(b != NULL, "entry block exists");

    lr_inst_t *inst = b->first;
    bool saw_sext = false;
    uint32_t sext_dest = 0;
    lr_inst_t *gep = NULL;
    while (inst) {
        if (inst->op == LR_OP_SEXT) {
            saw_sext = true;
            sext_dest = inst->dest;
            TEST_ASSERT(inst->type->kind == LR_TYPE_I64, "sext result is i64");
            TEST_ASSERT(inst->num_operands == 1, "sext has one operand");
            TEST_ASSERT(inst->operands[0].kind == LR_VAL_VREG, "sext source is vreg");
            TEST_ASSERT(inst->operands[0].vreg == f->param_vregs[0], "sext source is idx param");
        }
        if (inst->op == LR_OP_GEP)
            gep = inst;
        inst = inst->next;
    }

    TEST_ASSERT(saw_sext, "parser inserts sext for runtime gep index");
    TEST_ASSERT(gep != NULL, "gep exists");
    TEST_ASSERT_EQ(gep->num_operands, 3, "gep has base + 2 indices");
    TEST_ASSERT(gep->operands[2].kind == LR_VAL_VREG, "runtime index is vreg");
    TEST_ASSERT(gep->operands[2].type->kind == LR_TYPE_I64, "runtime index type canonicalized to i64");
    TEST_ASSERT(gep->operands[2].vreg == sext_dest, "gep uses canonicalized sext vreg");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_decl_with_modern_param_attrs(void) {
    const char *src =
        "declare void @llvm.memcpy.p0.p0.i32("
        "ptr noalias writeonly captures(none), "
        "ptr noalias readonly captures(none), "
        "i32, i1 immarg) #0\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "declaration exists");
    TEST_ASSERT(f->is_decl, "is declaration");
    TEST_ASSERT_EQ(f->num_params, 4, "param count matches");
    TEST_ASSERT_EQ(f->param_types[0]->kind, LR_TYPE_PTR, "param 0 is ptr");
    TEST_ASSERT_EQ(f->param_types[1]->kind, LR_TYPE_PTR, "param 1 is ptr");
    TEST_ASSERT_EQ(f->param_types[2]->kind, LR_TYPE_I32, "param 2 is i32");
    TEST_ASSERT_EQ(f->param_types[3]->kind, LR_TYPE_I1, "param 3 is i1");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_store_with_const_gep_operand(void) {
    const char *src =
        "@arr = global [4 x i32] zeroinitializer\n"
        "define void @f(ptr %dst) {\n"
        "entry:\n"
        "  store ptr getelementptr inbounds ([4 x i32], ptr @arr, i32 0, i32 1), "
        "ptr %dst, align 8\n"
        "  ret void\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    while (f && strcmp(f->name, "f") != 0)
        f = f->next;
    TEST_ASSERT(f != NULL, "function f exists");

    lr_block_t *b = f->first_block;
    TEST_ASSERT(b != NULL, "entry block exists");
    lr_inst_t *store = b->first;
    TEST_ASSERT(store != NULL, "store instruction exists");
    TEST_ASSERT_EQ(store->op, LR_OP_STORE, "first op is store");
    TEST_ASSERT_EQ(store->operands[0].kind, LR_VAL_GLOBAL,
                   "constant gep lowered to global operand");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_call_arg_with_align_attr(void) {
    const char *src =
        "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)\n"
        "define void @f(ptr %dst, ptr %src) {\n"
        "entry:\n"
        "  call void @llvm.memcpy.p0.p0.i64(ptr align 8 %dst, ptr align 8 %src, "
        "i64 12, i1 false)\n"
        "  ret void\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    while (f && strcmp(f->name, "f") != 0)
        f = f->next;
    TEST_ASSERT(f != NULL, "function f exists");
    TEST_ASSERT(!f->is_decl, "f is definition");

    lr_block_t *b = f->first_block;
    TEST_ASSERT(b != NULL, "entry block exists");
    lr_inst_t *call = b->first;
    TEST_ASSERT(call != NULL, "call exists");
    TEST_ASSERT_EQ(call->op, LR_OP_CALL, "call parsed");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_store_with_struct_constant(void) {
    const char *src =
        "%t = type { i32, i32 }\n"
        "define void @f(ptr %dst) {\n"
        "entry:\n"
        "  store %t { i32 1, i32 2 }, ptr %dst, align 4\n"
        "  ret void\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "function exists");
    lr_block_t *b = f->first_block;
    TEST_ASSERT(b != NULL, "entry block exists");
    lr_inst_t *store = b->first;
    TEST_ASSERT(store != NULL, "store exists");
    TEST_ASSERT_EQ(store->op, LR_OP_STORE, "store parsed");
    TEST_ASSERT_EQ(store->operands[0].kind, LR_VAL_IMM_I64,
                   "struct constant packed into i64");
    TEST_ASSERT_EQ(store->operands[0].imm_i64,
                   (int64_t)1 | ((int64_t)2 << 32),
                   "packed {i32 1, i32 2}");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_store_packed_struct_float_pair(void) {
    const char *src =
        "%complex_4 = type <{ float, float }>\n"
        "define void @f(ptr %dst) {\n"
        "entry:\n"
        "  store %complex_4 <{ float 1.0, float 2.0 }>, ptr %dst, align 4\n"
        "  ret void\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "function exists");
    lr_block_t *b = f->first_block;
    TEST_ASSERT(b != NULL, "entry block exists");
    lr_inst_t *store = b->first;
    TEST_ASSERT(store != NULL, "store exists");
    TEST_ASSERT_EQ(store->op, LR_OP_STORE, "store parsed");
    TEST_ASSERT_EQ(store->operands[0].kind, LR_VAL_IMM_I64,
                   "packed float pair fits in i64");

    uint32_t lo, hi;
    float f1 = 1.0f, f2 = 2.0f;
    memcpy(&lo, &f1, 4);
    memcpy(&hi, &f2, 4);
    int64_t expect = (int64_t)lo | ((int64_t)hi << 32);
    TEST_ASSERT_EQ(store->operands[0].imm_i64, expect,
                   "packed <{float 1.0, float 2.0}>");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_store_packed_struct_double_pair(void) {
    const char *src =
        "%complex_8 = type <{ double, double }>\n"
        "define void @f(ptr %dst) {\n"
        "entry:\n"
        "  store %complex_8 <{ double 1.0, double 2.0 }>, ptr %dst, align 8\n"
        "  ret void\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "function exists");
    lr_block_t *b = f->first_block;
    TEST_ASSERT(b != NULL, "entry block exists");

    lr_inst_t *inst = b->first;
    TEST_ASSERT(inst != NULL, "first inst exists");
    TEST_ASSERT_EQ(inst->op, LR_OP_GEP, "first field: gep");
    inst = inst->next;
    TEST_ASSERT(inst != NULL, "second inst exists");
    TEST_ASSERT_EQ(inst->op, LR_OP_STORE, "first field: store");
    TEST_ASSERT_EQ(inst->operands[0].kind, LR_VAL_IMM_F64,
                   "field 0 is double imm");
    inst = inst->next;
    TEST_ASSERT(inst != NULL, "third inst exists");
    TEST_ASSERT_EQ(inst->op, LR_OP_GEP, "second field: gep");
    inst = inst->next;
    TEST_ASSERT(inst != NULL, "fourth inst exists");
    TEST_ASSERT_EQ(inst->op, LR_OP_STORE, "second field: store");
    TEST_ASSERT_EQ(inst->operands[0].kind, LR_VAL_IMM_F64,
                   "field 1 is double imm");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_urem_instruction(void) {
    const char *src =
        "define i32 @f(i32 %a, i32 %b) {\n"
        "entry:\n"
        "  %r = urem i32 %a, %b\n"
        "  ret i32 %r\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "function exists");
    lr_block_t *b = f->first_block;
    TEST_ASSERT(b != NULL, "entry block exists");
    lr_inst_t *inst = b->first;
    TEST_ASSERT(inst != NULL, "instruction exists");
    TEST_ASSERT_EQ(inst->op, LR_OP_UREM, "urem parsed as unsigned rem opcode");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_udiv_instruction(void) {
    const char *src =
        "define i32 @f(i32 %a, i32 %b) {\n"
        "entry:\n"
        "  %r = udiv i32 %a, %b\n"
        "  ret i32 %r\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "function exists");
    lr_block_t *b = f->first_block;
    TEST_ASSERT(b != NULL, "entry block exists");
    lr_inst_t *inst = b->first;
    TEST_ASSERT(inst != NULL, "instruction exists");
    TEST_ASSERT_EQ(inst->op, LR_OP_UDIV, "udiv parsed as unsigned div opcode");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_frem_instruction(void) {
    const char *src =
        "define double @f(double %a, double %b) {\n"
        "entry:\n"
        "  %r = frem double %a, %b\n"
        "  ret double %r\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "function exists");
    lr_block_t *b = f->first_block;
    TEST_ASSERT(b != NULL, "entry block exists");
    lr_inst_t *inst = b->first;
    TEST_ASSERT(inst != NULL, "instruction exists");
    TEST_ASSERT_EQ(inst->op, LR_OP_FREM, "frem parsed as FP rem opcode");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_canonical_phi_pairs(void) {
    const char *src =
        "define i32 @f(i1 %cond) {\n"
        "entry:\n"
        "  br i1 %cond, label %if.true, label %if.false\n"
        "if.true:\n"
        "  br label %merge\n"
        "if.false:\n"
        "  br label %merge\n"
        "merge:\n"
        "  %x = phi i32 [42, %if.true], [7, %if.false]\n"
        "  ret i32 %x\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "function exists");
    lr_block_t *b = f->first_block;
    while (b && strcmp(b->name, "merge") != 0)
        b = b->next;
    TEST_ASSERT(b != NULL, "merge block exists");

    lr_inst_t *phi = b->first;
    TEST_ASSERT(phi != NULL, "phi exists");
    TEST_ASSERT_EQ(phi->op, LR_OP_PHI, "phi parsed");
    TEST_ASSERT_EQ(phi->num_operands, 4, "phi has 2 incoming pairs");
    TEST_ASSERT_EQ(phi->operands[1].kind, LR_VAL_BLOCK, "incoming block operand 0");
    TEST_ASSERT_EQ(phi->operands[3].kind, LR_VAL_BLOCK, "incoming block operand 1");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_phi_many_incoming_pairs(void) {
    const uint32_t npreds = 40;
    char *src = NULL;
    size_t src_len = 0;
    size_t src_cap = 0;
    lr_arena_t *arena = NULL;
    lr_module_t *m = NULL;
    lr_func_t *f = NULL;
    lr_block_t *b = NULL;
    lr_inst_t *phi = NULL;
    char err[256] = {0};

    if (appendf(&src, &src_len, &src_cap,
                "define i32 @f(i32 %%x) {\n"
                "entry:\n"
                "  br label %%b0\n") != 0) {
        free(src);
        return 1;
    }

    for (uint32_t i = 0; i < npreds; i++) {
        if (appendf(&src, &src_len, &src_cap,
                    "b%u:\n"
                    "  br label %%merge\n", i) != 0) {
            free(src);
            return 1;
        }
    }

    if (appendf(&src, &src_len, &src_cap,
                "merge:\n"
                "  %%p = phi i32 ") != 0) {
        free(src);
        return 1;
    }
    for (uint32_t i = 0; i < npreds; i++) {
        if (appendf(&src, &src_len, &src_cap,
                    "%s[%u, %%b%u]", i == 0 ? "" : ", ", i + 1u, i) != 0) {
            free(src);
            return 1;
        }
    }
    if (appendf(&src, &src_len, &src_cap,
                "\n"
                "  ret i32 %%p\n"
                "}\n") != 0) {
        free(src);
        return 1;
    }

    arena = lr_arena_create(0);
    m = lr_parse_ll_text(src, src_len, arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    f = m->first_func;
    TEST_ASSERT(f != NULL, "function exists");
    b = f->first_block;
    while (b && strcmp(b->name, "merge") != 0)
        b = b->next;
    TEST_ASSERT(b != NULL, "merge block exists");

    phi = b->first;
    TEST_ASSERT(phi != NULL, "phi instruction exists");
    TEST_ASSERT_EQ(phi->op, LR_OP_PHI, "merge begins with phi");
    TEST_ASSERT_EQ(phi->num_operands, npreds * 2u,
                   "phi stores all incoming value/block pairs");
    TEST_ASSERT_EQ(phi->operands[0].kind, LR_VAL_IMM_I64,
                   "first incoming value preserved");
    TEST_ASSERT_EQ(phi->operands[(npreds * 2u) - 2u].kind, LR_VAL_IMM_I64,
                   "last incoming value preserved");
    TEST_ASSERT_EQ(phi->operands[(npreds * 2u) - 1u].kind, LR_VAL_BLOCK,
                   "last incoming block preserved");

    lr_arena_destroy(arena);
    free(src);
    return 0;
}

int test_parser_select_with_ptr_operands(void) {
    const char *src =
        "@a = global i32 0\n"
        "@b = global i32 0\n"
        "define ptr @pick(i1 %cond) {\n"
        "entry:\n"
        "  %p = select i1 %cond, ptr @a, ptr @b\n"
        "  ret ptr %p\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    while (f && strcmp(f->name, "pick") != 0)
        f = f->next;
    TEST_ASSERT(f != NULL, "pick function exists");

    lr_block_t *b = f->first_block;
    TEST_ASSERT(b != NULL, "entry block exists");
    lr_inst_t *sel = b->first;
    TEST_ASSERT(sel != NULL, "select exists");
    TEST_ASSERT_EQ(sel->op, LR_OP_SELECT, "select parsed");
    TEST_ASSERT_EQ(sel->type->kind, LR_TYPE_PTR, "select result type is ptr");
    TEST_ASSERT_EQ(sel->operands[1].kind, LR_VAL_GLOBAL, "true arm is global");
    TEST_ASSERT_EQ(sel->operands[2].kind, LR_VAL_GLOBAL, "false arm is global");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_bitcast_const_expr_operand(void) {
    const char *src =
        "@arr = global [3 x i32] zeroinitializer\n"
        "declare void @llvm.memcpy.p0i8.p0i8.i32(i8*, i8*, i32, i1)\n"
        "define void @f(ptr %dst) {\n"
        "entry:\n"
        "  call void @llvm.memcpy.p0i8.p0i8.i32("
        "i8* %dst, i8* bitcast ([3 x i32]* @arr to i8*), i32 12, i1 false)\n"
        "  ret void\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    while (f && strcmp(f->name, "f") != 0)
        f = f->next;
    TEST_ASSERT(f != NULL, "function f exists");
    lr_block_t *b = f->first_block;
    TEST_ASSERT(b != NULL, "entry block exists");
    lr_inst_t *call = b->first;
    TEST_ASSERT(call != NULL, "call exists");
    TEST_ASSERT_EQ(call->op, LR_OP_CALL, "call parsed");
    TEST_ASSERT_EQ(call->operands[2].kind, LR_VAL_GLOBAL,
                   "bitcast const expr lowered to global ref");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_quoted_label_names(void) {
    const char *src =
        "define i32 @main() {\n"
        "\"entry block\":\n"
        "  br label %\"exit block\"\n"
        "\"exit block\":\n"
        "  ret i32 42\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "function exists");
    TEST_ASSERT(strcmp(f->name, "main") == 0, "function name is main");

    lr_block_t *entry = f->first_block;
    TEST_ASSERT(entry != NULL, "entry block exists");
    TEST_ASSERT(strcmp(entry->name, "entry block") == 0, "entry block name is correct");

    lr_block_t *exit = entry->next;
    TEST_ASSERT(exit != NULL, "exit block exists");
    TEST_ASSERT(strcmp(exit->name, "exit block") == 0, "exit block name is correct");

    lr_inst_t *br = entry->first;
    TEST_ASSERT(br != NULL, "br instruction exists");
    TEST_ASSERT_EQ(br->op, LR_OP_BR, "br instruction parsed");
    TEST_ASSERT_EQ(br->operands[0].kind, LR_VAL_BLOCK, "br target is block ref");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_boolean_literals(void) {
    const char *src =
        "define i1 @test_true() {\n"
        "entry:\n"
        "  ret i1 true\n"
        "}\n"
        "define i1 @test_false() {\n"
        "entry:\n"
        "  ret i1 false\n"
        "}\n"
        "define void @test_store() {\n"
        "entry:\n"
        "  %ptr = alloca i1\n"
        "  store i1 false, ptr %ptr, align 1\n"
        "  ret void\n"
        "}\n"
        "define i32 @test_br() {\n"
        "entry:\n"
        "  br i1 true, label %a, label %b\n"
        "a:\n"
        "  ret i32 1\n"
        "b:\n"
        "  ret i32 0\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "test_true exists");
    TEST_ASSERT(strcmp(f->name, "test_true") == 0, "first function is test_true");
    lr_block_t *b = f->first_block;
    TEST_ASSERT(b != NULL, "entry block exists");
    lr_inst_t *ret = b->first;
    TEST_ASSERT(ret != NULL, "ret instruction exists");
    TEST_ASSERT_EQ(ret->op, LR_OP_RET, "instruction is ret");
    TEST_ASSERT_EQ(ret->operands[0].kind, LR_VAL_IMM_I64, "true is immediate");
    TEST_ASSERT_EQ(ret->operands[0].imm_i64, 1, "true is 1");

    f = f->next;
    TEST_ASSERT(f != NULL, "test_false exists");
    TEST_ASSERT(strcmp(f->name, "test_false") == 0, "second function is test_false");
    b = f->first_block;
    ret = b->first;
    TEST_ASSERT_EQ(ret->operands[0].imm_i64, 0, "false is 0");

    f = f->next;
    TEST_ASSERT(f != NULL, "test_store exists");
    TEST_ASSERT(strcmp(f->name, "test_store") == 0, "third function is test_store");
    b = f->first_block;
    lr_inst_t *alloca_inst = b->first;
    TEST_ASSERT_EQ(alloca_inst->op, LR_OP_ALLOCA, "alloca parsed");
    lr_inst_t *store = alloca_inst->next;
    TEST_ASSERT(store != NULL, "store exists");
    TEST_ASSERT_EQ(store->op, LR_OP_STORE, "store parsed");
    TEST_ASSERT_EQ(store->operands[0].kind, LR_VAL_IMM_I64, "false is immediate");
    TEST_ASSERT_EQ(store->operands[0].imm_i64, 0, "false is 0");

    f = f->next;
    TEST_ASSERT(f != NULL, "test_br exists");
    TEST_ASSERT(strcmp(f->name, "test_br") == 0, "fourth function is test_br");
    b = f->first_block;
    lr_inst_t *br = b->first;
    TEST_ASSERT(br != NULL, "br exists");
    TEST_ASSERT_EQ(br->op, LR_OP_CONDBR, "br parsed");
    TEST_ASSERT_EQ(br->operands[0].kind, LR_VAL_IMM_I64, "true is immediate");
    TEST_ASSERT_EQ(br->operands[0].imm_i64, 1, "true is 1");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_function_pointer_type(void) {
    const char *src =
        "@f_ptr = global ptr null\n"
        "define void @f() {\n"
        "entry:\n"
        "  %0 = load void ()*, void ()** @f_ptr\n"
        "  ret void\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    while (f && strcmp(f->name, "f") != 0)
        f = f->next;
    TEST_ASSERT(f != NULL, "function f exists");
    lr_block_t *b = f->first_block;
    TEST_ASSERT(b != NULL, "entry block exists");
    lr_inst_t *load = b->first;
    TEST_ASSERT(load != NULL, "load exists");
    TEST_ASSERT_EQ(load->op, LR_OP_LOAD, "load parsed");
    TEST_ASSERT_EQ(load->type->kind, LR_TYPE_PTR,
                   "void ()* collapsed to ptr");
    TEST_ASSERT_EQ(load->operands[0].kind, LR_VAL_GLOBAL,
                   "load source is global");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_named_params_no_collision(void) {
    const char *src =
        "define void @increment(i32* %x) {\n"
        "entry:\n"
        "  %0 = load i32, i32* %x, align 4\n"
        "  %1 = add i32 %0, 1\n"
        "  store i32 %1, i32* %x, align 4\n"
        "  ret void\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "function exists");
    TEST_ASSERT_EQ(f->num_params, 1, "1 param");

    lr_block_t *b = f->first_block;
    TEST_ASSERT(b != NULL, "entry block exists");

    lr_inst_t *load = b->first;
    TEST_ASSERT(load != NULL, "load exists");
    TEST_ASSERT_EQ(load->op, LR_OP_LOAD, "first instruction is load");
    TEST_ASSERT_EQ(load->operands[0].kind, LR_VAL_VREG, "load from vreg");
    TEST_ASSERT_EQ(load->operands[0].vreg, f->param_vregs[0], "load from param vreg");

    lr_inst_t *add = load->next;
    TEST_ASSERT(add != NULL, "add exists");
    TEST_ASSERT_EQ(add->op, LR_OP_ADD, "second instruction is add");
    TEST_ASSERT_EQ(add->operands[0].kind, LR_VAL_VREG, "add first operand is vreg");
    TEST_ASSERT(add->operands[0].vreg != f->param_vregs[0], "add operand is load result, not param");

    lr_inst_t *store = add->next;
    TEST_ASSERT(store != NULL, "store exists");
    TEST_ASSERT_EQ(store->op, LR_OP_STORE, "third instruction is store");
    TEST_ASSERT_EQ(store->operands[0].kind, LR_VAL_VREG, "store value is vreg");
    TEST_ASSERT_EQ(store->operands[1].kind, LR_VAL_VREG, "store address is vreg");
    TEST_ASSERT_EQ(store->operands[1].vreg, f->param_vregs[0], "store to param vreg");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_unnamed_params_numeric_alias(void) {
    const char *src =
        "define i32 @sum(i32, i32) {\n"
        "entry:\n"
        "  %2 = add i32 %0, %1\n"
        "  ret i32 %2\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "function exists");
    TEST_ASSERT_EQ(f->num_params, 2, "2 params");

    lr_block_t *b = f->first_block;
    TEST_ASSERT(b != NULL, "entry block exists");

    lr_inst_t *add = b->first;
    TEST_ASSERT(add != NULL, "add exists");
    TEST_ASSERT_EQ(add->op, LR_OP_ADD, "first instruction is add");
    TEST_ASSERT_EQ(add->operands[0].kind, LR_VAL_VREG, "lhs is vreg");
    TEST_ASSERT_EQ(add->operands[1].kind, LR_VAL_VREG, "rhs is vreg");
    TEST_ASSERT_EQ(add->operands[0].vreg, f->param_vregs[0], "lhs uses first param alias %0");
    TEST_ASSERT_EQ(add->operands[1].vreg, f->param_vregs[1], "rhs uses second param alias %1");

    lr_inst_t *ret = add->next;
    TEST_ASSERT(ret != NULL, "ret exists");
    TEST_ASSERT_EQ(ret->op, LR_OP_RET, "second instruction is ret");
    TEST_ASSERT_EQ(ret->operands[0].kind, LR_VAL_VREG, "ret operand is vreg");
    TEST_ASSERT_EQ(ret->operands[0].vreg, add->dest, "ret returns add result");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_high_numeric_vregs(void) {
    const char *src =
        "define i32 @f() {\n"
        "entry:\n"
        "  %20000 = add i32 1, 2\n"
        "  %20001 = add i32 %20000, 3\n"
        "  ret i32 %20001\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "function exists");
    lr_block_t *b = f->first_block;
    TEST_ASSERT(b != NULL, "entry block exists");

    lr_inst_t *add0 = b->first;
    TEST_ASSERT(add0 != NULL, "first add exists");
    TEST_ASSERT_EQ(add0->op, LR_OP_ADD, "first instruction is add");
    lr_inst_t *add1 = add0->next;
    TEST_ASSERT(add1 != NULL, "second add exists");
    TEST_ASSERT_EQ(add1->op, LR_OP_ADD, "second instruction is add");
    TEST_ASSERT_EQ(add1->operands[0].kind, LR_VAL_VREG, "second add lhs is vreg");
    TEST_ASSERT_EQ(add1->operands[0].vreg, add0->dest, "high-numbered vreg reference resolved");

    lr_inst_t *ret = add1->next;
    TEST_ASSERT(ret != NULL, "ret exists");
    TEST_ASSERT_EQ(ret->op, LR_OP_RET, "third instruction is ret");
    TEST_ASSERT_EQ(ret->operands[0].kind, LR_VAL_VREG, "ret operand is vreg");
    TEST_ASSERT_EQ(ret->operands[0].vreg, add1->dest, "ret references second add result");

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_dynamic_vreg_map_growth(void) {
    const uint32_t n_vregs = 66000u;
    char *src = NULL;
    size_t src_len = 0;
    size_t src_cap = 0;

    if (appendf(&src, &src_len, &src_cap, "define i32 @f() {\nentry:\n") != 0) {
        free(src);
        TEST_ASSERT(false, "failed to allocate vreg test source");
    }
    for (uint32_t i = 0; i < n_vregs; i++) {
        if (i == 0) {
            if (appendf(&src, &src_len, &src_cap, "  %%0 = add i32 1, 2\n") != 0) {
                free(src);
                TEST_ASSERT(false, "failed to build vreg test source");
            }
        } else {
            if (appendf(&src, &src_len, &src_cap,
                        "  %%%u = add i32 %%%u, 1\n", i, i - 1u) != 0) {
                free(src);
                TEST_ASSERT(false, "failed to build vreg test source");
            }
        }
    }
    if (appendf(&src, &src_len, &src_cap, "  ret i32 %%%u\n}\n", n_vregs - 1u) != 0) {
        free(src);
        TEST_ASSERT(false, "failed to finalize vreg test source");
    }

    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};
    lr_module_t *m = lr_parse_ll_text(src, src_len, arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "function exists");
    TEST_ASSERT(f->next_vreg >= n_vregs, "all generated vregs are allocated");

    free(src);
    lr_arena_destroy(arena);
    return 0;
}

int test_parser_dynamic_block_map_growth(void) {
    const uint32_t n_blocks = 4200u;
    char *src = NULL;
    size_t src_len = 0;
    size_t src_cap = 0;

    if (appendf(&src, &src_len, &src_cap, "define i32 @f() {\nentry:\n  br label %%b0\n") != 0) {
        free(src);
        TEST_ASSERT(false, "failed to allocate block test source");
    }
    for (uint32_t i = 0; i < n_blocks; i++) {
        if (appendf(&src, &src_len, &src_cap, "b%u:\n", i) != 0) {
            free(src);
            TEST_ASSERT(false, "failed to build block labels");
        }
        if (i + 1u < n_blocks) {
            if (appendf(&src, &src_len, &src_cap, "  br label %%b%u\n", i + 1u) != 0) {
                free(src);
                TEST_ASSERT(false, "failed to build block branches");
            }
        } else {
            if (appendf(&src, &src_len, &src_cap, "  ret i32 42\n") != 0) {
                free(src);
                TEST_ASSERT(false, "failed to build terminal block");
            }
        }
    }
    if (appendf(&src, &src_len, &src_cap, "}\n") != 0) {
        free(src);
        TEST_ASSERT(false, "failed to finalize block test source");
    }

    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};
    lr_module_t *m = lr_parse_ll_text(src, src_len, arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "function exists");
    TEST_ASSERT_EQ(f->num_blocks, n_blocks + 1u, "all block labels resolve without duplicates");

    free(src);
    lr_arena_destroy(arena);
    return 0;
}

int test_parser_dynamic_global_map_growth(void) {
    const uint32_t n_globals = 4500u;
    char *src = NULL;
    size_t src_len = 0;
    size_t src_cap = 0;

    for (uint32_t i = 0; i < n_globals; i++) {
        if (appendf(&src, &src_len, &src_cap, "@g%u = global i32 %u\n", i, i) != 0) {
            free(src);
            TEST_ASSERT(false, "failed to build global declarations");
        }
    }
    if (appendf(&src, &src_len, &src_cap,
                "define i32 @f() {\nentry:\n  %%0 = load i32, i32* @g%u\n  ret i32 %%0\n}\n",
                n_globals - 1u) != 0) {
        free(src);
        TEST_ASSERT(false, "failed to finalize global test source");
    }

    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};
    lr_module_t *m = lr_parse_ll_text(src, src_len, arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    uint32_t global_count = 0;
    for (lr_global_t *g = m->first_global; g; g = g->next)
        global_count++;
    TEST_ASSERT_EQ(global_count, n_globals, "all globals are parsed");

    free(src);
    lr_arena_destroy(arena);
    return 0;
}

int test_parser_dynamic_func_map_growth(void) {
    const uint32_t n_funcs = 1200u;
    char *src = NULL;
    size_t src_len = 0;
    size_t src_cap = 0;

    for (uint32_t i = 0; i < n_funcs; i++) {
        if (appendf(&src, &src_len, &src_cap, "declare i32 @fn%u()\n", i) != 0) {
            free(src);
            TEST_ASSERT(false, "failed to build function declarations");
        }
    }
    if (appendf(&src, &src_len, &src_cap, "define i32 @main() {\nentry:\n  ret i32 0\n}\n") != 0) {
        free(src);
        TEST_ASSERT(false, "failed to finalize function test source");
    }

    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};
    lr_module_t *m = lr_parse_ll_text(src, src_len, arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    uint32_t func_count = 0;
    for (lr_func_t *f = m->first_func; f; f = f->next)
        func_count++;
    TEST_ASSERT_EQ(func_count, n_funcs + 1u, "all functions are parsed");

    free(src);
    lr_arena_destroy(arena);
    return 0;
}

int test_parser_cast_expr_in_aggregate_init(void) {
    const char *src =
        "%tt_class = type { i8*, i8* }\n"
        "%array = type { i32, i32 }\n"
        "declare void @_copy_tt(i8*, i8*)\n"
        "declare void @_alloc_tt(i8**)\n"
        "declare void @_method_tt(%tt_class*, %array**)\n"
        "@_Name_tt = private constant [3 x i8] c\"tt\\00\"\n"
        "@_Type_Info_tt = constant { i8* } { i8* getelementptr inbounds "
        "([3 x i8], [3 x i8]* @_Name_tt, i32 0, i32 0) }\n"
        "@_VTable_tt = constant { [5 x i8*] } { [5 x i8*] [\n"
        "  i8* null,\n"
        "  i8* bitcast ({ i8* }* @_Type_Info_tt to i8*),\n"
        "  i8* bitcast (void (i8*, i8*)* @_copy_tt to i8*),\n"
        "  i8* bitcast (void (i8**)* @_alloc_tt to i8*),\n"
        "  i8* bitcast (void (%tt_class*, %array**)* @_method_tt to i8*)\n"
        "] }\n"
        "@_Type_Int4 = constant { i8*, i8* } {\n"
        "  i8* inttoptr (i32 4 to i8*),\n"
        "  i8* inttoptr (i8 4 to i8*)\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};

    lr_module_t *m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    lr_global_t *vtable = NULL;
    lr_global_t *typeint = NULL;
    for (lr_global_t *g = m->first_global; g; g = g->next) {
        if (strcmp(g->name, "_VTable_tt") == 0)
            vtable = g;
        if (strcmp(g->name, "_Type_Int4") == 0)
            typeint = g;
    }
    TEST_ASSERT(vtable != NULL, "vtable global parsed");
    TEST_ASSERT(vtable->init_data != NULL, "vtable has init data");

    int reloc_count = 0;
    bool has_type_info_reloc = false;
    bool has_copy_reloc = false;
    bool has_method_reloc = false;
    for (lr_reloc_t *r = vtable->relocs; r; r = r->next) {
        reloc_count++;
        if (strcmp(r->symbol_name, "_Type_Info_tt") == 0)
            has_type_info_reloc = true;
        if (strcmp(r->symbol_name, "_copy_tt") == 0)
            has_copy_reloc = true;
        if (strcmp(r->symbol_name, "_method_tt") == 0)
            has_method_reloc = true;
    }
    TEST_ASSERT(reloc_count >= 4, "vtable has at least 4 relocations from bitcast exprs");
    TEST_ASSERT(has_type_info_reloc, "bitcast of struct ptr produces relocation");
    TEST_ASSERT(has_copy_reloc, "bitcast of simple func ptr produces relocation");
    TEST_ASSERT(has_method_reloc, "bitcast of func ptr with named-type params produces relocation");

    TEST_ASSERT(typeint != NULL, "inttoptr struct global parsed");
    TEST_ASSERT(typeint->init_data != NULL, "inttoptr struct has init data");
    TEST_ASSERT(typeint->init_size >= 2 * sizeof(uintptr_t),
                "inttoptr struct stores two pointer-sized fields");
    {
        uintptr_t f0 = 0;
        uintptr_t f1 = 0;
        memcpy(&f0, typeint->init_data, sizeof(uintptr_t));
        memcpy(&f1, typeint->init_data + sizeof(uintptr_t), sizeof(uintptr_t));
        TEST_ASSERT_EQ((long long)f0, 4, "first inttoptr immediate preserved");
        TEST_ASSERT_EQ((long long)f1, 4, "second inttoptr immediate preserved");
    }

    lr_arena_destroy(arena);
    return 0;
}

int test_parser_streaming_callback_order(void) {
    const char *src =
        "@g = global i32 7\n"
        "declare i32 @decl_only(i32)\n"
        "define i32 @first(i32 %x) {\n"
        "entry:\n"
        "  %y = add i32 %x, 1\n"
        "  ret i32 %y\n"
        "}\n"
        "define i32 @second() {\n"
        "entry:\n"
        "  ret i32 2\n"
        "}\n";
    char err[256] = {0};
    stream_cb_ctx_t ctx = {0};

    lr_module_t *m = lr_parse_ll_streaming(src, strlen(src),
                                           collect_stream_callback, &ctx,
                                           err, sizeof(err));
    TEST_ASSERT(m != NULL, err);
    TEST_ASSERT_EQ(ctx.calls, 3, "callback called for declaration and definitions");
    TEST_ASSERT(strcmp(ctx.names, "decl_only,first,second") == 0,
                "callback order follows source order");
    TEST_ASSERT(ctx.saw_global_before_first_callback, "globals parsed before first callback");

    lr_module_free(m);
    return 0;
}

int test_parser_streaming_callback_error_propagates(void) {
    const char *src =
        "define i32 @first() {\n"
        "entry:\n"
        "  ret i32 1\n"
        "}\n"
        "define i32 @second() {\n"
        "entry:\n"
        "  ret i32 2\n"
        "}\n";
    char err[256] = {0};
    stream_cb_ctx_t ctx = {0};
    ctx.fail_on_call = 2;

    lr_module_t *m = lr_parse_ll_streaming(src, strlen(src),
                                           collect_stream_callback, &ctx,
                                           err, sizeof(err));
    TEST_ASSERT(m == NULL, "streaming parser fails when callback fails");
    TEST_ASSERT(strstr(err, "function callback failed") != NULL,
                "callback failure reports parser error");
    TEST_ASSERT(strstr(err, "second") != NULL, "error message identifies failing function");

    return 0;
}

int test_parser_vector_type_roundtrip(void) {
    const char *src =
        "define <2 x float> @id(<2 x float> %x) {\n"
        "entry:\n"
        "  ret <2 x float> %x\n"
        "}\n";
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};
    lr_module_t *m;
    lr_func_t *f;
    FILE *tmp;
    char dump[1024] = {0};
    size_t nread = 0;

    m = lr_parse_ll_text(src, strlen(src), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, err);

    f = m->first_func;
    TEST_ASSERT(f != NULL, "function exists");
    TEST_ASSERT(f->ret_type != NULL, "function return type exists");
    TEST_ASSERT_EQ(f->ret_type->kind, LR_TYPE_VECTOR, "return type is vector");
    TEST_ASSERT_EQ(f->num_params, 1, "single parameter");
    TEST_ASSERT(f->param_types != NULL, "parameter type list exists");
    TEST_ASSERT_EQ(f->param_types[0]->kind, LR_TYPE_VECTOR, "param type is vector");
    TEST_ASSERT_EQ(f->param_types[0]->array.count, 2, "vector has 2 elements");
    TEST_ASSERT_EQ(f->param_types[0]->array.elem->kind, LR_TYPE_FLOAT,
                   "vector element type is float");

    tmp = tmpfile();
    TEST_ASSERT(tmp != NULL, "tmpfile for dump");
    lr_module_dump(m, tmp);
    fflush(tmp);
    rewind(tmp);
    nread = fread(dump, 1, sizeof(dump) - 1, tmp);
    dump[nread] = '\0';
    fclose(tmp);

    TEST_ASSERT(strstr(dump, "define <2 x float> @id(") != NULL,
                "dump preserves vector return type syntax");
    TEST_ASSERT(strstr(dump, "ret <2 x float>") != NULL,
                "dump preserves vector operand type syntax");
    TEST_ASSERT(strstr(dump, "[2 x float]") == NULL,
                "dump does not degrade vector to array syntax");

    lr_arena_destroy(arena);
    return 0;
}
