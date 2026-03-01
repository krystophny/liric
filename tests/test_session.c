#include <liric/liric_session.h>
#include "arena.h"
#include "ir.h"
#include "llvm_backend.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <sys/stat.h>
#include <sys/wait.h>
#endif

extern const uint8_t bc_ret42_data[];
extern const size_t bc_ret42_len;

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

static lr_func_t *find_func_by_name(lr_module_t *m, const char *name) {
    lr_func_t *f;
    if (!m || !name)
        return NULL;
    for (f = m->first_func; f; f = f->next) {
        if (f->name && strcmp(f->name, name) == 0)
            return f;
    }
    return NULL;
}

static uint32_t count_block_insts(const lr_block_t *b) {
    const lr_inst_t *inst;
    uint32_t count = 0;

    if (!b)
        return 0;
    for (inst = b->first; inst; inst = inst->next)
        count++;
    return count;
}

static uint32_t count_func_insts(const lr_func_t *f) {
    const lr_block_t *b;
    uint32_t count = 0;

    if (!f)
        return 0;
    for (b = f->first_block; b; b = b->next)
        count += count_block_insts(b);
    return count;
}

static void set_compile_mode_env(const char *value) {
#if defined(_WIN32)
    if (value) {
        (void)_putenv_s("LIRIC_COMPILE_MODE", value);
    } else {
        (void)_putenv_s("LIRIC_COMPILE_MODE", "");
    }
#else
    if (value) {
        (void)setenv("LIRIC_COMPILE_MODE", value, 1);
    } else {
        (void)unsetenv("LIRIC_COMPILE_MODE");
    }
#endif
}

