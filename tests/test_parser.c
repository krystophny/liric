#include "../src/arena.h"
#include "../src/ir.h"
#include "../src/ll_parser.h"
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
    TEST_ASSERT_EQ(alloca_inst->type->kind, LR_TYPE_PTR, "named type parsed as opaque ptr");

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
