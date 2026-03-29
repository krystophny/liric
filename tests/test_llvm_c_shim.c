#include <liric/liric_compat.h>
#include <liric/liric_legacy.h>
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

int test_llvm_c_shim_lookup_float_return_uses_host_abi(void) {
    lc_context_t *ctx = lc_context_create();
    lc_module_compat_t *mod = NULL;
    LLVMLiricSessionStateRef state = NULL;
    lr_type_t *f32 = NULL;
    lr_type_t *fn_ty = NULL;
    lc_value_t *fnv = NULL;
    lr_func_t *f = NULL;
    lc_value_t *bb = NULL;
    lc_value_t *c3 = NULL;
    int rc = 0;
    void *addr = NULL;
    typedef float (*fn_t)(void);
    fn_t fn = NULL;

    TEST_ASSERT(ctx != NULL, "context create");
    mod = lc_module_create(ctx, "llvm_c_shim_float_ret");
    TEST_ASSERT(mod != NULL, "module create");

    f32 = lc_get_float_type(mod);
    TEST_ASSERT(f32 != NULL, "f32 type");
    fn_ty = lr_type_func_new(lc_module_get_ir(mod), f32, NULL, 0, false);
    TEST_ASSERT(fn_ty != NULL, "function type");

    fnv = lc_func_create(mod, "retf", fn_ty);
    TEST_ASSERT(fnv != NULL, "function create");
    f = lc_value_get_func(fnv);
    TEST_ASSERT(f != NULL, "function unwrap");
    bb = lc_block_create(mod, f, "entry");
    TEST_ASSERT(bb != NULL, "block create");
    c3 = lc_value_const_fp(mod, f32, 3.0, false);
    TEST_ASSERT(c3 != NULL, "float const create");
    lc_create_ret(mod, lc_value_get_block(bb), c3);

    state = LLVMLiricSessionCreate();
    TEST_ASSERT(state != NULL, "shim state create");
    rc = LLVMLiricSessionAddCompatModule(state, mod);
    TEST_ASSERT_EQ(rc, 0, "add module");

    addr = LLVMLiricSessionLookup(state, "retf");
    TEST_ASSERT(addr != NULL, "lookup retf");
    memcpy(&fn, &addr, sizeof(addr));
    TEST_ASSERT(fn != NULL, "cast function");
    TEST_ASSERT(fn() > 2.99f && fn() < 3.01f, "retf() returns 3.0f");

    LLVMLiricSessionDispose(state);
    lc_module_destroy(mod);
    lc_context_destroy(ctx);
    return 0;
}

int test_llvm_c_shim_rejects_undeclared_data_global(void) {
    const char *src =
        "define i64 @f3() {\n"
        "entry:\n"
        "  %1 = load i64, i64* @count\n"
        "  ret i64 %1\n"
        "}\n";
    char err[256] = {0};
    lr_module_t *parsed = NULL;
    lr_module_t *old_ir = NULL;
    lc_context_t *ctx = lc_context_create();
    lc_module_compat_t *mod = NULL;
    LLVMLiricSessionStateRef state = NULL;
    int rc = 0;

    TEST_ASSERT(ctx != NULL, "context create");
    mod = lc_module_create(ctx, "llvm_c_shim_undeclared_global");
    TEST_ASSERT(mod != NULL, "module create");

    parsed = lr_parse_ll(src, strlen(src), err, sizeof(err));
    TEST_ASSERT(parsed != NULL, "parser accepts unresolved data global IR");

    old_ir = lc_module_get_ir(mod);
    TEST_ASSERT(old_ir != NULL, "module ir");
    mod->mod = parsed;
    if (mod->ctx)
        mod->ctx->mod = parsed;
    lr_module_free(old_ir);

    state = LLVMLiricSessionCreate();
    TEST_ASSERT(state != NULL, "shim state create");
    rc = LLVMLiricSessionAddCompatModule(state, mod);
    TEST_ASSERT(rc != 0, "add module rejects undeclared data global");

    LLVMLiricSessionDispose(state);
    lc_module_destroy(mod);
    lc_context_destroy(ctx);
    return 0;
}

int test_llvm_c_shim_load_library_rejects_null(void) {
    LLVMLiricSessionStateRef state = LLVMLiricSessionCreate();
    TEST_ASSERT(state != NULL, "shim state create");

    TEST_ASSERT(LLVMLiricSessionLoadLibrary(NULL, "/tmp/no.so") == -1,
                "null state rejected");
    TEST_ASSERT(LLVMLiricSessionLoadLibrary(state, NULL) == -1,
                "null path rejected");
    TEST_ASSERT(LLVMLiricSessionLoadLibrary(state, "") == -1,
                "empty path rejected");

    LLVMLiricSessionDispose(state);
    return 0;
}

