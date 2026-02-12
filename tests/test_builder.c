#include <liric/liric.h>
#include <liric/liric_compat.h>
#include <stdio.h>
#include <stdlib.h>
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

static inline void fn_ptr_cast(void *dst, void *src) {
    memcpy(dst, &src, sizeof(src));
}
#define GET_FN(fn_var, jit, name) \
    do { void *_p = lr_jit_get_function((jit), (name)); \
         fn_ptr_cast(&(fn_var), _p); } while (0)

int test_builder_compat_add_to_jit(void) {
    lc_context_t *ctx = lc_context_create();
    TEST_ASSERT(ctx != NULL, "context create");

    lc_module_compat_t *mod = lc_module_create(ctx, "compat_jit");
    TEST_ASSERT(mod != NULL, "compat module create");

    lr_module_t *ir = lc_module_get_ir(mod);
    TEST_ASSERT(ir != NULL, "compat module ir");
    lr_type_t *i32 = lc_get_int_type(mod, 32);
    TEST_ASSERT(i32 != NULL, "i32 type");

    lr_type_t *fn_ty = lr_type_func_new(ir, i32, NULL, 0, false);
    TEST_ASSERT(fn_ty != NULL, "function type");
    lc_value_t *fn_val = lc_func_create(mod, "compat_ret42", fn_ty);
    TEST_ASSERT(fn_val != NULL, "function create");
    lr_func_t *f = lc_value_get_func(fn_val);
    TEST_ASSERT(f != NULL, "function unwrap");
    lc_value_t *bb_val = lc_block_create(mod, f, "entry");
    lr_block_t *entry = lc_value_get_block(bb_val);
    TEST_ASSERT(entry != NULL, "entry block");
    lc_value_t *c42 = lc_value_const_int(mod, i32, 42, 32);
    TEST_ASSERT(c42 != NULL, "const 42");
    lc_create_ret(mod, entry, c42);

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");
    int rc = lc_module_add_to_jit(mod, jit);
    TEST_ASSERT_EQ(rc, 0, "lc_module_add_to_jit");

    typedef int (*fn_t)(void);
    fn_t fn;
    GET_FN(fn, jit, "compat_ret42");
    TEST_ASSERT(fn != NULL, "function lookup");
    TEST_ASSERT_EQ(fn(), 42, "compat_ret42() == 42");

    lr_jit_destroy(jit);
    lc_module_destroy(mod);
    lc_context_destroy(ctx);
    return 0;
}

int test_builder_compat_add_to_jit_null_args(void) {
    lc_context_t *ctx = lc_context_create();
    TEST_ASSERT(ctx != NULL, "context create");
    lc_module_compat_t *mod = lc_module_create(ctx, "compat_null_args");
    TEST_ASSERT(mod != NULL, "compat module create");
    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    TEST_ASSERT_EQ(lc_module_add_to_jit(NULL, jit), -1, "null module rejected");
    TEST_ASSERT_EQ(lc_module_add_to_jit(mod, NULL), -1, "null jit rejected");

    lr_jit_destroy(jit);
    lc_module_destroy(mod);
    lc_context_destroy(ctx);
    return 0;
}

