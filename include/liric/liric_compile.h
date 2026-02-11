#ifndef LIRIC_COMPILE_H
#define LIRIC_COMPILE_H

#include <liric/liric.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lr_compile_session lr_compile_session_t;

typedef enum lr_compile_strategy {
    LR_COMPILE_STRATEGY_DIRECT_PASS = 0,
    LR_COMPILE_STRATEGY_IR_MODE = 1,
} lr_compile_strategy_t;

typedef enum lr_compile_error_code {
    LR_COMPILE_OK = 0,
    LR_COMPILE_ERR_INVALID_ARGUMENT,
    LR_COMPILE_ERR_MODE_CONFLICT,
    LR_COMPILE_ERR_STATE,
    LR_COMPILE_ERR_NOT_FOUND,
    LR_COMPILE_ERR_BACKEND,
    LR_COMPILE_ERR_PARSE,
    LR_COMPILE_ERR_UNSUPPORTED,
} lr_compile_error_code_t;

typedef struct lr_compile_error {
    lr_compile_error_code_t code;
    char message[256];
} lr_compile_error_t;

typedef struct lr_compile_config {
    lr_compile_strategy_t strategy;
    const char *target_name;
    bool enable_local_peephole;
    bool enable_ir_pipeline;
} lr_compile_config_t;

typedef struct lr_function_spec {
    const char *name;
    lr_type_t *ret_type;
    lr_type_t **param_types;
    uint32_t num_params;
    bool vararg;
} lr_function_spec_t;

typedef struct lr_symbol_handle {
    const char *name;
    void *addr;
} lr_symbol_handle_t;

typedef uint32_t lr_block_id_t;

typedef enum lr_opcode {
    LR_OP_RET,
    LR_OP_RET_VOID,
    LR_OP_BR,
    LR_OP_CONDBR,
    LR_OP_UNREACHABLE,
    LR_OP_ADD,
    LR_OP_SUB,
    LR_OP_MUL,
    LR_OP_SDIV,
    LR_OP_SREM,
    LR_OP_AND,
    LR_OP_OR,
    LR_OP_XOR,
    LR_OP_SHL,
    LR_OP_LSHR,
    LR_OP_ASHR,
    LR_OP_FADD,
    LR_OP_FSUB,
    LR_OP_FMUL,
    LR_OP_FDIV,
    LR_OP_FNEG,
    LR_OP_ICMP,
    LR_OP_FCMP,
    LR_OP_ALLOCA,
    LR_OP_LOAD,
    LR_OP_STORE,
    LR_OP_GEP,
    LR_OP_CALL,
    LR_OP_PHI,
    LR_OP_SELECT,
    LR_OP_SEXT,
    LR_OP_ZEXT,
    LR_OP_TRUNC,
    LR_OP_BITCAST,
    LR_OP_PTRTOINT,
    LR_OP_INTTOPTR,
    LR_OP_SITOFP,
    LR_OP_FPTOSI,
    LR_OP_FPEXT,
    LR_OP_FPTRUNC,
    LR_OP_EXTRACTVALUE,
    LR_OP_INSERTVALUE,
} lr_opcode_t;

typedef struct lr_inst_desc {
    lr_opcode_t op;
    lr_type_t *type;
    uint32_t dest;
    const lr_operand_desc_t *operands;
    uint32_t num_operands;
    const uint32_t *indices;
    uint32_t num_indices;
    int icmp_pred;
    int fcmp_pred;
    bool call_external_abi;
    bool call_vararg;
} lr_inst_desc_t;

typedef struct lr_ir_pipeline {
    uint32_t opt_level;
    bool constant_propagation;
} lr_ir_pipeline_t;

typedef int (*lr_write_cb)(void *user, const char *data, size_t len);

lr_compile_session_t *lr_compile_begin(const lr_compile_config_t *cfg,
                                       lr_compile_error_t *err);
void lr_compile_end(lr_compile_session_t *s);

int lr_add_symbol(lr_compile_session_t *s, const char *name, void *addr,
                  lr_compile_error_t *err);
void *lr_lookup_symbol(lr_compile_session_t *s, const char *name);

/* Session-owned type constructors for the compile API. */
lr_type_t *lr_compile_type_void(lr_compile_session_t *s);
lr_type_t *lr_compile_type_i1(lr_compile_session_t *s);
lr_type_t *lr_compile_type_i8(lr_compile_session_t *s);
lr_type_t *lr_compile_type_i16(lr_compile_session_t *s);
lr_type_t *lr_compile_type_i32(lr_compile_session_t *s);
lr_type_t *lr_compile_type_i64(lr_compile_session_t *s);
lr_type_t *lr_compile_type_float(lr_compile_session_t *s);
lr_type_t *lr_compile_type_double(lr_compile_session_t *s);
lr_type_t *lr_compile_type_ptr(lr_compile_session_t *s);
lr_type_t *lr_compile_type_array(lr_compile_session_t *s, lr_type_t *elem,
                                 uint64_t count);
lr_type_t *lr_compile_type_struct(lr_compile_session_t *s, lr_type_t **fields,
                                  uint32_t num_fields, bool packed);
lr_type_t *lr_compile_type_func(lr_compile_session_t *s, lr_type_t *ret,
                                lr_type_t **params, uint32_t num_params,
                                bool vararg);

int lr_func_begin(lr_compile_session_t *s, const lr_function_spec_t *spec,
                  lr_compile_error_t *err);
int lr_block_begin(lr_compile_session_t *s, lr_block_id_t block,
                   lr_compile_error_t *err);
int lr_emit(lr_compile_session_t *s, const lr_inst_desc_t *inst,
            lr_compile_error_t *err);
int lr_block_seal(lr_compile_session_t *s, lr_block_id_t block,
                  lr_compile_error_t *err);
int lr_func_end(lr_compile_session_t *s, lr_symbol_handle_t *out_symbol,
                lr_compile_error_t *err);

int lr_ir_optimize(lr_compile_session_t *s, const lr_ir_pipeline_t *pipe,
                   lr_compile_error_t *err);
int lr_ir_print(lr_compile_session_t *s, lr_write_cb cb, void *user,
                lr_compile_error_t *err);

int lr_compile_ll(lr_compile_session_t *s, const char *src, size_t len,
                  lr_symbol_handle_t *out_last_symbol,
                  lr_compile_error_t *err);

#ifdef __cplusplus
}
#endif

#endif
