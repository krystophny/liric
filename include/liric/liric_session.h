#ifndef LIRIC_SESSION_H
#define LIRIC_SESSION_H

#include <liric/liric_legacy.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Handle ------------------------------------------------------------ */

typedef struct lr_session lr_session_t;

/* ---- Config ------------------------------------------------------------ */

typedef enum lr_session_mode {
    LR_MODE_DIRECT = 0,
    LR_MODE_IR = 1,
} lr_session_mode_t;

typedef enum lr_session_backend {
    LR_SESSION_BACKEND_DEFAULT = 0,
    LR_SESSION_BACKEND_ISEL = 1,
    LR_SESSION_BACKEND_COPY_PATCH = 2,
    LR_SESSION_BACKEND_LLVM = 3,
} lr_session_backend_t;

typedef struct lr_session_config {
    lr_session_mode_t mode;
    const char *target;
    lr_session_backend_t backend; /* default: LR_SESSION_BACKEND_DEFAULT */
} lr_session_config_t;

/* ---- Error ------------------------------------------------------------- */

typedef struct lr_error {
    int code;
    char msg[256];
} lr_error_t;

/* Error codes */
enum {
    LR_OK = 0,
    LR_ERR_ARGUMENT,
    LR_ERR_STATE,
    LR_ERR_MODE,
    LR_ERR_NOT_FOUND,
    LR_ERR_BACKEND,
    LR_ERR_PARSE,
};

/* ---- Opcodes ------------------------------------------------------------ */

typedef lr_opcode_t lr_op_t;

/* ---- Instruction descriptor -------------------------------------------- */

typedef struct lr_inst_desc {
    lr_op_t op;
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
    uint32_t call_fixed_args;
} lr_inst_desc_t;

/* ---- Lifecycle --------------------------------------------------------- */

lr_session_t *lr_session_create(const lr_session_config_t *cfg,
                                lr_error_t *err);
void lr_session_destroy(lr_session_t *s);

/* ---- Symbols ----------------------------------------------------------- */

void lr_session_add_symbol(lr_session_t *s, const char *name, void *addr);
void *lr_session_lookup(lr_session_t *s, const char *name);

/* ---- Types (session-scoped singletons) --------------------------------- */

lr_type_t *lr_type_void_s(lr_session_t *s);
lr_type_t *lr_type_i1_s(lr_session_t *s);
lr_type_t *lr_type_i8_s(lr_session_t *s);
lr_type_t *lr_type_i16_s(lr_session_t *s);
lr_type_t *lr_type_i32_s(lr_session_t *s);
lr_type_t *lr_type_i64_s(lr_session_t *s);
lr_type_t *lr_type_f32_s(lr_session_t *s);
lr_type_t *lr_type_f64_s(lr_session_t *s);
lr_type_t *lr_type_ptr_s(lr_session_t *s);
lr_type_t *lr_type_array_s(lr_session_t *s, lr_type_t *elem, uint64_t count);
lr_type_t *lr_type_struct_s(lr_session_t *s, lr_type_t **fields, uint32_t n,
                            bool packed);
lr_type_t *lr_type_function_s(lr_session_t *s, lr_type_t *ret,
                              lr_type_t **params, uint32_t n, bool vararg);

/* ---- Globals ----------------------------------------------------------- */

uint32_t lr_session_global(lr_session_t *s, const char *name, lr_type_t *type,
                           bool is_const, const void *init, size_t init_size);
uint32_t lr_session_global_extern(lr_session_t *s, const char *name,
                                  lr_type_t *type);
void lr_session_global_reloc(lr_session_t *s, uint32_t id, size_t offset,
                             const char *sym);
uint32_t lr_session_intern(lr_session_t *s, const char *name);

/* ---- Function ---------------------------------------------------------- */

int lr_session_declare(lr_session_t *s, const char *name, lr_type_t *ret,
                       lr_type_t **params, uint32_t n, bool vararg,
                       lr_error_t *err);
int lr_session_func_begin(lr_session_t *s, const char *name, lr_type_t *ret,
                          lr_type_t **params, uint32_t n, bool vararg,
                          lr_error_t *err);
