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
    TEST_ASSERT_EQ(store->operands[0].kind, LR_VAL_UNDEF,
                   "aggregate constant represented as undef placeholder");

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
    TEST_ASSERT_EQ(inst->op, LR_OP_SREM, "urem parsed with integer rem opcode");

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

    lr_arena_destroy(arena);
    return 0;
}
