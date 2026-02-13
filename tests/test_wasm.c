#include "../src/arena.h"
#include "../src/ir.h"
#include "../src/wasm_decode.h"
#include "../src/wasm_to_ir.h"
#include "../src/liric.h"
#include "../src/jit.h"
#include <liric/liric_session.h>
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

/* ---- LEB128 tests ---- */

int test_wasm_leb128_u32(void) {
    uint32_t val;
    size_t n;

    /* 0 encodes as 0x00 */
    uint8_t b0[] = {0x00};
    n = lr_wasm_read_leb_u32(b0, 1, &val);
    TEST_ASSERT_EQ(n, 1, "leb u32 0 bytes");
    TEST_ASSERT_EQ(val, 0, "leb u32 0");

    /* 127 encodes as 0x7F */
    uint8_t b127[] = {0x7F};
    n = lr_wasm_read_leb_u32(b127, 1, &val);
    TEST_ASSERT_EQ(n, 1, "leb u32 127 bytes");
    TEST_ASSERT_EQ(val, 127, "leb u32 127");

    /* 128 encodes as 0x80, 0x01 */
    uint8_t b128[] = {0x80, 0x01};
    n = lr_wasm_read_leb_u32(b128, 2, &val);
    TEST_ASSERT_EQ(n, 2, "leb u32 128 bytes");
    TEST_ASSERT_EQ(val, 128, "leb u32 128");

    /* 624485 encodes as 0xE5, 0x8E, 0x26 */
    uint8_t b624485[] = {0xE5, 0x8E, 0x26};
    n = lr_wasm_read_leb_u32(b624485, 3, &val);
    TEST_ASSERT_EQ(n, 3, "leb u32 624485 bytes");
    TEST_ASSERT_EQ(val, 624485, "leb u32 624485");

    return 0;
}

int test_wasm_leb128_i32(void) {
    int32_t val;
    size_t n;

    /* 0 encodes as 0x00 */
    uint8_t b0[] = {0x00};
    n = lr_wasm_read_leb_i32(b0, 1, &val);
    TEST_ASSERT_EQ(n, 1, "leb i32 0 bytes");
    TEST_ASSERT_EQ(val, 0, "leb i32 0");

    /* -1 encodes as 0x7F */
    uint8_t bm1[] = {0x7F};
    n = lr_wasm_read_leb_i32(bm1, 1, &val);
    TEST_ASSERT_EQ(n, 1, "leb i32 -1 bytes");
    TEST_ASSERT_EQ(val, -1, "leb i32 -1");

    /* -128 encodes as 0x80, 0x7F */
    uint8_t bm128[] = {0x80, 0x7F};
    n = lr_wasm_read_leb_i32(bm128, 2, &val);
    TEST_ASSERT_EQ(n, 2, "leb i32 -128 bytes");
    TEST_ASSERT_EQ(val, -128, "leb i32 -128");

    /* 127 encodes as 0xFF, 0x00 (needs extra byte for sign) */
    uint8_t b127[] = {0xFF, 0x00};
    n = lr_wasm_read_leb_i32(b127, 2, &val);
    TEST_ASSERT_EQ(n, 2, "leb i32 127 bytes");
    TEST_ASSERT_EQ(val, 127, "leb i32 127");

    return 0;
}

int test_wasm_leb128_i64(void) {
    int64_t val;
    size_t n;

    uint8_t b0[] = {0x00};
    n = lr_wasm_read_leb_i64(b0, 1, &val);
    TEST_ASSERT_EQ(n, 1, "leb i64 0 bytes");
    TEST_ASSERT_EQ(val, 0, "leb i64 0");

    uint8_t bm1[] = {0x7F};
    n = lr_wasm_read_leb_i64(bm1, 1, &val);
    TEST_ASSERT_EQ(n, 1, "leb i64 -1 bytes");
    TEST_ASSERT_EQ(val, -1, "leb i64 -1");

    return 0;
}

/* ---- Decoder tests ---- */