int lr_session_func_begin_existing(lr_session_t *s, lr_module_t *module,
                                    lr_func_t *func, lr_error_t *err);
uint32_t lr_session_param(lr_session_t *s, uint32_t idx);
int lr_session_add_phi_copy(lr_session_t *s, uint32_t pred_block_id,
                            const lr_phi_copy_desc_t *copy,
                            lr_error_t *err);
int lr_session_func_end(lr_session_t *s, void **out_addr, lr_error_t *err);

/* ---- Blocks ------------------------------------------------------------ */

uint32_t lr_session_block(lr_session_t *s);
int lr_session_set_block(lr_session_t *s, uint32_t block_id, lr_error_t *err);
int lr_session_adopt_block(lr_session_t *s, uint32_t block_id,
                           lr_block_t *block, lr_error_t *err);
int lr_session_bind_ir(lr_session_t *s, lr_module_t *module,
                       lr_func_t *func, lr_block_t *block,
                       lr_error_t *err);

/* ---- Vreg allocation --------------------------------------------------- */

uint32_t lr_session_vreg(lr_session_t *s);

/* ---- Generic emit (returns dest vreg, 0 for void ops) ------------------ */

uint32_t lr_session_emit(lr_session_t *s, const lr_inst_desc_t *inst,
                         lr_error_t *err);

/* ---- IR-mode only ------------------------------------------------------ */

int lr_session_dump_ir(lr_session_t *s, FILE *out, lr_error_t *err);

/* ---- Convenience: parse+compile .ll text ------------------------------- */

int lr_session_compile_ll(lr_session_t *s, const char *src, size_t len,
                          void **out_addr, lr_error_t *err);
int lr_session_compile_bc(lr_session_t *s, const uint8_t *data, size_t len,
                          void **out_addr, lr_error_t *err);
int lr_session_compile_auto(lr_session_t *s, const uint8_t *data, size_t len,
                            void **out_addr, lr_error_t *err);

/* ---- Output ------------------------------------------------------------ */

int lr_session_emit_object(lr_session_t *s, const char *path, lr_error_t *err);
int lr_session_emit_exe(lr_session_t *s, const char *path, lr_error_t *err);
int lr_session_emit_exe_with_runtime(lr_session_t *s, const char *path,
                                      const char *runtime_ll, size_t runtime_len,
                                      lr_error_t *err);

/* ---- Access to underlying module (for compat layer interop) ------------ */

lr_module_t *lr_session_module(lr_session_t *s);
bool lr_session_is_direct(lr_session_t *s);
bool lr_session_is_compiling(lr_session_t *s);
lr_func_t *lr_session_cur_func(lr_session_t *s);
lr_block_t *lr_session_cur_block(lr_session_t *s);
lr_jit_t *lr_session_jit(lr_session_t *s);

/* ---- Inline convenience wrappers --------------------------------------- */