int test_session_direct_ret_42(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    cfg.mode = LR_MODE_DIRECT;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    lr_type_t *i32 = lr_type_i32_s(s);
    TEST_ASSERT(i32 != NULL, "i32 type");

    int rc = lr_session_func_begin(s, "session_ret_42", i32, NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");

    uint32_t b0 = lr_session_block(s);
    rc = lr_session_set_block(s, b0, &err);
    TEST_ASSERT_EQ(rc, 0, "set block");

    lr_emit_ret(s, LR_IMM(42, i32));

    void *addr = NULL;
    rc = lr_session_func_end(s, &addr, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");
    TEST_ASSERT(addr != NULL, "compiled function address");

    typedef int (*fn_t)(void);
    fn_t fn;
    fn_ptr_cast(&fn, addr);
    TEST_ASSERT_EQ(fn(), 42, "session_ret_42() == 42");

    lr_session_destroy(s);
    return 0;
}

int test_session_add_args(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    lr_type_t *i32 = lr_type_i32_s(s);
    lr_type_t *params[] = {i32, i32};

    int rc = lr_session_func_begin(s, "session_add", i32, params, 2, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");

    uint32_t va = lr_session_param(s, 0);
    uint32_t vb = lr_session_param(s, 1);

    uint32_t b0 = lr_session_block(s);
    lr_session_set_block(s, b0, &err);

    uint32_t vc = lr_emit_add(s, i32, LR_VREG(va, i32), LR_VREG(vb, i32));
    lr_emit_ret(s, LR_VREG(vc, i32));

    void *addr = NULL;
    rc = lr_session_func_end(s, &addr, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");

    typedef int (*fn_t)(int, int);
    fn_t fn;
    fn_ptr_cast(&fn, addr);
    TEST_ASSERT_EQ(fn(10, 32), 42, "add(10,32) == 42");
    TEST_ASSERT_EQ(fn(-5, 5), 0, "add(-5,5) == 0");

    lr_session_destroy(s);
    return 0;
}

int test_session_arithmetic_chain(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    lr_type_t *i32 = lr_type_i32_s(s);
    lr_type_t *params[] = {i32, i32};

    int rc = lr_session_func_begin(s, "session_arith", i32, params, 2, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");

    uint32_t va = lr_session_param(s, 0);
    uint32_t vb = lr_session_param(s, 1);

    uint32_t b0 = lr_session_block(s);
    lr_session_set_block(s, b0, &err);

    uint32_t sum = lr_emit_add(s, i32, LR_VREG(va, i32), LR_VREG(vb, i32));
    uint32_t prod = lr_emit_mul(s, i32, LR_VREG(sum, i32), LR_VREG(vb, i32));
    uint32_t diff = lr_emit_sub(s, i32, LR_VREG(prod, i32), LR_VREG(va, i32));
    lr_emit_ret(s, LR_VREG(diff, i32));

    void *addr = NULL;
    rc = lr_session_func_end(s, &addr, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");

    typedef int (*fn_t)(int, int);
    fn_t fn;
    fn_ptr_cast(&fn, addr);
    TEST_ASSERT_EQ(fn(3, 4), 25, "arith(3,4) == 25");
    TEST_ASSERT_EQ(fn(10, 2), 14, "arith(10,2) == 14");

    lr_session_destroy(s);
    return 0;
}

int test_session_stream_stencil_fast_path(void) {
#if !defined(__x86_64__) && !defined(_M_X64)
    return 0;
#else
    lr_session_config_t cfg = {0};
    lr_error_t err;
    const char *prev_mode = getenv("LIRIC_COMPILE_MODE");
    lr_module_t *m;
    lr_func_t *f;
    lr_session_t *s;
    lr_type_t *i32;
    lr_type_t *params[2];
    int rc;
    uint32_t a, b, sum;
    void *addr = NULL;
    typedef int (*fn_t)(int, int);
    fn_t fn;

    cfg.mode = LR_MODE_DIRECT;
    cfg.backend = LR_SESSION_BACKEND_COPY_PATCH;
    s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    i32 = lr_type_i32_s(s);
    params[0] = i32;
    params[1] = i32;
    rc = lr_session_func_begin(s, "session_stream_fast", i32, params, 2, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");

    a = lr_session_param(s, 0);
    b = lr_session_param(s, 1);
    rc = lr_session_set_block(s, lr_session_block(s), &err);
    TEST_ASSERT_EQ(rc, 0, "set block");

    sum = lr_emit_add(s, i32, LR_VREG(a, i32), LR_VREG(b, i32));
    lr_emit_ret(s, LR_VREG(sum, i32));

    rc = lr_session_func_end(s, &addr, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");
    TEST_ASSERT(addr != NULL, "compiled function address");

    fn_ptr_cast(&fn, addr);
    TEST_ASSERT_EQ(fn(20, 22), 42, "stream fast add result");

    m = lr_session_module(s);
    f = find_func_by_name(m, "session_stream_fast");
    TEST_ASSERT(f != NULL, "function exists in module");
    TEST_ASSERT(f->is_decl, "direct mode marks function declared after JIT");
    TEST_ASSERT(f->first_block != NULL, "function block exists");
    TEST_ASSERT(f->first_block->first != NULL, "fast path mirrors emitted IR instructions");
    TEST_ASSERT_EQ(count_block_insts(f->first_block), 2,
                   "fast path block captures add+ret in IR");

    lr_session_destroy(s);
    if (prev_mode) {
        set_compile_mode_env(prev_mode);
    } else {
        set_compile_mode_env(NULL);
    }
    return 0;
#endif
}

int test_session_stream_isel_fast_path(void) {
#if !defined(__x86_64__) && !defined(_M_X64) && !defined(__aarch64__)
    return 0;
#else
    lr_session_config_t cfg = {0};
    lr_error_t err;
    const char *prev_mode = getenv("LIRIC_COMPILE_MODE");
    lr_module_t *m;
    lr_func_t *f;
    lr_session_t *s;
    lr_type_t *i32;
    lr_type_t *params[2];
    int rc;
    uint32_t a, b, sum;
    void *addr = NULL;
    typedef int (*fn_t)(int, int);
    fn_t fn;

    cfg.mode = LR_MODE_DIRECT;
    cfg.backend = LR_SESSION_BACKEND_ISEL;
    s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    i32 = lr_type_i32_s(s);
    params[0] = i32;
    params[1] = i32;
    rc = lr_session_func_begin(s, "session_stream_isel_fast", i32, params, 2, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");

    a = lr_session_param(s, 0);
    b = lr_session_param(s, 1);
    rc = lr_session_set_block(s, lr_session_block(s), &err);
    TEST_ASSERT_EQ(rc, 0, "set block");

    sum = lr_emit_add(s, i32, LR_VREG(a, i32), LR_VREG(b, i32));
    lr_emit_ret(s, LR_VREG(sum, i32));

    rc = lr_session_func_end(s, &addr, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");
    TEST_ASSERT(addr != NULL, "compiled function address");

    fn_ptr_cast(&fn, addr);
    TEST_ASSERT_EQ(fn(20, 22), 42, "stream isel add result");

    m = lr_session_module(s);
    f = find_func_by_name(m, "session_stream_isel_fast");
    TEST_ASSERT(f != NULL, "function exists in module");
    TEST_ASSERT(f->is_decl, "direct mode marks function declared after JIT");
    TEST_ASSERT(f->first_block != NULL, "function block exists");
    TEST_ASSERT(f->first_block->first != NULL, "isel fast path mirrors emitted IR instructions");
    TEST_ASSERT_EQ(count_block_insts(f->first_block), 2,
                   "isel fast path block captures add+ret in IR");

    lr_session_destroy(s);
    if (prev_mode) {
        set_compile_mode_env(prev_mode);
    } else {
        set_compile_mode_env(NULL);
    }
    return 0;
#endif
}

int test_session_direct_llvm_mode_stream_contract(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err = {0};
    const char *prev_mode = getenv("LIRIC_COMPILE_MODE");
    lr_session_t *s = NULL;
    lr_type_t *i32 = NULL;
    int rc = -1;
    int result = 1;
    char prev_mode_buf[64] = {0};

    if (prev_mode)
        (void)snprintf(prev_mode_buf, sizeof(prev_mode_buf), "%s", prev_mode);

    cfg.mode = LR_MODE_DIRECT;
    cfg.backend = LR_SESSION_BACKEND_LLVM;
    s = lr_session_create(&cfg, &err);
    if (!s) {
        fprintf(stderr, "  FAIL: session create (line %d)\n", __LINE__);
        goto cleanup;
    }

    i32 = lr_type_i32_s(s);
    if (!i32) {
        fprintf(stderr, "  FAIL: i32 type (line %d)\n", __LINE__);
        goto cleanup;
    }

    rc = lr_session_func_begin(s, "session_direct_llvm_stream", i32, NULL, 0, false, &err);
#if defined(LIRIC_HAVE_REAL_LLVM_BACKEND) && LIRIC_HAVE_REAL_LLVM_BACKEND
    if (!lr_llvm_jit_is_available()) {
        if (rc == 0) {
            fprintf(stderr, "  FAIL: func begin expected failure without LLJIT support (line %d)\n",
                    __LINE__);
            goto cleanup;
        }
        result = 0;
        goto cleanup;
    }
#else
    if (rc == 0) {
        fprintf(stderr, "  FAIL: func begin expected failure when backend disabled (line %d)\n",
                __LINE__);
        goto cleanup;
    }
    result = 0;
    goto cleanup;
#endif

    if (rc != 0) {
        fprintf(stderr, "  FAIL: func begin succeeds in DIRECT+llvm mode (line %d)\n", __LINE__);
        goto cleanup;
    }
    rc = lr_session_set_block(s, lr_session_block(s), &err);
    if (rc != 0) {
        fprintf(stderr, "  FAIL: set block succeeds (line %d)\n", __LINE__);
        goto cleanup;
    }
    lr_emit_ret(s, LR_IMM(42, i32));
    {
        void *addr = NULL;
        typedef int (*fn_t)(void);
        fn_t fn = NULL;
        rc = lr_session_func_end(s, &addr, &err);
        if (rc != 0 || !addr) {
            fprintf(stderr, "  FAIL: func end succeeds in DIRECT+llvm mode (line %d)\n",
                    __LINE__);
            goto cleanup;
        }
        fn_ptr_cast(&fn, addr);
        if (!fn || fn() != 42) {
            fprintf(stderr, "  FAIL: compiled function returns 42 (line %d)\n", __LINE__);
            goto cleanup;
        }
    }
    {
        lr_module_t *m = lr_session_module(s);
        lr_func_t *f = find_func_by_name(m, "session_direct_llvm_stream");
        if (!f || f->is_decl || !f->first_block || !f->first_block->first) {
            fprintf(stderr, "  FAIL: DIRECT+llvm retains LLVM-replay IR for module emission (line %d)\n",
                    __LINE__);
            goto cleanup;
        }
    }

    result = 0;

cleanup:
    if (s)
        lr_session_destroy(s);
    if (prev_mode && prev_mode[0]) {
        set_compile_mode_env(prev_mode_buf);
    } else {
        set_compile_mode_env(NULL);
    }
    return result;
}

int test_session_direct_llvm_forward_ref_lookup_contract(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err = {0};
    lr_session_t *s = NULL;
    lr_type_t *i32 = NULL;
    lr_type_t *ptr = NULL;
    uint32_t callee_sym;
    uint32_t call_vreg;
    int rc = -1;
    int result = 1;
    void *caller_addr = NULL;
    typedef int (*fn_t)(void);
    fn_t fn = NULL;

    cfg.mode = LR_MODE_DIRECT;
    cfg.backend = LR_SESSION_BACKEND_LLVM;
    s = lr_session_create(&cfg, &err);
    if (!s) {
        fprintf(stderr, "  FAIL: session create (line %d)\n", __LINE__);
        goto cleanup;
    }

    i32 = lr_type_i32_s(s);
    ptr = lr_type_ptr_s(s);
    if (!i32 || !ptr) {
        fprintf(stderr, "  FAIL: primitive types available (line %d)\n", __LINE__);
        goto cleanup;
    }

    rc = lr_session_func_begin(s, "session_direct_llvm_forward_caller",
                               i32, NULL, 0, false, &err);
#if defined(LIRIC_HAVE_REAL_LLVM_BACKEND) && LIRIC_HAVE_REAL_LLVM_BACKEND
    if (!lr_llvm_jit_is_available()) {
        if (rc == 0) {
            fprintf(stderr,
                    "  FAIL: func begin expected failure without LLJIT support (line %d)\n",
                    __LINE__);
            goto cleanup;
        }
        result = 0;
        goto cleanup;
    }
#else
    if (rc == 0) {
        fprintf(stderr,
                "  FAIL: func begin expected failure when backend disabled (line %d)\n",
                __LINE__);
        goto cleanup;
    }
    result = 0;
    goto cleanup;
#endif

    if (rc != 0) {
        fprintf(stderr, "  FAIL: caller func begin (line %d)\n", __LINE__);
        goto cleanup;
    }
    rc = lr_session_set_block(s, lr_session_block(s), &err);
    if (rc != 0) {
        fprintf(stderr, "  FAIL: caller set block (line %d)\n", __LINE__);
        goto cleanup;
    }

    callee_sym = lr_session_intern(s, "session_direct_llvm_forward_callee");
    call_vreg = lr_emit_call(s, i32, LR_GLOBAL(callee_sym, ptr), NULL, 0);
    lr_emit_ret(s, LR_VREG(call_vreg, i32));

    rc = lr_session_func_end(s, NULL, &err);
    if (rc != 0) {
        fprintf(stderr, "  FAIL: caller func end (line %d)\n", __LINE__);
        goto cleanup;
    }

    rc = lr_session_func_begin(s, "session_direct_llvm_forward_callee",
                               i32, NULL, 0, false, &err);
    if (rc != 0) {
        fprintf(stderr, "  FAIL: callee func begin (line %d)\n", __LINE__);
        goto cleanup;
    }
    rc = lr_session_set_block(s, lr_session_block(s), &err);
    if (rc != 0) {
        fprintf(stderr, "  FAIL: callee set block (line %d)\n", __LINE__);
        goto cleanup;
    }
    lr_emit_ret(s, LR_IMM(42, i32));

    rc = lr_session_func_end(s, NULL, &err);
    if (rc != 0) {
        fprintf(stderr, "  FAIL: callee func end (line %d)\n", __LINE__);
        goto cleanup;
    }

    caller_addr = lr_session_lookup(s, "session_direct_llvm_forward_caller");
    if (!caller_addr) {
        fprintf(stderr, "  FAIL: caller lookup (line %d)\n", __LINE__);
        goto cleanup;
    }
    fn_ptr_cast(&fn, caller_addr);
    if (!fn || fn() != 42) {
        fprintf(stderr, "  FAIL: caller returns 42 (line %d)\n", __LINE__);
        goto cleanup;
    }

    result = 0;

cleanup:
    if (s)
        lr_session_destroy(s);
    return result;
}

int test_session_direct_forward_ref_lookup_contract(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err = {0};
    lr_session_t *s = NULL;
    lr_type_t *i32 = NULL;
    lr_type_t *ptr = NULL;
    uint32_t callee_sym;
    uint32_t call_vreg;
    int rc = -1;
    void *caller_addr = NULL;
    typedef int (*fn_t)(void);
    fn_t fn = NULL;

    cfg.mode = LR_MODE_DIRECT;
    cfg.backend = LR_SESSION_BACKEND_ISEL;
    s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    i32 = lr_type_i32_s(s);
    ptr = lr_type_ptr_s(s);
    TEST_ASSERT(i32 != NULL, "i32 type");
    TEST_ASSERT(ptr != NULL, "ptr type");

    rc = lr_session_func_begin(s, "session_direct_forward_caller",
                               i32, NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "caller func begin");
    rc = lr_session_set_block(s, lr_session_block(s), &err);
    TEST_ASSERT_EQ(rc, 0, "caller set block");
    callee_sym = lr_session_intern(s, "session_direct_forward_callee");
    call_vreg = lr_emit_call(s, i32, LR_GLOBAL(callee_sym, ptr), NULL, 0);
    lr_emit_ret(s, LR_VREG(call_vreg, i32));
    rc = lr_session_func_end(s, NULL, &err);
    TEST_ASSERT_EQ(rc, 0, "caller func end");
    TEST_ASSERT(lr_session_lookup(s, "session_direct_forward_caller") == NULL,
                "caller lookup deferred while forward callee unresolved");

    rc = lr_session_func_begin(s, "session_direct_forward_callee",
                               i32, NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "callee func begin");
    rc = lr_session_set_block(s, lr_session_block(s), &err);
    TEST_ASSERT_EQ(rc, 0, "callee set block");
    lr_emit_ret(s, LR_IMM(42, i32));
    rc = lr_session_func_end(s, NULL, &err);
    TEST_ASSERT_EQ(rc, 0, "callee func end");

    caller_addr = lr_session_lookup(s, "session_direct_forward_caller");
    TEST_ASSERT(caller_addr != NULL, "caller lookup after callee definition");
    fn_ptr_cast(&fn, caller_addr);
    TEST_ASSERT(fn != NULL, "cast caller");
    TEST_ASSERT_EQ(fn(), 42, "caller returns 42");

    lr_session_destroy(s);
    return 0;
}

int test_session_direct_forward_global_lookup_contract(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err = {0};
    lr_session_t *s = NULL;
    lr_type_t *i64 = NULL;
    lr_type_t *ptr = NULL;
    uint32_t global_sym;
    uint32_t addr_vreg;
    uint32_t global_id;
    int64_t init_value = 123;
    int rc = -1;
    void *user_addr = NULL;
    void *global_addr = NULL;
    typedef uint64_t (*fn_t)(void);
    fn_t fn = NULL;

    cfg.mode = LR_MODE_DIRECT;
    cfg.backend = LR_SESSION_BACKEND_ISEL;
    s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    i64 = lr_type_i64_s(s);
    ptr = lr_type_ptr_s(s);
    TEST_ASSERT(i64 != NULL, "i64 type");
    TEST_ASSERT(ptr != NULL, "ptr type");

    rc = lr_session_func_begin(s, "session_direct_forward_global_user",
                               i64, NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "user func begin");
    rc = lr_session_set_block(s, lr_session_block(s), &err);
    TEST_ASSERT_EQ(rc, 0, "user set block");
    global_sym = lr_session_intern(s, "session_direct_forward_global_anchor");
    addr_vreg = lr_emit_ptrtoint(s, i64, LR_GLOBAL(global_sym, ptr));
    lr_emit_ret(s, LR_VREG(addr_vreg, i64));
    rc = lr_session_func_end(s, NULL, &err);
    TEST_ASSERT_EQ(rc, 0, "user func end");
    TEST_ASSERT(lr_session_lookup(s, "session_direct_forward_global_user") == NULL,
                "user lookup deferred while forward global unresolved");

    global_id = lr_session_global(s, "session_direct_forward_global_anchor", i64,
                                  false, &init_value, sizeof(init_value));
    TEST_ASSERT(global_id != UINT32_MAX, "global definition succeeds");

    user_addr = lr_session_lookup(s, "session_direct_forward_global_user");
    TEST_ASSERT(user_addr != NULL, "user lookup after global definition");
    global_addr = lr_session_lookup(s, "session_direct_forward_global_anchor");
    TEST_ASSERT(global_addr != NULL, "global symbol lookup after definition");
    fn_ptr_cast(&fn, user_addr);
    TEST_ASSERT(fn != NULL, "cast user");
    TEST_ASSERT_EQ(fn(), (uint64_t)(uintptr_t)global_addr,
                   "user returns resolved global address");

    lr_session_destroy(s);
    return 0;
}

int test_session_explicit_backend_overrides_env(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err = {0};
    const char *prev_mode = getenv("LIRIC_COMPILE_MODE");
    lr_session_t *s = NULL;
    lr_type_t *i32 = NULL;
    uint32_t b0;
    int rc;
    void *addr = NULL;
    typedef int (*fn_t)(void);
    fn_t fn = NULL;
    char prev_mode_buf[64] = {0};

    if (prev_mode)
        (void)snprintf(prev_mode_buf, sizeof(prev_mode_buf), "%s", prev_mode);

    set_compile_mode_env("llvm");
    cfg.mode = LR_MODE_DIRECT;
    cfg.backend = LR_SESSION_BACKEND_ISEL;

    s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create with explicit backend");

    i32 = lr_type_i32_s(s);
    TEST_ASSERT(i32 != NULL, "i32 type");

    rc = lr_session_func_begin(s, "session_explicit_backend_isel", i32,
                               NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");
    b0 = lr_session_block(s);
    rc = lr_session_set_block(s, b0, &err);
    TEST_ASSERT_EQ(rc, 0, "set block");
    lr_emit_ret(s, LR_IMM(42, i32));
    rc = lr_session_func_end(s, &addr, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");
    TEST_ASSERT(addr != NULL, "compiled function address");

    fn_ptr_cast(&fn, addr);
    TEST_ASSERT(fn != NULL, "cast function");
    TEST_ASSERT_EQ(fn(), 42, "session_explicit_backend_isel() == 42");

    lr_session_destroy(s);
    if (prev_mode && prev_mode[0]) {
        set_compile_mode_env(prev_mode_buf);
    } else {
        set_compile_mode_env(NULL);
    }
    return 0;
}

int test_session_stream_stencil_no_ir_fallback(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    const char *prev_mode = getenv("LIRIC_COMPILE_MODE");
    lr_module_t *m;
    lr_func_t *f;
    lr_block_t *b;
    lr_session_t *s;
    lr_type_t *i32;
    lr_type_t *i1;
    lr_type_t *params[2];
    int rc;
    uint32_t va, vb;
    uint32_t entry_id, then_id, else_id;
    uint32_t cmp;
    void *addr = NULL;
    typedef int (*fn_t)(int, int);
    fn_t fn;

    cfg.mode = LR_MODE_DIRECT;
    cfg.backend = LR_SESSION_BACKEND_COPY_PATCH;
    s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    i32 = lr_type_i32_s(s);
    i1 = lr_type_i1_s(s);
    params[0] = i32;
    params[1] = i32;

    rc = lr_session_func_begin(s, "session_stream_no_fallback", i32, params, 2, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");
    va = lr_session_param(s, 0);
    vb = lr_session_param(s, 1);

    entry_id = lr_session_block(s);
    then_id = lr_session_block(s);
    else_id = lr_session_block(s);

    rc = lr_session_set_block(s, entry_id, &err);
    TEST_ASSERT_EQ(rc, 0, "set entry block");
    cmp = lr_emit_icmp(s, LR_CMP_SGT, LR_VREG(va, i32), LR_VREG(vb, i32));
    lr_emit_condbr(s, LR_VREG(cmp, i1), then_id, else_id);

    rc = lr_session_set_block(s, then_id, &err);
    TEST_ASSERT_EQ(rc, 0, "set then block");
    lr_emit_ret(s, LR_VREG(va, i32));

    rc = lr_session_set_block(s, else_id, &err);
    TEST_ASSERT_EQ(rc, 0, "set else block");
    lr_emit_ret(s, LR_VREG(vb, i32));

    rc = lr_session_func_end(s, &addr, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");
    TEST_ASSERT(addr != NULL, "compiled function address");

    fn_ptr_cast(&fn, addr);
    TEST_ASSERT_EQ(fn(10, 3), 10, "branch returns lhs when greater");
    TEST_ASSERT_EQ(fn(2, 7), 7, "branch returns rhs when greater");

    m = lr_session_module(s);
    f = find_func_by_name(m, "session_stream_no_fallback");
    TEST_ASSERT(f != NULL, "function exists in module");
    TEST_ASSERT(f->first_block != NULL, "function has blocks");
    for (b = f->first_block; b; b = b->next)
        TEST_ASSERT(b->first != NULL, "DIRECT mode mirrors emitted IR instructions");
    TEST_ASSERT_EQ(count_func_insts(f), 4, "branch function records 4 IR instructions");

    lr_session_destroy(s);
    if (prev_mode) {
        set_compile_mode_env(prev_mode);
    } else {
        set_compile_mode_env(NULL);
    }
    return 0;
}

int test_session_add_phi_copy_api(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    lr_session_t *s = lr_session_create(&cfg, &err);
    lr_phi_copy_desc_t copy;
    lr_type_t *i32;
    uint32_t b0;
    int rc;
    void *addr = NULL;
    typedef int (*fn_t)(void);
    fn_t fn;

    TEST_ASSERT(s != NULL, "session create");
    i32 = lr_type_i32_s(s);
    TEST_ASSERT(i32 != NULL, "i32 type");

    rc = lr_session_func_begin(s, "session_phi_copy_api", i32, NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");

    b0 = lr_session_block(s);
    rc = lr_session_set_block(s, b0, &err);
    TEST_ASSERT_EQ(rc, 0, "set block");

    copy.dest_vreg = 999;
    copy.src_op = LR_IMM(7, i32);
    rc = lr_session_add_phi_copy(s, b0, b0, &copy, &err);
    TEST_ASSERT_EQ(rc, 0, "add phi copy");

    lr_emit_ret(s, LR_IMM(42, i32));
    rc = lr_session_func_end(s, &addr, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");
    TEST_ASSERT(addr != NULL, "compiled function address");

    fn_ptr_cast(&fn, addr);
    TEST_ASSERT_EQ(fn(), 42, "function result preserved after phi copy add");

    lr_session_destroy(s);
    return 0;
}

int test_session_icmp_branch(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    lr_type_t *i32 = lr_type_i32_s(s);
    lr_type_t *i1 = lr_type_i1_s(s);
    lr_type_t *params[] = {i32, i32};

    int rc = lr_session_func_begin(s, "session_max", i32, params, 2, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");

    uint32_t va = lr_session_param(s, 0);
    uint32_t vb = lr_session_param(s, 1);

    uint32_t entry_id = lr_session_block(s);
    uint32_t then_id = lr_session_block(s);
    uint32_t else_id = lr_session_block(s);

    lr_session_set_block(s, entry_id, &err);
    uint32_t cmp = lr_emit_icmp(s, LR_CMP_SGT, LR_VREG(va, i32), LR_VREG(vb, i32));
    lr_emit_condbr(s, LR_VREG(cmp, i1), then_id, else_id);

    lr_session_set_block(s, then_id, &err);
    lr_emit_ret(s, LR_VREG(va, i32));

    lr_session_set_block(s, else_id, &err);
    lr_emit_ret(s, LR_VREG(vb, i32));

    void *addr = NULL;
    rc = lr_session_func_end(s, &addr, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");

    typedef int (*fn_t)(int, int);
    fn_t fn;
    fn_ptr_cast(&fn, addr);
    TEST_ASSERT_EQ(fn(10, 5), 10, "max(10,5) == 10");
    TEST_ASSERT_EQ(fn(3, 7), 7, "max(3,7) == 7");
    TEST_ASSERT_EQ(fn(4, 4), 4, "max(4,4) == 4");

    lr_session_destroy(s);
    return 0;
}

int test_session_alloca_load_store(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    lr_type_t *i32 = lr_type_i32_s(s);
    lr_type_t *ptr = lr_type_ptr_s(s);

    int rc = lr_session_func_begin(s, "session_als", i32, NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");

    uint32_t b0 = lr_session_block(s);
    lr_session_set_block(s, b0, &err);

    uint32_t slot = lr_emit_alloca(s, i32);
    lr_emit_store(s, LR_IMM(99, i32), LR_VREG(slot, ptr));
    uint32_t val = lr_emit_load(s, i32, LR_VREG(slot, ptr));
    lr_emit_ret(s, LR_VREG(val, i32));

    void *addr = NULL;
    rc = lr_session_func_end(s, &addr, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");

    typedef int (*fn_t)(void);
    fn_t fn;
    fn_ptr_cast(&fn, addr);
    TEST_ASSERT_EQ(fn(), 99, "als() == 99");

    lr_session_destroy(s);
    return 0;
}

int test_session_loop_phi(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    lr_type_t *i32 = lr_type_i32_s(s);
    lr_type_t *i1 = lr_type_i1_s(s);

    int rc = lr_session_func_begin(s, "session_sum10", i32, NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");

    uint32_t entry_id = lr_session_block(s);
    uint32_t loop_id = lr_session_block(s);
    uint32_t exit_id = lr_session_block(s);

    lr_session_set_block(s, entry_id, &err);
    lr_emit_br(s, loop_id);

    lr_session_set_block(s, loop_id, &err);

    /* PHIs: i starts at 0, sum starts at 0.
       After body: next = i+1, sum_next = sum+next.
       PHIs reference forward vregs. With 0 params:
       phi_i = vreg 0, phi_s = vreg 1, next = vreg 2, sum_next = vreg 3 */
    lr_operand_desc_t phi_i_v[] = {LR_IMM(0, i32), LR_VREG(2, i32)};
    uint32_t phi_i_b[] = {entry_id, loop_id};
    uint32_t vi = lr_emit_phi(s, i32, phi_i_v, phi_i_b, 2);

    lr_operand_desc_t phi_s_v[] = {LR_IMM(0, i32), LR_VREG(3, i32)};
    uint32_t phi_s_b[] = {entry_id, loop_id};
    uint32_t vs = lr_emit_phi(s, i32, phi_s_v, phi_s_b, 2);

    uint32_t vnext = lr_emit_add(s, i32, LR_VREG(vi, i32), LR_IMM(1, i32));
    uint32_t vsum_next = lr_emit_add(s, i32, LR_VREG(vs, i32), LR_VREG(vnext, i32));

    uint32_t vdone = lr_emit_icmp(s, LR_CMP_EQ, LR_VREG(vnext, i32), LR_IMM(10, i32));
    lr_emit_condbr(s, LR_VREG(vdone, i1), exit_id, loop_id);

    lr_session_set_block(s, exit_id, &err);
    lr_emit_ret(s, LR_VREG(vsum_next, i32));

    void *addr = NULL;
    rc = lr_session_func_end(s, &addr, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");

    typedef int (*fn_t)(void);
    fn_t fn;
    fn_ptr_cast(&fn, addr);
    TEST_ASSERT_EQ(fn(), 55, "sum10() == 55");

    lr_session_destroy(s);
    return 0;
}

int test_session_call(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    lr_type_t *i32 = lr_type_i32_s(s);
    lr_type_t *ptr = lr_type_ptr_s(s);

    /* define i32 @helper(i32 %x) { ret i32 %x + 10 } */
    lr_type_t *h_params[] = {i32};
    int rc = lr_session_func_begin(s, "session_helper", i32, h_params, 1, false, &err);
    TEST_ASSERT_EQ(rc, 0, "helper func begin");
    uint32_t hx = lr_session_param(s, 0);
    uint32_t hb = lr_session_block(s);
    lr_session_set_block(s, hb, &err);
    uint32_t hr = lr_emit_add(s, i32, LR_VREG(hx, i32), LR_IMM(10, i32));
    lr_emit_ret(s, LR_VREG(hr, i32));
    void *helper_addr = NULL;
    rc = lr_session_func_end(s, &helper_addr, &err);
    TEST_ASSERT_EQ(rc, 0, "helper func end");

    /* define i32 @caller(i32 %a) { %r = call @helper(%a); ret i32 %r } */
    lr_type_t *c_params[] = {i32};
    rc = lr_session_func_begin(s, "session_caller", i32, c_params, 1, false, &err);
    TEST_ASSERT_EQ(rc, 0, "caller func begin");
    uint32_t ca = lr_session_param(s, 0);
    uint32_t cb = lr_session_block(s);
    lr_session_set_block(s, cb, &err);

    uint32_t helper_sym = lr_session_intern(s, "session_helper");
    lr_operand_desc_t args[] = {LR_VREG(ca, i32)};
    uint32_t cr = lr_emit_call(s, i32, LR_GLOBAL(helper_sym, ptr), args, 1);
    lr_emit_ret(s, LR_VREG(cr, i32));

    void *caller_addr = NULL;
    rc = lr_session_func_end(s, &caller_addr, &err);
    TEST_ASSERT_EQ(rc, 0, "caller func end");

    typedef int (*fn_t)(int);
    fn_t fn;
    fn_ptr_cast(&fn, caller_addr);
    TEST_ASSERT_EQ(fn(32), 42, "caller(32) == 42");

    lr_session_destroy(s);
    return 0;
}

int test_session_operand_global_offset_propagates_to_ir(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    lr_session_t *s;
    lr_type_t *i64;
    lr_type_t *ptr;
    lr_module_t *m;
    lr_func_t *f;
    lr_inst_t *inst;
    lr_operand_desc_t base;
    uint32_t gid;
    int rc;

    cfg.mode = LR_MODE_IR;
    s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    i64 = lr_type_i64_s(s);
    ptr = lr_type_ptr_s(s);
    gid = lr_session_global_extern(s, "session_global_offset_anchor", ptr);

    rc = lr_session_func_begin(s, "session_global_offset_ir", i64, NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");
    rc = lr_session_set_block(s, lr_session_block(s), &err);
    TEST_ASSERT_EQ(rc, 0, "set block");

    base = LR_GLOBAL(gid, ptr);
    base.global_offset = 24;
    lr_emit_ret(s, LR_VREG(lr_emit_ptrtoint(s, i64, base), i64));

    rc = lr_session_func_end(s, NULL, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");

    m = lr_session_module(s);
    f = find_func_by_name(m, "session_global_offset_ir");
    TEST_ASSERT(f != NULL, "function exists in module");
    TEST_ASSERT(f->first_block != NULL, "function has block");
    inst = f->first_block->first;
    TEST_ASSERT(inst != NULL, "function has first instruction");
    TEST_ASSERT_EQ(inst->op, LR_OP_PTRTOINT, "first instruction is ptrtoint");
    TEST_ASSERT_EQ(inst->num_operands, 1, "ptrtoint has one operand");
    TEST_ASSERT_EQ(inst->operands[0].kind, LR_VAL_GLOBAL, "operand kind is global");
    TEST_ASSERT_EQ(inst->operands[0].global_id, gid, "operand global id preserved");
    TEST_ASSERT_EQ(inst->operands[0].global_offset, 24, "operand global_offset preserved");

    lr_session_destroy(s);
    return 0;
}

int test_session_select(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    lr_type_t *i32 = lr_type_i32_s(s);
    lr_type_t *i1 = lr_type_i1_s(s);
    lr_type_t *params[] = {i32, i32};

    int rc = lr_session_func_begin(s, "session_sel_max", i32, params, 2, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");

    uint32_t va = lr_session_param(s, 0);
    uint32_t vb = lr_session_param(s, 1);

    uint32_t b0 = lr_session_block(s);
    lr_session_set_block(s, b0, &err);

    uint32_t cmp = lr_emit_icmp(s, LR_CMP_SGT, LR_VREG(va, i32), LR_VREG(vb, i32));
    uint32_t sel = lr_emit_select(s, i32, LR_VREG(cmp, i1),
                                  LR_VREG(va, i32), LR_VREG(vb, i32));
    lr_emit_ret(s, LR_VREG(sel, i32));

    void *addr = NULL;
    rc = lr_session_func_end(s, &addr, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");

    typedef int (*fn_t)(int, int);
    fn_t fn;
    fn_ptr_cast(&fn, addr);
    TEST_ASSERT_EQ(fn(10, 5), 10, "sel_max(10,5) == 10");
    TEST_ASSERT_EQ(fn(3, 7), 7, "sel_max(3,7) == 7");

    lr_session_destroy(s);
    return 0;
}

int test_session_ir_print(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    cfg.mode = LR_MODE_IR;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    lr_type_t *i32 = lr_type_i32_s(s);

    int rc = lr_session_func_begin(s, "session_ir_ret_7", i32, NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");

    uint32_t b0 = lr_session_block(s);
    lr_session_set_block(s, b0, &err);
    lr_emit_ret(s, LR_IMM(7, i32));

    void *addr = NULL;
    rc = lr_session_func_end(s, &addr, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");
    TEST_ASSERT(addr != NULL, "compiled function address");

    FILE *tmp = tmpfile();
    TEST_ASSERT(tmp != NULL, "tmpfile");
    rc = lr_session_dump_ir(s, tmp, &err);
    TEST_ASSERT_EQ(rc, 0, "ir dump");

    fseek(tmp, 0, SEEK_END);
    long len = ftell(tmp);
    TEST_ASSERT(len > 0, "ir dump produced output");
    fseek(tmp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)len + 1);
    TEST_ASSERT(buf != NULL, "alloc dump buf");
    size_t nread = fread(buf, 1, (size_t)len, tmp);
    TEST_ASSERT(nread == (size_t)len, "read dump");
    buf[len] = '\0';
    fclose(tmp);

    TEST_ASSERT(strstr(buf, "define i32 @session_ir_ret_7") != NULL,
                "ir output contains function");

    typedef int (*fn_t)(void);
    fn_t fn;
    fn_ptr_cast(&fn, addr);
    TEST_ASSERT_EQ(fn(), 7, "session_ir_ret_7() == 7");

    free(buf);
    lr_session_destroy(s);
    return 0;
}

int test_session_scalar_gep_undef_tail_trimmed(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    lr_session_t *s;
    lr_type_t *i8;
    lr_type_t *i64;
    lr_type_t *ptr;
    lr_operand_desc_t gep_indices[2];
    uint32_t slot;
    uint32_t gep;
    uint32_t p2i;
    int rc;
    FILE *tmp;
    long len;
    char *buf;
    size_t nread;

    cfg.mode = LR_MODE_IR;
    s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    i8 = lr_type_i8_s(s);
    i64 = lr_type_i64_s(s);
    ptr = lr_type_ptr_s(s);
    TEST_ASSERT(i8 != NULL, "i8 type");
    TEST_ASSERT(i64 != NULL, "i64 type");
    TEST_ASSERT(ptr != NULL, "ptr type");

    rc = lr_session_func_begin(s, "session_scalar_gep_trim", i64, NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");
    rc = lr_session_set_block(s, lr_session_block(s), &err);
    TEST_ASSERT_EQ(rc, 0, "set block");

    slot = lr_emit_alloca(s, i8);
    gep_indices[0] = LR_IMM(7, i64);
    gep_indices[1] = LR_UNDEF(i64);
    gep = lr_emit_gep(s, i8, LR_VREG(slot, ptr), gep_indices, 2);
    p2i = lr_emit_ptrtoint(s, i64, LR_VREG(gep, ptr));
    lr_emit_ret(s, LR_VREG(p2i, i64));

    rc = lr_session_func_end(s, NULL, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");

    tmp = tmpfile();
    TEST_ASSERT(tmp != NULL, "tmpfile");
    rc = lr_session_dump_ir(s, tmp, &err);
    TEST_ASSERT_EQ(rc, 0, "ir dump");

    fseek(tmp, 0, SEEK_END);
    len = ftell(tmp);
    TEST_ASSERT(len > 0, "ir dump produced output");
    fseek(tmp, 0, SEEK_SET);
    buf = (char *)malloc((size_t)len + 1u);
    TEST_ASSERT(buf != NULL, "alloc dump buf");
    nread = fread(buf, 1, (size_t)len, tmp);
    TEST_ASSERT(nread == (size_t)len, "read dump");
    buf[len] = '\0';
    fclose(tmp);

    TEST_ASSERT(strstr(buf, "getelementptr i8, ptr") != NULL,
                "ir output contains scalar gep");
    TEST_ASSERT(strstr(buf, ", i64 undef") == NULL,
                "scalar gep omits trailing undef index");

    free(buf);
    lr_session_destroy(s);
    return 0;
}

int test_ir_dump_scalar_gep_undef_tail_trimmed(void) {
    lr_arena_t *arena = NULL;
    lr_module_t *m = NULL;
    lr_func_t *f = NULL;
    lr_block_t *b = NULL;
    lr_inst_t *inst = NULL;
    lr_operand_t ops[3];
    FILE *tmp = NULL;
    long len;
    char *buf = NULL;
    size_t nread;

    arena = lr_arena_create(0);
    TEST_ASSERT(arena != NULL, "arena create");
    m = lr_module_create(arena);
    TEST_ASSERT(m != NULL, "module create");
    f = lr_func_create(m, "dump_scalar_gep_trim", m->type_i64, NULL, 0, false);
    TEST_ASSERT(f != NULL, "func create");
    b = lr_block_create(f, arena, "entry");
    TEST_ASSERT(b != NULL, "block create");

    inst = lr_inst_create(arena, LR_OP_ALLOCA, m->type_i8, 1, NULL, 0);
    TEST_ASSERT(inst != NULL, "alloca create");
    lr_block_append(b, inst);

    memset(ops, 0, sizeof(ops));
    ops[0].kind = LR_VAL_VREG;
    ops[0].vreg = 1;
    ops[0].type = m->type_ptr;
    ops[1].kind = LR_VAL_IMM_I64;
    ops[1].imm_i64 = 7;
    ops[1].type = m->type_i64;
    ops[2].kind = LR_VAL_UNDEF;
    ops[2].type = m->type_i64;
    inst = lr_inst_create(arena, LR_OP_GEP, m->type_i8, 2, ops, 3);
    TEST_ASSERT(inst != NULL, "gep create");
    lr_block_append(b, inst);

    memset(ops, 0, sizeof(ops));
    ops[0].kind = LR_VAL_VREG;
    ops[0].vreg = 2;
    ops[0].type = m->type_ptr;
    inst = lr_inst_create(arena, LR_OP_PTRTOINT, m->type_i64, 3, ops, 1);
    TEST_ASSERT(inst != NULL, "ptrtoint create");
    lr_block_append(b, inst);

    memset(ops, 0, sizeof(ops));
    ops[0].kind = LR_VAL_VREG;
    ops[0].vreg = 3;
    ops[0].type = m->type_i64;
    inst = lr_inst_create(arena, LR_OP_RET, m->type_i64, 0, ops, 1);
    TEST_ASSERT(inst != NULL, "ret create");
    lr_block_append(b, inst);

    tmp = tmpfile();
    TEST_ASSERT(tmp != NULL, "tmpfile");
    lr_dump_func(f, m, tmp);

    fseek(tmp, 0, SEEK_END);
    len = ftell(tmp);
    TEST_ASSERT(len > 0, "dump produced output");
    fseek(tmp, 0, SEEK_SET);
    buf = (char *)malloc((size_t)len + 1u);
    TEST_ASSERT(buf != NULL, "alloc dump buf");
    nread = fread(buf, 1, (size_t)len, tmp);
    TEST_ASSERT(nread == (size_t)len, "read dump");
    buf[len] = '\0';
    fclose(tmp);

    TEST_ASSERT(strstr(buf, "getelementptr i8, ptr") != NULL,
                "ir output contains scalar gep");
    TEST_ASSERT(strstr(buf, ", i64 undef") == NULL,
                "dump trims trailing scalar gep undef index");

    free(buf);
    lr_arena_destroy(arena);
    return 0;
}

int test_session_ir_lookup_prefers_module_symbol_over_process_symbol(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    lr_session_t *s;
    lr_type_t *i32;
    lr_type_t *params[1];
    int rc;
    void *addr = NULL;
    typedef int (*fn_t)(int);
    fn_t fn;

    cfg.mode = LR_MODE_IR;
    s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    i32 = lr_type_i32_s(s);
    TEST_ASSERT(i32 != NULL, "i32 type");
    params[0] = i32;

    rc = lr_session_func_begin(s, "abs", i32, params, 1, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");
    rc = lr_session_set_block(s, lr_session_block(s), &err);
    TEST_ASSERT_EQ(rc, 0, "set block");
    lr_emit_ret(s, LR_IMM(77, i32));
    rc = lr_session_func_end(s, NULL, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");

    addr = lr_session_lookup(s, "abs");
    TEST_ASSERT(addr != NULL, "lookup abs");
    fn_ptr_cast(&fn, addr);
    TEST_ASSERT_EQ(fn(-5), 77, "lookup resolves module-defined abs");
    TEST_ASSERT_EQ(fn(123), 77, "module-defined abs result remains stable");

    lr_session_destroy(s);
    return 0;
}

int test_session_ll_compile(void) {
    static const char *src =
        "define i32 @session_ll_ret_42() {\n"
        "entry:\n"
        "  ret i32 42\n"
        "}\n";
    lr_session_config_t cfg = {0};
    lr_error_t err;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    void *addr = NULL;
    int rc = lr_session_compile_ll(s, src, strlen(src), &addr, &err);
    TEST_ASSERT_EQ(rc, 0, "compile ll");
    TEST_ASSERT(addr != NULL, "ll compiled address");

    typedef int (*fn_t)(void);
    fn_t fn;
    fn_ptr_cast(&fn, addr);
    TEST_ASSERT_EQ(fn(), 42, "session_ll_ret_42() == 42");

    lr_session_destroy(s);
    return 0;
}

int test_session_bc_compile(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    void *addr = NULL;
    int rc = lr_session_compile_bc(s, bc_ret42_data, bc_ret42_len, &addr, &err);
    TEST_ASSERT_EQ(rc, 0, "compile bc");
    TEST_ASSERT(addr != NULL, "bc compiled address");

    typedef int (*fn_t)(void);
    fn_t fn;
    fn_ptr_cast(&fn, addr);
    TEST_ASSERT_EQ(fn(), 42, "session_bc_ret_42() == 42");

    lr_session_destroy(s);
    return 0;
}

int test_session_auto_compile_ll_and_bc(void) {
    static const char *src =
        "define i32 @session_auto_ll_ret_42() {\n"
        "entry:\n"
        "  ret i32 42\n"
        "}\n";
    lr_session_config_t cfg = {0};
    lr_error_t err;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    void *addr_ll = NULL;
    int rc = lr_session_compile_auto(s, (const uint8_t *)src, strlen(src), &addr_ll, &err);
    TEST_ASSERT_EQ(rc, 0, "compile auto ll");
    TEST_ASSERT(addr_ll != NULL, "auto ll compiled address");

    typedef int (*fn_t)(void);
    fn_t fn_ll;
    fn_ptr_cast(&fn_ll, addr_ll);
    TEST_ASSERT_EQ(fn_ll(), 42, "session_auto_ll_ret_42() == 42");

    void *addr_bc = NULL;
    rc = lr_session_compile_auto(s, bc_ret42_data, bc_ret42_len, &addr_bc, &err);
    TEST_ASSERT_EQ(rc, 0, "compile auto bc");
    TEST_ASSERT(addr_bc != NULL, "auto bc compiled address");

    fn_t fn_bc;
    fn_ptr_cast(&fn_bc, addr_bc);
    TEST_ASSERT_EQ(fn_bc(), 42, "session_auto_bc_ret_42() == 42");

    lr_session_destroy(s);
    return 0;
}

int test_session_multiple_functions(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    lr_type_t *i32 = lr_type_i32_s(s);

    /* First function: ret 1 */
    int rc = lr_session_func_begin(s, "session_f1", i32, NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "f1 begin");
    uint32_t b0 = lr_session_block(s);
    lr_session_set_block(s, b0, &err);
    lr_emit_ret(s, LR_IMM(1, i32));
    void *addr1 = NULL;
    rc = lr_session_func_end(s, &addr1, &err);
    TEST_ASSERT_EQ(rc, 0, "f1 end");

    /* Second function: ret 2 */
    rc = lr_session_func_begin(s, "session_f2", i32, NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "f2 begin");
    b0 = lr_session_block(s);
    lr_session_set_block(s, b0, &err);
    lr_emit_ret(s, LR_IMM(2, i32));
    void *addr2 = NULL;
    rc = lr_session_func_end(s, &addr2, &err);
    TEST_ASSERT_EQ(rc, 0, "f2 end");

    /* Third function: ret 3 */
    rc = lr_session_func_begin(s, "session_f3", i32, NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "f3 begin");
    b0 = lr_session_block(s);
    lr_session_set_block(s, b0, &err);
    lr_emit_ret(s, LR_IMM(3, i32));
    void *addr3 = NULL;
    rc = lr_session_func_end(s, &addr3, &err);
    TEST_ASSERT_EQ(rc, 0, "f3 end");

    typedef int (*fn_t)(void);
    fn_t fn1, fn2, fn3;
    fn_ptr_cast(&fn1, addr1);
    fn_ptr_cast(&fn2, addr2);
    fn_ptr_cast(&fn3, addr3);
    TEST_ASSERT_EQ(fn1(), 1, "f1() == 1");
    TEST_ASSERT_EQ(fn2(), 2, "f2() == 2");
    TEST_ASSERT_EQ(fn3(), 3, "f3() == 3");

    lr_session_destroy(s);
    return 0;
}

int test_session_emit_object_llvm_mode_contract(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err = {0};
    int rc = -1;
    int result = 1;
    const char *path = "/tmp/liric_test_session_emit_obj_llvm.o";

    cfg.mode = LR_MODE_IR;
    cfg.backend = LR_SESSION_BACKEND_LLVM;
    lr_session_t *s = lr_session_create(&cfg, &err);
    if (!s) {
        fprintf(stderr, "  FAIL: session create (%s)\n", err.msg);
        goto cleanup;
    }

    lr_type_t *i32 = lr_type_i32_s(s);
    if (!i32) {
        fprintf(stderr, "  FAIL: i32 type\n");
        goto cleanup;
    }

    rc = lr_session_func_begin(s, "main", i32, NULL, 0, false, &err);
    if (rc != 0) {
        fprintf(stderr, "  FAIL: func begin (%s)\n", err.msg);
        goto cleanup;
    }
    uint32_t b0 = lr_session_block(s);
    rc = lr_session_set_block(s, b0, &err);
    if (rc != 0) {
        fprintf(stderr, "  FAIL: set block (%s)\n", err.msg);
        goto cleanup;
    }
    lr_emit_ret(s, LR_IMM(42, i32));
    rc = lr_session_func_end(s, NULL, &err);
    if (rc != 0) {
        fprintf(stderr, "  FAIL: func end (%s)\n", err.msg);
        goto cleanup;
    }

    rc = lr_session_emit_object(s, path, &err);
#if defined(LIRIC_HAVE_REAL_LLVM_BACKEND) && LIRIC_HAVE_REAL_LLVM_BACKEND
    if (rc != 0) {
        fprintf(stderr, "  FAIL: llvm mode object emission expected success (%s)\n", err.msg);
        goto cleanup;
    }
#else
    if (rc == 0) {
        fprintf(stderr, "  FAIL: llvm mode object emission expected failure when backend disabled\n");
        goto cleanup;
    }
#endif

    result = 0;

cleanup:
    remove(path);
    if (s)
        lr_session_destroy(s);
    return result;
}

int test_session_blob_export_ir_mode_contract(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err = {0};
    uint8_t *pkg = NULL;
    size_t pkg_len = 0;
    int rc;

    cfg.mode = LR_MODE_IR;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    rc = lr_session_export_blob_package(s, &pkg, &pkg_len, &err);
    TEST_ASSERT_EQ(rc, 0, "export blob package in IR mode");
    TEST_ASSERT(pkg != NULL, "blob package buffer allocated");
    TEST_ASSERT_EQ((int)pkg_len, 16, "empty blob package has header-only size");
    TEST_ASSERT(memcmp(pkg, "LRBLOB1\0", 8) == 0, "blob package magic");
    TEST_ASSERT(pkg[8] == 1 && pkg[9] == 0 && pkg[10] == 0 && pkg[11] == 0,
                "blob package version=1");
    TEST_ASSERT(pkg[12] == 0 && pkg[13] == 0 && pkg[14] == 0 && pkg[15] == 0,
                "blob package blob_count=0");

    free(pkg);
    lr_session_destroy(s);
    return 0;
}

#if defined(__linux__)

static int run_exe_expect(const char *path, int expected_rc) {
    if (chmod(path, 0755) != 0) return -1;
    int status = system(path);
    if (!WIFEXITED(status)) return -2;
    int actual = WEXITSTATUS(status);
    if (actual != expected_rc) {
        fprintf(stderr, "    expected exit %d, got %d\n", expected_rc, actual);
        return -3;
    }
    return 0;
}

int test_session_ir_exe_ret_42(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    cfg.mode = LR_MODE_IR;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    lr_type_t *i32 = lr_type_i32_s(s);
    int rc = lr_session_func_begin(s, "_start", i32, NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");

    uint32_t b0 = lr_session_block(s);
    lr_session_set_block(s, b0, &err);
    lr_emit_ret(s, LR_IMM(42, i32));

    rc = lr_session_func_end(s, NULL, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");

    const char *path = "/tmp/liric_test_ir_ret42";
    rc = lr_session_emit_exe(s, path, &err);
    TEST_ASSERT_EQ(rc, 0, "emit exe");

    rc = run_exe_expect(path, 42);
    TEST_ASSERT_EQ(rc, 0, "exe exit code 42");

    remove(path);
    lr_session_destroy(s);
    return 0;
}

int test_session_ir_exe_branch(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    cfg.mode = LR_MODE_IR;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    lr_type_t *i32 = lr_type_i32_s(s);
    lr_type_t *i1 = lr_type_i1_s(s);

    int rc = lr_session_func_begin(s, "_start", i32, NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");

    uint32_t entry_id = lr_session_block(s);
    uint32_t then_id = lr_session_block(s);
    uint32_t else_id = lr_session_block(s);

    lr_session_set_block(s, entry_id, &err);
    uint32_t cmp = lr_emit_icmp(s, LR_CMP_SGT, LR_IMM(7, i32), LR_IMM(5, i32));
    lr_emit_condbr(s, LR_VREG(cmp, i1), then_id, else_id);

    lr_session_set_block(s, then_id, &err);
    lr_emit_ret(s, LR_IMM(10, i32));

    lr_session_set_block(s, else_id, &err);
    lr_emit_ret(s, LR_IMM(20, i32));

    rc = lr_session_func_end(s, NULL, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");

    const char *path = "/tmp/liric_test_ir_branch";
    rc = lr_session_emit_exe(s, path, &err);
    TEST_ASSERT_EQ(rc, 0, "emit exe");

    rc = run_exe_expect(path, 10);
    TEST_ASSERT_EQ(rc, 0, "exe exit code 10");

    remove(path);
    lr_session_destroy(s);
    return 0;
}

int test_session_ir_exe_call(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    cfg.mode = LR_MODE_IR;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    lr_type_t *i32 = lr_type_i32_s(s);
    lr_type_t *ptr = lr_type_ptr_s(s);

    lr_type_t *h_params[] = {i32};
    int rc = lr_session_func_begin(s, "helper", i32, h_params, 1, false, &err);
    TEST_ASSERT_EQ(rc, 0, "helper func begin");
    uint32_t hx = lr_session_param(s, 0);
    uint32_t hb = lr_session_block(s);
    lr_session_set_block(s, hb, &err);
    uint32_t hr = lr_emit_add(s, i32, LR_VREG(hx, i32), LR_IMM(10, i32));
    lr_emit_ret(s, LR_VREG(hr, i32));
    rc = lr_session_func_end(s, NULL, &err);
    TEST_ASSERT_EQ(rc, 0, "helper func end");

    rc = lr_session_func_begin(s, "_start", i32, NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "_start func begin");
    uint32_t sb = lr_session_block(s);
    lr_session_set_block(s, sb, &err);
    uint32_t helper_sym = lr_session_intern(s, "helper");
    lr_operand_desc_t args[] = {LR_IMM(32, i32)};
    uint32_t cr = lr_emit_call(s, i32, LR_GLOBAL(helper_sym, ptr), args, 1);
    lr_emit_ret(s, LR_VREG(cr, i32));
    rc = lr_session_func_end(s, NULL, &err);
    TEST_ASSERT_EQ(rc, 0, "_start func end");

    const char *path = "/tmp/liric_test_ir_call";
    rc = lr_session_emit_exe(s, path, &err);
    TEST_ASSERT_EQ(rc, 0, "emit exe");

    rc = run_exe_expect(path, 42);
    TEST_ASSERT_EQ(rc, 0, "exe exit code 42");

    remove(path);
    lr_session_destroy(s);
    return 0;
}

int test_session_ir_exe_loop(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    cfg.mode = LR_MODE_IR;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    lr_type_t *i32 = lr_type_i32_s(s);
    lr_type_t *i1 = lr_type_i1_s(s);

    int rc = lr_session_func_begin(s, "_start", i32, NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");

    uint32_t entry_id = lr_session_block(s);
    uint32_t loop_id = lr_session_block(s);
    uint32_t exit_id = lr_session_block(s);

    lr_session_set_block(s, entry_id, &err);
    lr_emit_br(s, loop_id);

    lr_session_set_block(s, loop_id, &err);
    lr_operand_desc_t phi_i_v[] = {LR_IMM(0, i32), LR_VREG(2, i32)};
    uint32_t phi_i_b[] = {entry_id, loop_id};
    uint32_t vi = lr_emit_phi(s, i32, phi_i_v, phi_i_b, 2);

    lr_operand_desc_t phi_s_v[] = {LR_IMM(0, i32), LR_VREG(3, i32)};
    uint32_t phi_s_b[] = {entry_id, loop_id};
    uint32_t vs = lr_emit_phi(s, i32, phi_s_v, phi_s_b, 2);

    uint32_t vnext = lr_emit_add(s, i32, LR_VREG(vi, i32), LR_IMM(1, i32));
    uint32_t vsum_next = lr_emit_add(s, i32, LR_VREG(vs, i32), LR_VREG(vnext, i32));

    uint32_t vdone = lr_emit_icmp(s, LR_CMP_EQ, LR_VREG(vnext, i32), LR_IMM(10, i32));
    lr_emit_condbr(s, LR_VREG(vdone, i1), exit_id, loop_id);

    lr_session_set_block(s, exit_id, &err);
    lr_emit_ret(s, LR_VREG(vsum_next, i32));

    rc = lr_session_func_end(s, NULL, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");

    const char *path = "/tmp/liric_test_ir_loop";
    rc = lr_session_emit_exe(s, path, &err);
    TEST_ASSERT_EQ(rc, 0, "emit exe");

    rc = run_exe_expect(path, 55);
    TEST_ASSERT_EQ(rc, 0, "exe exit code 55");

    remove(path);
    lr_session_destroy(s);
    return 0;
}

int test_session_direct_exe_ret_42(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    cfg.mode = LR_MODE_DIRECT;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    lr_type_t *i32 = lr_type_i32_s(s);
    int rc = lr_session_func_begin(s, "_start", i32, NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");

    uint32_t b0 = lr_session_block(s);
    lr_session_set_block(s, b0, &err);
    lr_emit_ret(s, LR_IMM(42, i32));

    rc = lr_session_func_end(s, NULL, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");

    const char *path = "/tmp/liric_test_direct_ret42";
    rc = lr_session_emit_exe(s, path, &err);
    TEST_ASSERT_EQ(rc, 0, "emit exe");

    rc = run_exe_expect(path, 42);
    TEST_ASSERT_EQ(rc, 0, "exe exit code 42");

    remove(path);
    lr_session_destroy(s);
    return 0;
}

int test_session_direct_exe_branch(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    cfg.mode = LR_MODE_DIRECT;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    lr_type_t *i32 = lr_type_i32_s(s);
    lr_type_t *i1 = lr_type_i1_s(s);

    int rc = lr_session_func_begin(s, "_start", i32, NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");

    uint32_t entry_id = lr_session_block(s);
    uint32_t then_id = lr_session_block(s);
    uint32_t else_id = lr_session_block(s);

    lr_session_set_block(s, entry_id, &err);
    uint32_t cmp = lr_emit_icmp(s, LR_CMP_SGT, LR_IMM(7, i32), LR_IMM(5, i32));
    lr_emit_condbr(s, LR_VREG(cmp, i1), then_id, else_id);

    lr_session_set_block(s, then_id, &err);
    lr_emit_ret(s, LR_IMM(10, i32));

    lr_session_set_block(s, else_id, &err);
    lr_emit_ret(s, LR_IMM(20, i32));

    rc = lr_session_func_end(s, NULL, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");

    const char *path = "/tmp/liric_test_direct_branch";
    rc = lr_session_emit_exe(s, path, &err);
    TEST_ASSERT_EQ(rc, 0, "emit exe");

    rc = run_exe_expect(path, 10);
    TEST_ASSERT_EQ(rc, 0, "exe exit code 10");

    remove(path);
    lr_session_destroy(s);
    return 0;
}

int test_session_direct_exe_call(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    cfg.mode = LR_MODE_DIRECT;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    lr_type_t *i32 = lr_type_i32_s(s);
    lr_type_t *ptr = lr_type_ptr_s(s);

    lr_type_t *h_params[] = {i32};
    int rc = lr_session_func_begin(s, "helper", i32, h_params, 1, false, &err);
    TEST_ASSERT_EQ(rc, 0, "helper func begin");
    uint32_t hx = lr_session_param(s, 0);
    uint32_t hb = lr_session_block(s);
    lr_session_set_block(s, hb, &err);
    uint32_t hr = lr_emit_add(s, i32, LR_VREG(hx, i32), LR_IMM(10, i32));
    lr_emit_ret(s, LR_VREG(hr, i32));
    rc = lr_session_func_end(s, NULL, &err);
    TEST_ASSERT_EQ(rc, 0, "helper func end");

    rc = lr_session_func_begin(s, "_start", i32, NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "_start func begin");
    uint32_t sb = lr_session_block(s);
    lr_session_set_block(s, sb, &err);
    uint32_t helper_sym = lr_session_intern(s, "helper");
    lr_operand_desc_t args[] = {LR_IMM(32, i32)};
    uint32_t cr = lr_emit_call(s, i32, LR_GLOBAL(helper_sym, ptr), args, 1);
    lr_emit_ret(s, LR_VREG(cr, i32));
    rc = lr_session_func_end(s, NULL, &err);
    TEST_ASSERT_EQ(rc, 0, "_start func end");

    const char *path = "/tmp/liric_test_direct_call";
    rc = lr_session_emit_exe(s, path, &err);
    TEST_ASSERT_EQ(rc, 0, "emit exe");

    rc = run_exe_expect(path, 42);
    TEST_ASSERT_EQ(rc, 0, "exe exit code 42");

    remove(path);
    lr_session_destroy(s);
    return 0;
}

int test_session_direct_jit_and_exe(void) {
    lr_session_config_t cfg = {0};
    lr_error_t err;
    cfg.mode = LR_MODE_DIRECT;
    lr_session_t *s = lr_session_create(&cfg, &err);
    TEST_ASSERT(s != NULL, "session create");

    lr_type_t *i32 = lr_type_i32_s(s);

    /* Compile a function and JIT-execute it */
    int rc = lr_session_func_begin(s, "_start", i32, NULL, 0, false, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");
    uint32_t b0 = lr_session_block(s);
    lr_session_set_block(s, b0, &err);
    lr_emit_ret(s, LR_IMM(99, i32));
    void *addr = NULL;
    rc = lr_session_func_end(s, &addr, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");
    TEST_ASSERT(addr != NULL, "jit addr");

    /* Verify JIT works */
    typedef int (*fn_t)(void);
    fn_t fn;
    memcpy(&fn, &addr, sizeof(fn));
    TEST_ASSERT_EQ(fn(), 99, "jit call returns 99");

    /* Also emit as executable */
    const char *path = "/tmp/liric_test_direct_jit_and_exe";
    rc = lr_session_emit_exe(s, path, &err);
    TEST_ASSERT_EQ(rc, 0, "emit exe");

    rc = run_exe_expect(path, 99);
    TEST_ASSERT_EQ(rc, 0, "exe exit code 99");

    remove(path);
    lr_session_destroy(s);
    return 0;
}

#endif