int test_builder_compat_memory_and_call_path(void) {
    lc_context_t *ctx = lc_context_create();
    TEST_ASSERT(ctx != NULL, "context create");
    lc_module_compat_t *mod = lc_module_create(ctx, "compat_mem_call");
    TEST_ASSERT(mod != NULL, "compat module create");

    lr_module_t *ir = lc_module_get_ir(mod);
    TEST_ASSERT(ir != NULL, "compat module ir");
    lr_type_t *i32 = lc_get_int_type(mod, 32);
    TEST_ASSERT(i32 != NULL, "i32 type");

    lr_type_t *fn_params[1] = { i32 };
    lr_type_t *fn_ty = lr_type_func_new(ir, i32, fn_params, 1, false);
    TEST_ASSERT(fn_ty != NULL, "function type");

    lc_value_t *callee_val = lc_func_create(mod, "compat_inc5", fn_ty);
    TEST_ASSERT(callee_val != NULL, "callee function value");
    lr_func_t *callee = lc_value_get_func(callee_val);
    TEST_ASSERT(callee != NULL, "callee function");
    lc_value_t *callee_bb_val = lc_block_create(mod, callee, "entry");
    lr_block_t *callee_bb = lc_value_get_block(callee_bb_val);
    TEST_ASSERT(callee_bb != NULL, "callee entry block");
    lc_value_t *callee_arg0 = lc_func_get_arg(mod, callee_val, 0);
    lc_value_t *c5 = lc_value_const_int(mod, i32, 5, 32);
    lc_value_t *sum = lc_create_add(mod, callee_bb, callee, callee_arg0, c5, "sum");
    TEST_ASSERT(sum != NULL, "compat add in callee");
    lc_create_ret(mod, callee_bb, sum);

    lc_value_t *caller_val = lc_func_create(mod, "compat_mem_call", fn_ty);
    TEST_ASSERT(caller_val != NULL, "caller function value");
    lr_func_t *caller = lc_value_get_func(caller_val);
    TEST_ASSERT(caller != NULL, "caller function");
    lc_value_t *caller_bb_val = lc_block_create(mod, caller, "entry");
    lr_block_t *caller_bb = lc_value_get_block(caller_bb_val);
    TEST_ASSERT(caller_bb != NULL, "caller entry block");
    lc_value_t *caller_arg0 = lc_func_get_arg(mod, caller_val, 0);

    lr_type_t *arr2_i32 = lr_type_array_new(ir, i32, 2);
    TEST_ASSERT(arr2_i32 != NULL, "array type");
    lc_alloca_inst_t *alloca_arr = lc_create_alloca(
        mod, caller_bb, caller, arr2_i32, NULL, "arr");
    TEST_ASSERT(alloca_arr != NULL, "alloca array");
    TEST_ASSERT(alloca_arr->result != NULL, "alloca result");

    lc_value_t *idx0 = lc_value_const_int(mod, i32, 0, 32);
    lc_value_t *idx1 = lc_value_const_int(mod, i32, 1, 32);
    lc_value_t *indices[2] = { idx0, idx1 };
    lc_value_t *elem_ptr = lc_create_gep(mod, caller_bb, caller, arr2_i32,
        alloca_arr->result, indices, 2, "elem_ptr");
    TEST_ASSERT(elem_ptr != NULL, "gep element pointer");

    lc_create_store(mod, caller_bb, caller_arg0, elem_ptr);
    lc_value_t *loaded = lc_create_load(mod, caller_bb, caller, i32, elem_ptr, "loaded");
    TEST_ASSERT(loaded != NULL, "load element");

    lc_value_t *call_args[1] = { loaded };
    lc_value_t *call_res = lc_create_call(mod, caller_bb, caller, fn_ty,
        callee_val, call_args, 1, "inc5_call");
    TEST_ASSERT(call_res != NULL, "compat call result");
    lc_create_ret(mod, caller_bb, call_res);

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");
    int rc = lc_module_add_to_jit(mod, jit);
    TEST_ASSERT_EQ(rc, 0, "lc_module_add_to_jit");

    typedef int (*fn_t)(int);
    fn_t fn;
    GET_FN(fn, jit, "compat_mem_call");
    TEST_ASSERT(fn != NULL, "compat_mem_call lookup");
    TEST_ASSERT_EQ(fn(37), 42, "compat_mem_call(37) == 42");
    TEST_ASSERT_EQ(fn(0), 5, "compat_mem_call(0) == 5");

    lr_jit_destroy(jit);
    lc_module_destroy(mod);
    lc_context_destroy(ctx);
    return 0;
}