int test_wasm_decode_minimal(void) {
    /* Minimal valid WASM: magic + version + empty type section */
    uint8_t wasm[] = {
        0x00, 0x61, 0x73, 0x6D, /* magic: \0asm */
        0x01, 0x00, 0x00, 0x00, /* version: 1 */
        0x01,                   /* section id: type */
        0x01,                   /* section length: 1 */
        0x00,                   /* 0 types */
    };
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};
    lr_wasm_module_t *m = lr_wasm_decode(wasm, sizeof(wasm), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, "decode minimal");
    TEST_ASSERT_EQ(m->num_types, 0, "0 types");
    lr_arena_destroy(arena);
    return 0;
}

int test_wasm_decode_add(void) {
    /* Module with one function: (i32, i32) -> i32, body = local.get 0 + local.get 1 */
    uint8_t wasm[] = {
        0x00, 0x61, 0x73, 0x6D,  /* magic */
        0x01, 0x00, 0x00, 0x00,  /* version */
        /* Type section */
        0x01,       /* sec id: type */
        0x07,       /* sec len */
        0x01,       /* 1 type */
        0x60,       /* functype */
        0x02, 0x7F, 0x7F,  /* 2 params: i32, i32 */
        0x01, 0x7F,        /* 1 result: i32 */
        /* Function section */
        0x03,       /* sec id: function */
        0x02,       /* sec len */
        0x01,       /* 1 function */
        0x00,       /* type index 0 */
        /* Export section */
        0x07,       /* sec id: export */
        0x07,       /* sec len */
        0x01,       /* 1 export */
        0x03, 'a', 'd', 'd',  /* name: "add" */
        0x00,       /* kind: func */
        0x00,       /* index: 0 */
        /* Code section */
        0x0A,       /* sec id: code */
        0x09,       /* sec len */
        0x01,       /* 1 code entry */
        0x07,       /* body size */
        0x00,       /* 0 local groups */
        0x20, 0x00, /* local.get 0 */
        0x20, 0x01, /* local.get 1 */
        0x6A,       /* i32.add */
        0x0B,       /* end */
    };
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};
    lr_wasm_module_t *m = lr_wasm_decode(wasm, sizeof(wasm), arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, "decode add");
    TEST_ASSERT_EQ(m->num_types, 1, "1 type");
    TEST_ASSERT_EQ(m->types[0].num_params, 2, "2 params");
    TEST_ASSERT_EQ(m->types[0].num_results, 1, "1 result");
    TEST_ASSERT_EQ(m->num_funcs, 1, "1 func");
    TEST_ASSERT_EQ(m->num_exports, 1, "1 export");
    TEST_ASSERT(strcmp(m->exports[0].name, "add") == 0, "export name");
    TEST_ASSERT_EQ(m->num_codes, 1, "1 code");
    TEST_ASSERT_EQ(m->codes[0].body_len, 6, "body len");
    lr_arena_destroy(arena);
    return 0;
}

int test_wasm_decode_invalid_magic(void) {
    uint8_t bad[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x00, 0x00, 0x00};
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};
    lr_wasm_module_t *m = lr_wasm_decode(bad, sizeof(bad), arena, err, sizeof(err));
    TEST_ASSERT(m == NULL, "reject invalid magic");
    TEST_ASSERT(strlen(err) > 0, "error message set");
    lr_arena_destroy(arena);
    return 0;
}

/* ---- IR conversion tests ---- */

int test_wasm_ir_ret_42(void) {
    /* Function returning constant 42 */
    uint8_t wasm[] = {
        0x00, 0x61, 0x73, 0x6D,
        0x01, 0x00, 0x00, 0x00,
        /* Type: () -> i32 */
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7F,
        /* Function: type 0 */
        0x03, 0x02, 0x01, 0x00,
        /* Export: "f" -> func 0 */
        0x07, 0x05, 0x01, 0x01, 'f', 0x00, 0x00,
        /* Code: i32.const 42, end */
        0x0A, 0x06, 0x01, 0x04, 0x00, 0x41, 0x2A, 0x0B,
    };
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};
    lr_wasm_module_t *wmod = lr_wasm_decode(wasm, sizeof(wasm), arena, err, sizeof(err));
    TEST_ASSERT(wmod != NULL, "decode ret_42");
    lr_module_t *m = lr_wasm_build_module(wmod, arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, "ir ret_42");
    TEST_ASSERT(m->first_func != NULL, "has function");
    TEST_ASSERT(strcmp(m->first_func->name, "f") == 0, "func name");
    TEST_ASSERT(m->first_func->ret_type->kind == LR_TYPE_I32, "ret type i32");
    lr_arena_destroy(arena);
    return 0;
}

