#include <liric/liric_compile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

static inline void fn_ptr_cast(void *dst, void *src) {
    memcpy(dst, &src, sizeof(src));
}

typedef struct test_buf {
    char *data;
    size_t len;
    size_t cap;
} test_buf_t;

static int write_to_buf(void *user, const char *data, size_t len) {
    test_buf_t *buf = (test_buf_t *)user;
    size_t new_len = 0;
    char *new_data = NULL;
    if (!buf || !data)
        return -1;
    new_len = buf->len + len;
    if (new_len + 1 > buf->cap) {
        size_t new_cap = buf->cap == 0 ? 1024u : buf->cap;
        while (new_cap < new_len + 1)
            new_cap *= 2u;
        new_data = (char *)realloc(buf->data, new_cap);
        if (!new_data)
            return -1;
        buf->data = new_data;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, data, len);
    buf->len = new_len;
    buf->data[buf->len] = '\0';
    return 0;
}

int test_compile_session_direct_ret_42(void) {
    lr_compile_config_t cfg;
    lr_compile_error_t err;
    lr_compile_session_t *s = NULL;
    lr_type_t *i32 = NULL;
    lr_function_spec_t spec;
    lr_operand_desc_t ret_ops[1];
    lr_inst_desc_t ret_inst;
    lr_symbol_handle_t sym;
    typedef int (*fn_t)(void);
    fn_t fn = NULL;
    int rc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.strategy = LR_COMPILE_STRATEGY_DIRECT_PASS;
    s = lr_compile_begin(&cfg, &err);
    TEST_ASSERT(s != NULL, "compile session create");

    i32 = lr_compile_type_i32(s);
    TEST_ASSERT(i32 != NULL, "i32 type");

    memset(&spec, 0, sizeof(spec));
    spec.name = "compile_direct_ret_42";
    spec.ret_type = i32;
    rc = lr_func_begin(s, &spec, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");

    rc = lr_block_begin(s, 0, &err);
    TEST_ASSERT_EQ(rc, 0, "block begin");

    ret_ops[0] = LR_IMM(42, i32);
    memset(&ret_inst, 0, sizeof(ret_inst));
    ret_inst.op = LR_OP_RET;
    ret_inst.type = i32;
    ret_inst.operands = ret_ops;
    ret_inst.num_operands = 1;
    rc = lr_emit(s, &ret_inst, &err);
    TEST_ASSERT_EQ(rc, 0, "emit ret");

    rc = lr_block_seal(s, 0, &err);
    TEST_ASSERT_EQ(rc, 0, "block seal");

    memset(&sym, 0, sizeof(sym));
    rc = lr_func_end(s, &sym, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");
    TEST_ASSERT(sym.addr != NULL, "compiled function address");

    fn_ptr_cast(&fn, sym.addr);
    TEST_ASSERT(fn != NULL, "function pointer cast");
    TEST_ASSERT_EQ(fn(), 42, "compiled function return value");

    lr_compile_end(s);
    return 0;
}

int test_compile_session_ir_print_and_opt(void) {
    lr_compile_config_t cfg;
    lr_compile_error_t err;
    lr_compile_session_t *s = NULL;
    lr_type_t *i32 = NULL;
    lr_function_spec_t spec;
    lr_operand_desc_t ret_ops[1];
    lr_inst_desc_t ret_inst;
    lr_symbol_handle_t sym;
    lr_ir_pipeline_t pipe;
    test_buf_t out;
    typedef int (*fn_t)(void);
    fn_t fn = NULL;
    int rc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.strategy = LR_COMPILE_STRATEGY_IR_MODE;
    cfg.enable_ir_pipeline = true;
    s = lr_compile_begin(&cfg, &err);
    TEST_ASSERT(s != NULL, "compile session create");

    i32 = lr_compile_type_i32(s);
    TEST_ASSERT(i32 != NULL, "i32 type");

    memset(&spec, 0, sizeof(spec));
    spec.name = "compile_ir_ret_7";
    spec.ret_type = i32;
    rc = lr_func_begin(s, &spec, &err);
    TEST_ASSERT_EQ(rc, 0, "func begin");

    rc = lr_block_begin(s, 0, &err);
    TEST_ASSERT_EQ(rc, 0, "block begin");

    ret_ops[0] = LR_IMM(7, i32);
    memset(&ret_inst, 0, sizeof(ret_inst));
    ret_inst.op = LR_OP_RET;
    ret_inst.type = i32;
    ret_inst.operands = ret_ops;
    ret_inst.num_operands = 1;
    rc = lr_emit(s, &ret_inst, &err);
    TEST_ASSERT_EQ(rc, 0, "emit ret");

    rc = lr_block_seal(s, 0, &err);
    TEST_ASSERT_EQ(rc, 0, "block seal");

    memset(&sym, 0, sizeof(sym));
    rc = lr_func_end(s, &sym, &err);
    TEST_ASSERT_EQ(rc, 0, "func end");
    TEST_ASSERT(sym.addr != NULL, "compiled function address");

    memset(&pipe, 0, sizeof(pipe));
    pipe.opt_level = 2;
    pipe.constant_propagation = true;
    rc = lr_ir_optimize(s, &pipe, &err);
    TEST_ASSERT_EQ(rc, 0, "ir optimize");

    memset(&out, 0, sizeof(out));
    rc = lr_ir_print(s, write_to_buf, &out, &err);
    TEST_ASSERT_EQ(rc, 0, "ir print");
    TEST_ASSERT(out.data != NULL, "ir print output");
    TEST_ASSERT(strstr(out.data, "define i32 @compile_ir_ret_7") != NULL,
                "ir output contains function");

    fn_ptr_cast(&fn, sym.addr);
    TEST_ASSERT(fn != NULL, "function pointer cast");
    TEST_ASSERT_EQ(fn(), 7, "compiled function return value");

    free(out.data);
    lr_compile_end(s);
    return 0;
}

int test_compile_session_ll_compile_ret_42(void) {
    static const char *src =
        "define i32 @compile_ll_ret_42() {\n"
        "entry:\n"
        "  ret i32 42\n"
        "}\n";
    lr_compile_config_t cfg;
    lr_compile_error_t err;
    lr_compile_session_t *s = NULL;
    lr_symbol_handle_t sym;
    typedef int (*fn_t)(void);
    fn_t fn = NULL;
    int rc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.strategy = LR_COMPILE_STRATEGY_DIRECT_PASS;
    s = lr_compile_begin(&cfg, &err);
    TEST_ASSERT(s != NULL, "compile session create");

    memset(&sym, 0, sizeof(sym));
    rc = lr_compile_ll(s, src, strlen(src), &sym, &err);
    TEST_ASSERT_EQ(rc, 0, "compile ll");
    TEST_ASSERT(sym.name != NULL, "ll symbol name");
    TEST_ASSERT(sym.addr != NULL, "ll symbol address");
    TEST_ASSERT(strcmp(sym.name, "compile_ll_ret_42") == 0,
                "ll symbol name match");

    fn_ptr_cast(&fn, sym.addr);
    TEST_ASSERT(fn != NULL, "function pointer cast");
    TEST_ASSERT_EQ(fn(), 42, "ll compiled function return value");

    lr_compile_end(s);
    return 0;
}