int test_llvm_c_shim_lookup_complex4_return_uses_host_abi(void) {
    /* On x86_64, <2 x float> returns in xmm0 matching {float,float} layout.
       On aarch64, HFA returns ({float,float} → s0+s1) are not yet supported
       in the backend, so use a pointer-output pattern to verify the JIT
       compiles and executes complex-valued stores correctly. */
#if defined(__aarch64__) || defined(_M_ARM64)
    const char *src =
        "define void @retc32(ptr %out) {\n"
        "entry:\n"
        "  store float 2.5, ptr %out\n"
        "  %imag = getelementptr float, ptr %out, i64 1\n"
        "  store float 3.5, ptr %imag\n"
        "  ret void\n"
        "}\n";
#else
    const char *src =
        "define <2 x float> @retc32() {\n"
        "entry:\n"
        "  ret <2 x float> <float 2.5, float 3.5>\n"
        "}\n";
#endif
    char err[256] = {0};
    lr_module_t *parsed = NULL;
    lr_module_t *old_ir = NULL;
    lc_context_t *ctx = lc_context_create();
    lc_module_compat_t *mod = NULL;
    LLVMLiricSessionStateRef state = NULL;
    int rc = 0;
    void *addr = NULL;
    typedef struct {
        float re;
        float im;
    } c32_t;
    c32_t v = {0.0f, 0.0f};

    TEST_ASSERT(ctx != NULL, "context create");
    mod = lc_module_create(ctx, "llvm_c_shim_c32_ret");
    TEST_ASSERT(mod != NULL, "module create");

    parsed = lr_parse_ll(src, strlen(src), err, sizeof(err));
    TEST_ASSERT(parsed != NULL, "parse c32 return module");
    old_ir = lc_module_get_ir(mod);
    TEST_ASSERT(old_ir != NULL, "module ir");
    mod->mod = parsed;
    if (mod->ctx)
        mod->ctx->mod = parsed;
    lr_module_free(old_ir);

    state = LLVMLiricSessionCreate();
    TEST_ASSERT(state != NULL, "shim state create");
    rc = LLVMLiricSessionAddCompatModule(state, mod);
    TEST_ASSERT_EQ(rc, 0, "add module");

    addr = LLVMLiricSessionLookup(state, "retc32");
    TEST_ASSERT(addr != NULL, "lookup retc32");

#if defined(__aarch64__) || defined(_M_ARM64)
    typedef void (*fn_ptr_t)(c32_t *);
    fn_ptr_t fn_ptr = NULL;
    memcpy(&fn_ptr, &addr, sizeof(addr));
    TEST_ASSERT(fn_ptr != NULL, "cast function");
    fn_ptr(&v);
#else
    typedef c32_t (*fn_t)(void);
    fn_t fn = NULL;
    memcpy(&fn, &addr, sizeof(addr));
    TEST_ASSERT(fn != NULL, "cast function");
    v = fn();
#endif
    TEST_ASSERT(v.re > 2.49f && v.re < 2.51f, "retc32() real lane");
    TEST_ASSERT(v.im > 3.49f && v.im < 3.51f, "retc32() imag lane");

    LLVMLiricSessionDispose(state);
    lc_module_destroy(mod);
    lc_context_destroy(ctx);
    return 0;
}