int test_wasm_ir_add_args(void) {
    /* Function adding two i32 params */
    uint8_t wasm[] = {
        0x00, 0x61, 0x73, 0x6D,
        0x01, 0x00, 0x00, 0x00,
        /* Type: (i32, i32) -> i32 */
        0x01, 0x07, 0x01, 0x60, 0x02, 0x7F, 0x7F, 0x01, 0x7F,
        /* Function: type 0 */
        0x03, 0x02, 0x01, 0x00,
        /* Export: "add" -> func 0 */
        0x07, 0x07, 0x01, 0x03, 'a', 'd', 'd', 0x00, 0x00,
        /* Code: local.get 0, local.get 1, i32.add, end */
        0x0A, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6A, 0x0B,
    };
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};
    lr_wasm_module_t *wmod = lr_wasm_decode(wasm, sizeof(wasm), arena, err, sizeof(err));
    TEST_ASSERT(wmod != NULL, "decode add_args");
    lr_module_t *m = lr_wasm_build_module(wmod, arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, "ir add_args");
    lr_func_t *f = m->first_func;
    TEST_ASSERT(f != NULL, "has function");
    TEST_ASSERT_EQ(f->num_params, 2, "2 params");

    /* Walk instructions to find an ADD */
    bool found_add = false;
    for (lr_block_t *b = f->first_block; b; b = b->next)
        for (lr_inst_t *inst = b->first; inst; inst = inst->next)
            if (inst->op == LR_OP_ADD) found_add = true;
    TEST_ASSERT(found_add, "IR contains ADD");
    lr_arena_destroy(arena);
    return 0;
}

int test_wasm_ir_i64_unsigned_div_rem_lower_to_integer_ops(void) {
    uint8_t div_wasm[] = {
        0x00, 0x61, 0x73, 0x6D,
        0x01, 0x00, 0x00, 0x00,
        /* Type: () -> i64 */
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7E,
        /* Function: type 0 */
        0x03, 0x02, 0x01, 0x00,
        /* Export: "div_u64" -> func 0 */
        0x07, 0x0B, 0x01, 0x07, 'd', 'i', 'v', '_', 'u', '6', '4', 0x00, 0x00,
        /* Code: i64.const 60, i64.const 7, i64.div_u, end */
        0x0A, 0x09, 0x01, 0x07, 0x00, 0x42, 0x3C, 0x42, 0x07, 0x80, 0x0B,
    };
    uint8_t rem_wasm[] = {
        0x00, 0x61, 0x73, 0x6D,
        0x01, 0x00, 0x00, 0x00,
        /* Type: () -> i64 */
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7E,
        /* Function: type 0 */
        0x03, 0x02, 0x01, 0x00,
        /* Export: "rem_u64" -> func 0 */
        0x07, 0x0B, 0x01, 0x07, 'r', 'e', 'm', '_', 'u', '6', '4', 0x00, 0x00,
        /* Code: i64.const 60, i64.const 7, i64.rem_u, end */
        0x0A, 0x09, 0x01, 0x07, 0x00, 0x42, 0x3C, 0x42, 0x07, 0x82, 0x0B,
    };
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};
    lr_wasm_module_t *wmod = lr_wasm_decode(div_wasm, sizeof(div_wasm), arena, err, sizeof(err));
    TEST_ASSERT(wmod != NULL, "decode i64.div_u");
    lr_module_t *m = lr_wasm_build_module(wmod, arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, "build module i64.div_u");
    lr_func_t *f = m->first_func;
    bool found_sdiv = false;
    TEST_ASSERT(f != NULL && f->first_block, "has i64.div_u body");
    for (lr_inst_t *inst = f->first_block->first; inst; inst = inst->next)
        if (inst->op == LR_OP_SDIV)
            found_sdiv = true;
    TEST_ASSERT(found_sdiv, "i64.div_u lowers to integer div op");

    wmod = lr_wasm_decode(rem_wasm, sizeof(rem_wasm), arena, err, sizeof(err));
    TEST_ASSERT(wmod != NULL, "decode i64.rem_u");
    m = lr_wasm_build_module(wmod, arena, err, sizeof(err));
    TEST_ASSERT(m != NULL, "build module i64.rem_u");
    f = m->first_func;
    bool found_srem = false;
    TEST_ASSERT(f != NULL && f->first_block, "has i64.rem_u body");
    for (lr_inst_t *inst = f->first_block->first; inst; inst = inst->next)
        if (inst->op == LR_OP_SREM)
            found_srem = true;
    TEST_ASSERT(found_srem, "i64.rem_u lowers to integer rem op");

    lr_arena_destroy(arena);
    return 0;
}