int test_builder_compat_phi_finalize_add_incoming_after_finalize_noop(void) {
    lc_context_t *ctx = lc_context_create();
    TEST_ASSERT(ctx != NULL, "context create");
    lc_module_compat_t *mod = lc_module_create(ctx, "compat_phi_finalize");
    TEST_ASSERT(mod != NULL, "compat module create");

    lr_module_t *ir = lc_module_get_ir(mod);
    TEST_ASSERT(ir != NULL, "compat module ir");
    lr_type_t *i32 = lc_get_int_type(mod, 32);
    TEST_ASSERT(i32 != NULL, "i32 type");

    lr_type_t *params[1] = { i32 };
    lr_type_t *fn_ty = lr_type_func_new(ir, i32, params, 1, false);
    TEST_ASSERT(fn_ty != NULL, "function type");

    lc_value_t *fn_val = lc_func_create(mod, "compat_abs_phi_finalize", fn_ty);
    TEST_ASSERT(fn_val != NULL, "function create");
    lr_func_t *fn = lc_value_get_func(fn_val);
    TEST_ASSERT(fn != NULL, "function unwrap");
    lc_value_t *arg = lc_func_get_arg(mod, fn_val, 0);
    TEST_ASSERT(arg != NULL, "function arg");

    lr_block_t *entry = lc_value_get_block(lc_block_create(mod, fn, "entry"));
    lr_block_t *then_bb = lc_value_get_block(lc_block_create(mod, fn, "then"));
    lr_block_t *else_bb = lc_value_get_block(lc_block_create(mod, fn, "else"));
    lr_block_t *merge_bb = lc_value_get_block(lc_block_create(mod, fn, "merge"));
    TEST_ASSERT(entry && then_bb && else_bb && merge_bb, "block create");

    lc_value_t *c0 = lc_value_const_int(mod, i32, 0, 32);
    TEST_ASSERT(c0 != NULL, "const zero");
    lc_value_t *is_neg = lc_create_icmp_slt(mod, entry, fn, arg, c0, "is_neg");
    TEST_ASSERT(is_neg != NULL, "icmp slt");
    lc_create_cond_br(mod, entry, is_neg, then_bb, else_bb);

    lc_value_t *neg = lc_create_sub(mod, then_bb, fn, c0, arg, "neg");
    TEST_ASSERT(neg != NULL, "neg value");
    lc_create_br(mod, then_bb, merge_bb);
    lc_create_br(mod, else_bb, merge_bb);

    lc_phi_node_t *phi = lc_create_phi(mod, merge_bb, fn, i32, "result");
    TEST_ASSERT(phi != NULL, "phi create");
    lc_phi_add_incoming(phi, neg, then_bb);
    lc_phi_add_incoming(phi, arg, else_bb);
    lc_phi_finalize(phi);
    /* Regression guard: this must remain a no-op after finalize. */
    lc_phi_add_incoming(phi, c0, entry);
    lc_phi_finalize(phi);
    TEST_ASSERT(phi->result != NULL, "phi result");
    lc_create_ret(mod, merge_bb, phi->result);

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");
    int rc = lc_module_add_to_jit(mod, jit);
    TEST_ASSERT_EQ(rc, 0, "lc_module_add_to_jit");

    typedef int (*fn_t)(int);
    fn_t fp;
    GET_FN(fp, jit, "compat_abs_phi_finalize");
    TEST_ASSERT(fp != NULL, "compat_abs_phi_finalize lookup");
    TEST_ASSERT_EQ(fp(5), 5, "compat_abs_phi_finalize(5) == 5");
    TEST_ASSERT_EQ(fp(-7), 7, "compat_abs_phi_finalize(-7) == 7");
    TEST_ASSERT_EQ(fp(0), 0, "compat_abs_phi_finalize(0) == 0");

    lr_jit_destroy(jit);
    lc_module_destroy(mod);
    lc_context_destroy(ctx);
    return 0;
}