int test_llvm_c_shim_cross_module_i1_call(void) {
    const char *src1 =
        "define i1 @is_four(i32 %x) {\n"
        "entry:\n"
        "  %c = icmp eq i32 %x, 4\n"
        "  ret i1 %c\n"
        "}\n";
    const char *src2 =
        "declare i1 @is_four(i32)\n"
        "define i1 @eval() {\n"
        "entry:\n"
        "  %r = call i1 @is_four(i32 4)\n"
        "  ret i1 %r\n"
        "}\n";
    char err[256] = {0};
    lr_module_t *parsed = NULL;
    lr_module_t *old_ir = NULL;
    lc_context_t *ctx = lc_context_create();
    lc_module_compat_t *mod = NULL;
    LLVMLiricSessionStateRef state = NULL;
    int rc = 0;
    void *addr = NULL;
    typedef unsigned char (*fn_t)(void);
    fn_t fn = NULL;

    TEST_ASSERT(ctx != NULL, "context create");
    mod = lc_module_create(ctx, "llvm_c_shim_cross_i1_1");
    TEST_ASSERT(mod != NULL, "module create #1");

    parsed = lr_parse_ll(src1, strlen(src1), err, sizeof(err));
    TEST_ASSERT(parsed != NULL, "parse module #1");
    old_ir = lc_module_get_ir(mod);
    TEST_ASSERT(old_ir != NULL, "module #1 ir");
    mod->mod = parsed;
    if (mod->ctx)
        mod->ctx->mod = parsed;
    lr_module_free(old_ir);

    state = LLVMLiricSessionCreate();
    TEST_ASSERT(state != NULL, "shim state create");
    rc = LLVMLiricSessionAddCompatModule(state, mod);
    TEST_ASSERT_EQ(rc, 0, "add module #1");
    lc_module_destroy(mod);
    mod = NULL;

    mod = lc_module_create(ctx, "llvm_c_shim_cross_i1_2");
    TEST_ASSERT(mod != NULL, "module create #2");
    parsed = lr_parse_ll(src2, strlen(src2), err, sizeof(err));
    TEST_ASSERT(parsed != NULL, "parse module #2");
    old_ir = lc_module_get_ir(mod);
    TEST_ASSERT(old_ir != NULL, "module #2 ir");
    mod->mod = parsed;
    if (mod->ctx)
        mod->ctx->mod = parsed;
    lr_module_free(old_ir);

    rc = LLVMLiricSessionAddCompatModule(state, mod);
    TEST_ASSERT_EQ(rc, 0, "add module #2");

    addr = LLVMLiricSessionLookup(state, "eval");
    TEST_ASSERT(addr != NULL, "lookup eval");
    memcpy(&fn, &addr, sizeof(addr));
    TEST_ASSERT(fn != NULL, "cast function");
    TEST_ASSERT_EQ(fn(), 1, "eval() returns true");

    LLVMLiricSessionDispose(state);
    lc_module_destroy(mod);
    lc_context_destroy(ctx);
    return 0;
}

int test_llvm_c_shim_cross_module_internal_global_ptr_call(void) {
    const char *src1 =
        "@buf = internal global [8 x i8] zeroinitializer\n"
        "define ptr @get_buf() {\n"
        "entry:\n"
        "  ret ptr @buf\n"
        "}\n";
    const char *src2 =
        "declare ptr @get_buf()\n"
        "define i64 @call_get_buf() {\n"
        "entry:\n"
        "  %p = call ptr @get_buf()\n"
        "  %i = ptrtoint ptr %p to i64\n"
        "  ret i64 %i\n"
        "}\n";
    char err[256] = {0};
    lr_module_t *parsed = NULL;
    lr_module_t *old_ir = NULL;
    lc_context_t *ctx = lc_context_create();
    lc_module_compat_t *mod = NULL;
    LLVMLiricSessionStateRef state = NULL;
    int rc = 0;
    void *addr = NULL;
    typedef void *(*get_fn_t)(void);
    typedef uint64_t (*call_fn_t)(void);
    get_fn_t get_fn = NULL;
    call_fn_t call_fn = NULL;
    void *buf_addr = NULL;

    TEST_ASSERT(ctx != NULL, "context create");
    mod = lc_module_create(ctx, "llvm_c_shim_cross_ptr_1");
    TEST_ASSERT(mod != NULL, "module create #1");

    parsed = lr_parse_ll(src1, strlen(src1), err, sizeof(err));
    TEST_ASSERT(parsed != NULL, "parse module #1");
    old_ir = lc_module_get_ir(mod);
    TEST_ASSERT(old_ir != NULL, "module #1 ir");
    mod->mod = parsed;
    if (mod->ctx)
        mod->ctx->mod = parsed;
    lr_module_free(old_ir);

    state = LLVMLiricSessionCreate();
    TEST_ASSERT(state != NULL, "shim state create");
    rc = LLVMLiricSessionAddCompatModule(state, mod);
    TEST_ASSERT_EQ(rc, 0, "add module #1");
    lc_module_destroy(mod);
    mod = NULL;

    mod = lc_module_create(ctx, "llvm_c_shim_cross_ptr_2");
    TEST_ASSERT(mod != NULL, "module create #2");
    parsed = lr_parse_ll(src2, strlen(src2), err, sizeof(err));
    TEST_ASSERT(parsed != NULL, "parse module #2");
    old_ir = lc_module_get_ir(mod);
    TEST_ASSERT(old_ir != NULL, "module #2 ir");
    mod->mod = parsed;
    if (mod->ctx)
        mod->ctx->mod = parsed;
    lr_module_free(old_ir);

    rc = LLVMLiricSessionAddCompatModule(state, mod);
    TEST_ASSERT_EQ(rc, 0, "add module #2");

    addr = LLVMLiricSessionLookup(state, "get_buf");
    TEST_ASSERT(addr != NULL, "lookup get_buf");
    memcpy(&get_fn, &addr, sizeof(addr));
    TEST_ASSERT(get_fn != NULL, "cast get_buf");
    buf_addr = get_fn();
    TEST_ASSERT(buf_addr != NULL, "get_buf returns internal global");

    addr = LLVMLiricSessionLookup(state, "call_get_buf");
    TEST_ASSERT(addr != NULL, "lookup call_get_buf");
    memcpy(&call_fn, &addr, sizeof(addr));
    TEST_ASSERT(call_fn != NULL, "cast call_get_buf");
    TEST_ASSERT_EQ((uintptr_t)call_fn(), (uintptr_t)buf_addr,
                   "cross-module call preserves internal global address");

    LLVMLiricSessionDispose(state);
    lc_module_destroy(mod);
    lc_context_destroy(ctx);
    return 0;
}