int test_wasm_to_session_builds_function_ir(void) {
    lr_session_config_t cfg = {0};
    lr_error_t sess_err = {0};
    lr_session_t *session;
    lr_func_t *f;
    bool found_add = false;
    bool found_ret = false;
    void *last_addr = NULL;
    /* Function adding two i32 params */
    uint8_t wasm[] = {
        0x00, 0x61, 0x73, 0x6D,
        0x01, 0x00, 0x00, 0x00,
        /* Type: (i32, i32) -> i32 */
        0x01, 0x07, 0x01, 0x60, 0x02, 0x7F, 0x7F, 0x01, 0x7F,
        /* Function: type 0 */
        0x03, 0x02, 0x01, 0x00,
        /* Export: "add" -> func 0 */
        0x07, 0x07, 0x01, 0x03, 'a', 'd', 'd', 0x00, 0x00,
        /* Code: local.get 0, local.get 1, i32.add, end */
        0x0A, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6A, 0x0B,
    };
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};
    lr_wasm_module_t *wmod = lr_wasm_decode(wasm, sizeof(wasm), arena, err, sizeof(err));
    TEST_ASSERT(wmod != NULL, "decode add_args");

    cfg.mode = LR_MODE_IR;
    session = lr_session_create(&cfg, &sess_err);
    TEST_ASSERT(session != NULL, "session create");

    TEST_ASSERT_EQ(lr_wasm_to_session(wmod, session, &last_addr, &sess_err), 0,
                   "wasm to session conversion");
    TEST_ASSERT(last_addr != NULL, "conversion returns last compiled function address");

    f = lr_session_module(session)->first_func;
    TEST_ASSERT(f != NULL, "session module has function");
    TEST_ASSERT(strcmp(f->name, "add") == 0, "session function name");
    for (lr_block_t *b = f->first_block; b; b = b->next) {
        for (lr_inst_t *inst = b->first; inst; inst = inst->next) {
            if (inst->op == LR_OP_ADD)
                found_add = true;
            if (inst->op == LR_OP_RET)
                found_ret = true;
        }
    }
    TEST_ASSERT(found_add, "session IR contains add");
    TEST_ASSERT(found_ret, "session IR contains ret");

    lr_session_destroy(session);
    lr_arena_destroy(arena);
    return 0;
}

int test_wasm_to_session_invalid_arguments(void) {
    lr_error_t sess_err = {0};
    /* Function returning constant 42 */
    uint8_t wasm[] = {
        0x00, 0x61, 0x73, 0x6D,
        0x01, 0x00, 0x00, 0x00,
        /* Type: () -> i32 */
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7F,
        /* Function: type 0 */
        0x03, 0x02, 0x01, 0x00,
        /* Export: "f" -> func 0 */
        0x07, 0x05, 0x01, 0x01, 'f', 0x00, 0x00,
        /* Code: i32.const 42, end */
        0x0A, 0x06, 0x01, 0x04, 0x00, 0x41, 0x2A, 0x0B,
    };
    lr_arena_t *arena = lr_arena_create(0);
    char err[256] = {0};
    lr_wasm_module_t *wmod = lr_wasm_decode(wasm, sizeof(wasm), arena, err, sizeof(err));
    TEST_ASSERT(wmod != NULL, "decode ret_42");

    TEST_ASSERT_EQ(lr_wasm_to_session(wmod, NULL, NULL, &sess_err), -1,
                   "null session rejected");
    TEST_ASSERT_EQ(sess_err.code, LR_ERR_ARGUMENT, "null session error code");
    TEST_ASSERT(strstr(sess_err.msg, "invalid wasm session conversion input") != NULL,
                "null session error message");
    lr_arena_destroy(arena);
    return 0;
}

/* ---- JIT execution tests ---- */