static inline uint32_t lr_emit_add(lr_session_t *s, lr_type_t *ty,
                                   lr_operand_desc_t lhs,
                                   lr_operand_desc_t rhs) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[2] = {lhs, rhs};
    d.op = LR_OP_ADD; d.type = ty; d.operands = ops; d.num_operands = 2;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_sub(lr_session_t *s, lr_type_t *ty,
                                   lr_operand_desc_t lhs,
                                   lr_operand_desc_t rhs) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[2] = {lhs, rhs};
    d.op = LR_OP_SUB; d.type = ty; d.operands = ops; d.num_operands = 2;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_mul(lr_session_t *s, lr_type_t *ty,
                                   lr_operand_desc_t lhs,
                                   lr_operand_desc_t rhs) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[2] = {lhs, rhs};
    d.op = LR_OP_MUL; d.type = ty; d.operands = ops; d.num_operands = 2;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_sdiv(lr_session_t *s, lr_type_t *ty,
                                    lr_operand_desc_t lhs,
                                    lr_operand_desc_t rhs) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[2] = {lhs, rhs};
    d.op = LR_OP_SDIV; d.type = ty; d.operands = ops; d.num_operands = 2;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_srem(lr_session_t *s, lr_type_t *ty,
                                    lr_operand_desc_t lhs,
                                    lr_operand_desc_t rhs) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[2] = {lhs, rhs};
    d.op = LR_OP_SREM; d.type = ty; d.operands = ops; d.num_operands = 2;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_and(lr_session_t *s, lr_type_t *ty,
                                   lr_operand_desc_t lhs,
                                   lr_operand_desc_t rhs) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[2] = {lhs, rhs};
    d.op = LR_OP_AND; d.type = ty; d.operands = ops; d.num_operands = 2;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_or(lr_session_t *s, lr_type_t *ty,
                                  lr_operand_desc_t lhs,
                                  lr_operand_desc_t rhs) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[2] = {lhs, rhs};
    d.op = LR_OP_OR; d.type = ty; d.operands = ops; d.num_operands = 2;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_xor(lr_session_t *s, lr_type_t *ty,
                                   lr_operand_desc_t lhs,
                                   lr_operand_desc_t rhs) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[2] = {lhs, rhs};
    d.op = LR_OP_XOR; d.type = ty; d.operands = ops; d.num_operands = 2;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_shl(lr_session_t *s, lr_type_t *ty,
                                   lr_operand_desc_t lhs,
                                   lr_operand_desc_t rhs) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[2] = {lhs, rhs};
    d.op = LR_OP_SHL; d.type = ty; d.operands = ops; d.num_operands = 2;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_lshr(lr_session_t *s, lr_type_t *ty,
                                    lr_operand_desc_t lhs,
                                    lr_operand_desc_t rhs) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[2] = {lhs, rhs};
    d.op = LR_OP_LSHR; d.type = ty; d.operands = ops; d.num_operands = 2;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_ashr(lr_session_t *s, lr_type_t *ty,
                                    lr_operand_desc_t lhs,
                                    lr_operand_desc_t rhs) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[2] = {lhs, rhs};
    d.op = LR_OP_ASHR; d.type = ty; d.operands = ops; d.num_operands = 2;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_fadd(lr_session_t *s, lr_type_t *ty,
                                    lr_operand_desc_t lhs,
                                    lr_operand_desc_t rhs) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[2] = {lhs, rhs};
    d.op = LR_OP_FADD; d.type = ty; d.operands = ops; d.num_operands = 2;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_fsub(lr_session_t *s, lr_type_t *ty,
                                    lr_operand_desc_t lhs,
                                    lr_operand_desc_t rhs) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[2] = {lhs, rhs};
    d.op = LR_OP_FSUB; d.type = ty; d.operands = ops; d.num_operands = 2;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_fmul(lr_session_t *s, lr_type_t *ty,
                                    lr_operand_desc_t lhs,
                                    lr_operand_desc_t rhs) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[2] = {lhs, rhs};
    d.op = LR_OP_FMUL; d.type = ty; d.operands = ops; d.num_operands = 2;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_fdiv(lr_session_t *s, lr_type_t *ty,
                                    lr_operand_desc_t lhs,
                                    lr_operand_desc_t rhs) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[2] = {lhs, rhs};
    d.op = LR_OP_FDIV; d.type = ty; d.operands = ops; d.num_operands = 2;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_fneg(lr_session_t *s, lr_type_t *ty,
                                    lr_operand_desc_t val) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[1] = {val};
    d.op = LR_OP_FNEG; d.type = ty; d.operands = ops; d.num_operands = 1;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_icmp(lr_session_t *s, int pred,
                                    lr_operand_desc_t lhs,
                                    lr_operand_desc_t rhs) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[2] = {lhs, rhs};
    d.op = LR_OP_ICMP; d.operands = ops; d.num_operands = 2;
    d.icmp_pred = pred;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_fcmp(lr_session_t *s, int pred,
                                    lr_operand_desc_t lhs,
                                    lr_operand_desc_t rhs) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[2] = {lhs, rhs};
    d.op = LR_OP_FCMP; d.operands = ops; d.num_operands = 2;
    d.fcmp_pred = pred;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_alloca(lr_session_t *s, lr_type_t *elem_type) {
    lr_inst_desc_t d = {0};
    d.op = LR_OP_ALLOCA; d.type = elem_type;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_load(lr_session_t *s, lr_type_t *ty,
                                    lr_operand_desc_t addr) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[1] = {addr};
    d.op = LR_OP_LOAD; d.type = ty; d.operands = ops; d.num_operands = 1;
    return lr_session_emit(s, &d, NULL);
}