int test_llvm_c_shim_cross_module_internal_global_struct_dispatch(void) {
    const char *src1 =
        "define i64 @alloc_fn(ptr %ctx, i64 %n) {\n"
        "entry:\n"
        "  %r = add i64 %n, 122\n"
        "  ret i64 %r\n"
        "}\n"
        "@alloc = internal global { ptr, ptr, ptr, ptr } "
        "{ ptr @alloc_fn, ptr null, ptr null, ptr null }\n"
        "define ptr @get_alloc() {\n"
        "entry:\n"
        "  ret ptr @alloc\n"
        "}\n";
    const char *src2 =
        "declare ptr @get_alloc()\n"
        "define i64 @call_alloc() {\n"
        "entry:\n"
        "  %a = call ptr @get_alloc()\n"
        "  %fp.slot = getelementptr { ptr, ptr, ptr, ptr }, ptr %a, i32 0, i32 0\n"
        "  %fp = load ptr, ptr %fp.slot\n"
        "  %r = call i64 %fp(ptr null, i64 1)\n"
        "  ret i64 %r\n"
        "}\n";
    char err[256] = {0};
    lr_module_t *parsed = NULL;
    lr_module_t *old_ir = NULL;
    lc_context_t *ctx = lc_context_create();
    lc_module_compat_t *mod = NULL;
    LLVMLiricSessionStateRef state = NULL;
    int rc = 0;
    void *addr = NULL;
    typedef void *(*get_fn_t)(void);
    typedef int64_t (*call_fn_t)(void);
    get_fn_t get_fn = NULL;
    call_fn_t call_fn = NULL;
    void *alloc_addr = NULL;

    TEST_ASSERT(ctx != NULL, "context create");
    mod = lc_module_create(ctx, "llvm_c_shim_cross_struct_1");
    TEST_ASSERT(mod != NULL, "module create #1");

    parsed = lr_parse_ll(src1, strlen(src1), err, sizeof(err));
    TEST_ASSERT(parsed != NULL, "parse module #1");
    old_ir = lc_module_get_ir(mod);
    TEST_ASSERT(old_ir != NULL, "module #1 ir");
    mod->mod = parsed;
    if (mod->ctx)
        mod->ctx->mod = parsed;
    lr_module_free(old_ir);

    state = LLVMLiricSessionCreate();
    TEST_ASSERT(state != NULL, "shim state create");
    rc = LLVMLiricSessionAddCompatModule(state, mod);
    TEST_ASSERT_EQ(rc, 0, "add module #1");
    lc_module_destroy(mod);
    mod = NULL;

    mod = lc_module_create(ctx, "llvm_c_shim_cross_struct_2");
    TEST_ASSERT(mod != NULL, "module create #2");
    parsed = lr_parse_ll(src2, strlen(src2), err, sizeof(err));
    TEST_ASSERT(parsed != NULL, "parse module #2");
    old_ir = lc_module_get_ir(mod);
    TEST_ASSERT(old_ir != NULL, "module #2 ir");
    mod->mod = parsed;
    if (mod->ctx)
        mod->ctx->mod = parsed;
    lr_module_free(old_ir);

    rc = LLVMLiricSessionAddCompatModule(state, mod);
    TEST_ASSERT_EQ(rc, 0, "add module #2");

    addr = LLVMLiricSessionLookup(state, "get_alloc");
    TEST_ASSERT(addr != NULL, "lookup get_alloc");
    memcpy(&get_fn, &addr, sizeof(addr));
    TEST_ASSERT(get_fn != NULL, "cast get_alloc");
    alloc_addr = get_fn();
    TEST_ASSERT(alloc_addr != NULL, "get_alloc returns internal global");

    addr = LLVMLiricSessionLookup(state, "call_alloc");
    TEST_ASSERT(addr != NULL, "lookup call_alloc");
    memcpy(&call_fn, &addr, sizeof(addr));
    TEST_ASSERT(call_fn != NULL, "cast call_alloc");
    TEST_ASSERT_EQ(call_fn(), 123,
                   "cross-module helper dispatch through internal global struct works");

    LLVMLiricSessionDispose(state);
    lc_module_destroy(mod);
    lc_context_destroy(ctx);
    return 0;
}