int test_wasm_jit_ret_42(void) {
    uint8_t wasm[] = {
        0x00, 0x61, 0x73, 0x6D,
        0x01, 0x00, 0x00, 0x00,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7F,
        0x03, 0x02, 0x01, 0x00,
        0x07, 0x05, 0x01, 0x01, 'f', 0x00, 0x00,
        0x0A, 0x06, 0x01, 0x04, 0x00, 0x41, 0x2A, 0x0B,
    };
    char err[256] = {0};
    lr_module_t *m = lr_parse_wasm(wasm, sizeof(wasm), err, sizeof(err));
    TEST_ASSERT(m != NULL, "parse wasm ret_42");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(void);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "f");
    TEST_ASSERT(fn != NULL, "function lookup");

    int result = fn();
    TEST_ASSERT_EQ(result, 42, "f() returns 42");

    lr_jit_destroy(jit);
    lr_module_free(m);
    return 0;
}

int test_wasm_jit_add_args(void) {
    uint8_t wasm[] = {
        0x00, 0x61, 0x73, 0x6D,
        0x01, 0x00, 0x00, 0x00,
        /* Type: (i32, i32) -> i32 */
        0x01, 0x07, 0x01, 0x60, 0x02, 0x7F, 0x7F, 0x01, 0x7F,
        /* Function: type 0 */
        0x03, 0x02, 0x01, 0x00,
        /* Export: "add" -> func 0 */
        0x07, 0x07, 0x01, 0x03, 'a', 'd', 'd', 0x00, 0x00,
        /* Code: local.get 0, local.get 1, i32.add, end */
        0x0A, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6A, 0x0B,
    };
    char err[256] = {0};
    lr_module_t *m = lr_parse_wasm(wasm, sizeof(wasm), err, sizeof(err));
    TEST_ASSERT(m != NULL, "parse wasm add");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int, int);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "add");
    TEST_ASSERT(fn != NULL, "function lookup");

    TEST_ASSERT_EQ(fn(10, 32), 42, "add(10, 32) == 42");
    TEST_ASSERT_EQ(fn(-5, 5), 0, "add(-5, 5) == 0");
    TEST_ASSERT_EQ(fn(0, 0), 0, "add(0, 0) == 0");

    lr_jit_destroy(jit);
    lr_module_free(m);
    return 0;
}

int test_wasm_jit_div_u_opcodes_lower(void) {
    uint8_t wasm_i32[] = {
        0x00, 0x61, 0x73, 0x6D,
        0x01, 0x00, 0x00, 0x00,
        /* Type: () -> i32 */
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7F,
        /* Function: type 0 */
        0x03, 0x02, 0x01, 0x00,
        /* Export: "div_u32" -> func 0 */
        0x07, 0x0B, 0x01, 0x07, 'd', 'i', 'v', '_', 'u', '3', '2', 0x00, 0x00,
        /* Code: i32.const 42, i32.const 5, i32.div_u, end */
        0x0A, 0x09, 0x01, 0x07, 0x00, 0x41, 0x2A, 0x41, 0x05, 0x6E, 0x0B,
    };
    char err[256] = {0};
    lr_module_t *m = lr_parse_wasm(wasm_i32, sizeof(wasm_i32), err, sizeof(err));
    TEST_ASSERT(m != NULL, "parse wasm i32.div_u");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create i32.div_u");
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module i32.div_u");

    typedef int (*fn32_t)(void);
    fn32_t fn32;
    LR_JIT_GET_FN(fn32, jit, "div_u32");
    TEST_ASSERT(fn32 != NULL, "function lookup i32.div_u");
    TEST_ASSERT_EQ(fn32(), 8, "i32.div_u opcode lowers to integer division");

    lr_jit_destroy(jit);
    lr_module_free(m);
    return 0;
}