static inline void lr_emit_store(lr_session_t *s, lr_operand_desc_t val,
                                 lr_operand_desc_t addr) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[2] = {val, addr};
    d.op = LR_OP_STORE; d.operands = ops; d.num_operands = 2;
    lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_gep(lr_session_t *s, lr_type_t *base_type,
                                   lr_operand_desc_t base_ptr,
                                   lr_operand_desc_t *indices,
                                   uint32_t num_indices) {
    lr_inst_desc_t d = {0};
    uint32_t nops = 1 + num_indices;
    lr_operand_desc_t ops[16];
    if (nops > 16) return 0;
    ops[0] = base_ptr;
    for (uint32_t i = 0; i < num_indices; i++) ops[1 + i] = indices[i];
    d.op = LR_OP_GEP; d.type = base_type; d.operands = ops;
    d.num_operands = nops;
    return lr_session_emit(s, &d, NULL);
}

static inline void lr_emit_ret(lr_session_t *s, lr_operand_desc_t val) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[1] = {val};
    d.op = LR_OP_RET; d.type = val.type; d.operands = ops;
    d.num_operands = 1;
    lr_session_emit(s, &d, NULL);
}

static inline void lr_emit_ret_void(lr_session_t *s) {
    lr_inst_desc_t d = {0};
    d.op = LR_OP_RET_VOID;
    lr_session_emit(s, &d, NULL);
}

static inline void lr_emit_br(lr_session_t *s, uint32_t target) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[1] = {LR_BLOCK(target)};
    d.op = LR_OP_BR; d.operands = ops; d.num_operands = 1;
    lr_session_emit(s, &d, NULL);
}

static inline void lr_emit_condbr(lr_session_t *s, lr_operand_desc_t cond,
                                  uint32_t true_id, uint32_t false_id) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[3] = {cond, LR_BLOCK(true_id), LR_BLOCK(false_id)};
    d.op = LR_OP_CONDBR; d.operands = ops; d.num_operands = 3;
    lr_session_emit(s, &d, NULL);
}

