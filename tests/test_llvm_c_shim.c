#include <llvm-c/LiricSession.h>
#include <liric/liric_legacy.h>
#include <llvm-c/LiricCompat.h>
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

int test_llvm_c_shim_add_and_lookup(void) {
    lc_context_t *ctx = lc_context_create();
    lc_module_compat_t *mod = NULL;
    LLVMLiricSessionStateRef state = NULL;
    lr_type_t *i32 = NULL;
    lr_type_t *fn_ty = NULL;
    lc_value_t *fnv = NULL;
    lr_func_t *f = NULL;
    lc_value_t *bb = NULL;
    lc_value_t *c7 = NULL;
    int rc = 0;
    void *addr = NULL;
    typedef int (*fn_t)(void);
    fn_t fn = NULL;

    TEST_ASSERT(ctx != NULL, "context create");
    mod = lc_module_create(ctx, "llvm_c_shim");
    TEST_ASSERT(mod != NULL, "module create");

    i32 = lc_get_int_type(mod, 32);
    TEST_ASSERT(i32 != NULL, "i32 type");
    fn_ty = lr_type_func_new(lc_module_get_ir(mod), i32, NULL, 0, false);
    TEST_ASSERT(fn_ty != NULL, "function type");

    fnv = lc_func_create(mod, "main", fn_ty);
    TEST_ASSERT(fnv != NULL, "function create");
    f = lc_value_get_func(fnv);
    TEST_ASSERT(f != NULL, "function unwrap");
    bb = lc_block_create(mod, f, "entry");
    TEST_ASSERT(bb != NULL, "block create");
    c7 = lc_value_const_int(mod, i32, 7, 32);
    TEST_ASSERT(c7 != NULL, "const create");
    lc_create_ret(mod, lc_value_get_block(bb), c7);

    state = LLVMLiricSessionCreate();
    TEST_ASSERT(state != NULL, "shim state create");
    rc = LLVMLiricSessionAddCompatModule(state, mod);
    TEST_ASSERT_EQ(rc, 0, "add module");

    addr = LLVMLiricSessionLookup(state, "main");
    TEST_ASSERT(addr != NULL, "lookup main");
    memcpy(&fn, &addr, sizeof(addr));
    TEST_ASSERT(fn != NULL, "cast function");
    TEST_ASSERT_EQ(fn(), 7, "main() returns 7");

    LLVMLiricSessionDispose(state);
    lc_module_destroy(mod);
    lc_context_destroy(ctx);
    return 0;
}