int test_wasm_jit_rem_u_opcodes_lower(void) {
    uint8_t wasm_i32[] = {
        0x00, 0x61, 0x73, 0x6D,
        0x01, 0x00, 0x00, 0x00,
        /* Type: () -> i32 */
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7F,
        /* Function: type 0 */
        0x03, 0x02, 0x01, 0x00,
        /* Export: "rem_u32" -> func 0 */
        0x07, 0x0B, 0x01, 0x07, 'r', 'e', 'm', '_', 'u', '3', '2', 0x00, 0x00,
        /* Code: i32.const 42, i32.const 5, i32.rem_u, end */
        0x0A, 0x09, 0x01, 0x07, 0x00, 0x41, 0x2A, 0x41, 0x05, 0x70, 0x0B,
    };
    char err[256] = {0};
    lr_module_t *m = lr_parse_wasm(wasm_i32, sizeof(wasm_i32), err, sizeof(err));
    TEST_ASSERT(m != NULL, "parse wasm i32.rem_u");

    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create i32.rem_u");
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module i32.rem_u");

    typedef int (*fn32_t)(void);
    fn32_t fn32;
    LR_JIT_GET_FN(fn32, jit, "rem_u32");
    TEST_ASSERT(fn32 != NULL, "function lookup i32.rem_u");
    TEST_ASSERT_EQ(fn32(), 2, "i32.rem_u opcode lowers to integer remainder");

    lr_jit_destroy(jit);
    lr_module_free(m);
    return 0;
}

int test_wasm_jit_branch(void) {
    /* abs(x): if x < 0 then 0-x else x */
    uint8_t wasm[] = {
        0x00, 0x61, 0x73, 0x6D,
        0x01, 0x00, 0x00, 0x00,
        /* Type: (i32) -> i32 */
        0x01, 0x06, 0x01, 0x60, 0x01, 0x7F, 0x01, 0x7F,
        /* Function: type 0 */
        0x03, 0x02, 0x01, 0x00,
        /* Export: "abs" -> func 0 */
        0x07, 0x07, 0x01, 0x03, 'a', 'b', 's', 0x00, 0x00,
        /* Code section */
        0x0A,
        0x1A,   /* section length: 26 */
        0x01,   /* 1 code entry */
        0x18,   /* body size: 24 */
        0x01,   /* 1 local group */
        0x01, 0x7F, /* 1 local of type i32 */
        /* local.get 0, i32.const 0, i32.lt_s */
        0x20, 0x00,
        0x41, 0x00,
        0x48,
        /* if (i32 result) */
        0x04, 0x7F,
        /* i32.const 0, local.get 0, i32.sub */
        0x41, 0x00,
        0x20, 0x00,
        0x6B,
        /* else */
        0x05,
        /* local.get 0 */
        0x20, 0x00,
        /* end (if) */
        0x0B,
        /* local.set 1 (store result) */
        0x21, 0x01,
        /* local.get 1 */
        0x20, 0x01,
        /* end (func) */
        0x0B,
    };
    char err[256] = {0};
    lr_module_t *m = lr_parse_wasm(wasm, sizeof(wasm), err, sizeof(err));
    if (!m) fprintf(stderr, "  err: %s\n", err);
    TEST_ASSERT(m != NULL, "parse wasm branch");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "abs");
    TEST_ASSERT(fn != NULL, "function lookup");

    TEST_ASSERT_EQ(fn(5), 5, "abs(5) == 5");
    TEST_ASSERT_EQ(fn(-5), 5, "abs(-5) == 5");
    TEST_ASSERT_EQ(fn(0), 0, "abs(0) == 0");

    lr_jit_destroy(jit);
    lr_module_free(m);
    return 0;
}