static inline void lr_emit_unreachable(lr_session_t *s) {
    lr_inst_desc_t d = {0};
    d.op = LR_OP_UNREACHABLE;
    lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_call(lr_session_t *s, lr_type_t *ret_type,
                                    lr_operand_desc_t callee,
                                    lr_operand_desc_t *args,
                                    uint32_t num_args) {
    lr_inst_desc_t d = {0};
    uint32_t nops = 1 + num_args;
    lr_operand_desc_t ops[32];
    if (nops > 32) return 0;
    ops[0] = callee;
    for (uint32_t i = 0; i < num_args; i++) ops[1 + i] = args[i];
    d.op = LR_OP_CALL; d.type = ret_type; d.operands = ops;
    d.num_operands = nops;
    return lr_session_emit(s, &d, NULL);
}

static inline void lr_emit_call_void(lr_session_t *s,
                                     lr_operand_desc_t callee,
                                     lr_operand_desc_t *args,
                                     uint32_t num_args) {
    lr_inst_desc_t d = {0};
    uint32_t nops = 1 + num_args;
    lr_operand_desc_t ops[32];
    if (nops > 32) return;
    ops[0] = callee;
    for (uint32_t i = 0; i < num_args; i++) ops[1 + i] = args[i];
    d.op = LR_OP_CALL; d.operands = ops; d.num_operands = nops;
    lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_phi(lr_session_t *s, lr_type_t *ty,
                                   lr_operand_desc_t *vals,
                                   uint32_t *block_ids,
                                   uint32_t num_incoming) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[32];
    uint32_t nops = num_incoming * 2;
    if (nops > 32) return 0;
    for (uint32_t i = 0; i < num_incoming; i++) {
        ops[i * 2] = vals[i];
        ops[i * 2 + 1] = LR_BLOCK(block_ids[i]);
    }
    d.op = LR_OP_PHI; d.type = ty; d.operands = ops; d.num_operands = nops;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_select(lr_session_t *s, lr_type_t *ty,
                                      lr_operand_desc_t cond,
                                      lr_operand_desc_t true_val,
                                      lr_operand_desc_t false_val) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[3] = {cond, true_val, false_val};
    d.op = LR_OP_SELECT; d.type = ty; d.operands = ops; d.num_operands = 3;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_sext(lr_session_t *s, lr_type_t *to,
                                    lr_operand_desc_t val) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[1] = {val};
    d.op = LR_OP_SEXT; d.type = to; d.operands = ops; d.num_operands = 1;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_zext(lr_session_t *s, lr_type_t *to,
                                    lr_operand_desc_t val) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[1] = {val};
    d.op = LR_OP_ZEXT; d.type = to; d.operands = ops; d.num_operands = 1;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_trunc(lr_session_t *s, lr_type_t *to,
                                     lr_operand_desc_t val) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[1] = {val};
    d.op = LR_OP_TRUNC; d.type = to; d.operands = ops; d.num_operands = 1;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_bitcast(lr_session_t *s, lr_type_t *to,
                                       lr_operand_desc_t val) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[1] = {val};
    d.op = LR_OP_BITCAST; d.type = to; d.operands = ops; d.num_operands = 1;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_ptrtoint(lr_session_t *s, lr_type_t *to,
                                        lr_operand_desc_t val) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[1] = {val};
    d.op = LR_OP_PTRTOINT; d.type = to; d.operands = ops; d.num_operands = 1;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_inttoptr(lr_session_t *s, lr_type_t *to,
                                        lr_operand_desc_t val) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[1] = {val};
    d.op = LR_OP_INTTOPTR; d.type = to; d.operands = ops; d.num_operands = 1;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_sitofp(lr_session_t *s, lr_type_t *to,
                                      lr_operand_desc_t val) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[1] = {val};
    d.op = LR_OP_SITOFP; d.type = to; d.operands = ops; d.num_operands = 1;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_uitofp(lr_session_t *s, lr_type_t *to,
                                      lr_operand_desc_t val) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[1] = {val};
    d.op = LR_OP_UITOFP; d.type = to; d.operands = ops; d.num_operands = 1;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_fptosi(lr_session_t *s, lr_type_t *to,
                                      lr_operand_desc_t val) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[1] = {val};
    d.op = LR_OP_FPTOSI; d.type = to; d.operands = ops; d.num_operands = 1;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_fptoui(lr_session_t *s, lr_type_t *to,
                                      lr_operand_desc_t val) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[1] = {val};
    d.op = LR_OP_FPTOUI; d.type = to; d.operands = ops; d.num_operands = 1;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_fpext(lr_session_t *s, lr_type_t *to,
                                     lr_operand_desc_t val) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[1] = {val};
    d.op = LR_OP_FPEXT; d.type = to; d.operands = ops; d.num_operands = 1;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_fptrunc(lr_session_t *s, lr_type_t *to,
                                       lr_operand_desc_t val) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[1] = {val};
    d.op = LR_OP_FPTRUNC; d.type = to; d.operands = ops; d.num_operands = 1;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_extractvalue(lr_session_t *s, lr_type_t *ty,
                                            lr_operand_desc_t agg,
                                            uint32_t *indices,
                                            uint32_t num_indices) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[1] = {agg};
    d.op = LR_OP_EXTRACTVALUE; d.type = ty; d.operands = ops;
    d.num_operands = 1; d.indices = indices; d.num_indices = num_indices;
    return lr_session_emit(s, &d, NULL);
}

static inline uint32_t lr_emit_insertvalue(lr_session_t *s, lr_type_t *ty,
                                           lr_operand_desc_t agg,
                                           lr_operand_desc_t val,
                                           uint32_t *indices,
                                           uint32_t num_indices) {
    lr_inst_desc_t d = {0};
    lr_operand_desc_t ops[2] = {agg, val};
    d.op = LR_OP_INSERTVALUE; d.type = ty; d.operands = ops;
    d.num_operands = 2; d.indices = indices; d.num_indices = num_indices;
    return lr_session_emit(s, &d, NULL);
}

#ifdef __cplusplus
}
#endif

#endif