int test_wasm_jit_loop(void) {
    /* sum(n): loop summing 1..n, return result
       local 0 = n (param)
       local 1 = i (counter, declared)
       local 2 = acc (accumulator, declared) */
    uint8_t wasm[] = {
        0x00, 0x61, 0x73, 0x6D,
        0x01, 0x00, 0x00, 0x00,
        /* Type: (i32) -> i32 */
        0x01, 0x06, 0x01, 0x60, 0x01, 0x7F, 0x01, 0x7F,
        /* Function: type 0 */
        0x03, 0x02, 0x01, 0x00,
        /* Export: "sum" -> func 0 */
        0x07, 0x07, 0x01, 0x03, 's', 'u', 'm', 0x00, 0x00,
        /* Code section */
        0x0A,
        0x25, /* section length: 37 */
        0x01, /* 1 code entry */
        0x23, /* body size: 35 */
        0x01, /* 1 local group */
        0x02, 0x7F, /* 2 locals of type i32 (i and acc) */
        /* block (void) */
        0x02, 0x40,
        /* loop (void) */
        0x03, 0x40,
        /* i = i + 1: local.get 1, i32.const 1, i32.add, local.set 1 */
        0x20, 0x01,
        0x41, 0x01,
        0x6A,
        0x21, 0x01,
        /* acc = acc + i: local.get 2, local.get 1, i32.add, local.set 2 */
        0x20, 0x02,
        0x20, 0x01,
        0x6A,
        0x21, 0x02,
        /* if i == n, br 1 (break out of block); else br 0 (continue loop) */
        0x20, 0x01,   /* local.get 1 (i) */
        0x20, 0x00,   /* local.get 0 (n) */
        0x46,         /* i32.eq */
        0x0D, 0x01,   /* br_if 1 (exits block) */
        0x0C, 0x00,   /* br 0 (continues loop) */
        /* end (loop) */
        0x0B,
        /* end (block) */
        0x0B,
        /* local.get 2 (return acc) */
        0x20, 0x02,
        /* end (func) */
        0x0B,
    };
    char err[256] = {0};
    lr_module_t *m = lr_parse_wasm(wasm, sizeof(wasm), err, sizeof(err));
    if (!m) fprintf(stderr, "  err: %s\n", err);
    TEST_ASSERT(m != NULL, "parse wasm loop");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "sum");
    TEST_ASSERT(fn != NULL, "function lookup");

    TEST_ASSERT_EQ(fn(10), 55, "sum(10) == 55");
    TEST_ASSERT_EQ(fn(1), 1, "sum(1) == 1");
    TEST_ASSERT_EQ(fn(100), 5050, "sum(100) == 5050");

    lr_jit_destroy(jit);
    lr_module_free(m);
    return 0;
}

int test_wasm_jit_call(void) {
    /* Two functions: helper(x) = x*2, main_fn(x) = helper(x) + 1 */
    uint8_t wasm[] = {
        0x00, 0x61, 0x73, 0x6D,
        0x01, 0x00, 0x00, 0x00,
        /* Type section: 1 type (i32)->i32 */
        0x01, 0x06, 0x01, 0x60, 0x01, 0x7F, 0x01, 0x7F,
        /* Function section: 2 functions, both type 0 */
        0x03, 0x03, 0x02, 0x00, 0x00,
        /* Export: "main_fn" -> func 1 */
        0x07, 0x0B, 0x01, 0x07, 'm', 'a', 'i', 'n', '_', 'f', 'n', 0x00, 0x01,
        /* Code section: 2 entries */
        0x0A,
        0x13, /* section length */
        0x02, /* 2 code entries */
        /* Code entry 0: helper(x) = x * 2 */
        0x07,       /* body size */
        0x00,       /* 0 local groups */
        0x20, 0x00, /* local.get 0 */
        0x41, 0x02, /* i32.const 2 */
        0x6C,       /* i32.mul */
        0x0B,       /* end */
        /* Code entry 1: main_fn(x) = call helper(x) + 1 */
        0x09,       /* body size */
        0x00,       /* 0 local groups */
        0x20, 0x00, /* local.get 0 */
        0x10, 0x00, /* call 0 (helper) */
        0x41, 0x01, /* i32.const 1 */
        0x6A,       /* i32.add */
        0x0B,       /* end */
    };
    char err[256] = {0};
    lr_module_t *m = lr_parse_wasm(wasm, sizeof(wasm), err, sizeof(err));
    if (!m) fprintf(stderr, "  err: %s\n", err);
    TEST_ASSERT(m != NULL, "parse wasm call");

    lr_jit_t *jit = lr_jit_create();
    int rc = lr_jit_add_module(jit, m);
    TEST_ASSERT_EQ(rc, 0, "jit add module");

    typedef int (*fn_t)(int);
    fn_t fn; LR_JIT_GET_FN(fn, jit, "main_fn");
    TEST_ASSERT(fn != NULL, "function lookup");

    /* main_fn(5) = helper(5) + 1 = 10 + 1 = 11 */
    TEST_ASSERT_EQ(fn(5), 11, "main_fn(5) == 11");
    TEST_ASSERT_EQ(fn(0), 1, "main_fn(0) == 1");
    TEST_ASSERT_EQ(fn(21), 43, "main_fn(21) == 43");

    lr_jit_destroy(jit);
    lr_module_free(m);
    return 0;
}
