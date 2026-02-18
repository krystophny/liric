#ifndef LIRIC_TARGET_H
#define LIRIC_TARGET_H

#include "ir.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Compilation mode: how IR becomes machine code */
typedef enum lr_compile_mode {
    LR_COMPILE_ISEL       = 0,  /* Mode B: ISel + encoding (current, default) */
    LR_COMPILE_COPY_PATCH = 1,  /* Mode A: copy-and-patch templates */
    LR_COMPILE_LLVM       = 2,  /* Mode C: translate to real LLVM (optional) */
} lr_compile_mode_t;

typedef struct lr_jit lr_jit_t;

typedef struct lr_compile_func_meta {
    lr_func_t *func;
    lr_type_t *ret_type;
    lr_type_t **param_types;
    uint32_t num_params;
    bool vararg;
    uint32_t num_blocks;
    uint32_t next_vreg;
    lr_compile_mode_t mode;
    lr_jit_t *jit;
} lr_compile_func_meta_t;

typedef struct lr_compile_inst_desc {
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
    uint32_t call_fixed_args;
} lr_compile_inst_desc_t;

/* Target-neutral condition codes used by backends */
enum {
    LR_CC_EQ = 0, LR_CC_NE, LR_CC_UGT, LR_CC_UGE, LR_CC_ULT, LR_CC_ULE,
    LR_CC_SGT, LR_CC_SGE, LR_CC_SLT, LR_CC_SLE,
    LR_CC_O, LR_CC_NO,
    /* FP condition codes for FCMP (ucomisd/fcmp semantics) */
    LR_CC_FP_OEQ, LR_CC_FP_ONE, LR_CC_FP_OGT, LR_CC_FP_OGE,
    LR_CC_FP_OLT, LR_CC_FP_OLE, LR_CC_FP_ORD, LR_CC_FP_UNO,
    LR_CC_FP_UEQ, LR_CC_FP_UNE, LR_CC_FP_UGT, LR_CC_FP_UGE,
    LR_CC_FP_ULT, LR_CC_FP_ULE,
};

typedef struct lr_target {
    const char *name;
    uint8_t ptr_size;

    /* Streaming compilation vtable (session API path) */
    int (*compile_begin)(void **compile_ctx,
                         const lr_compile_func_meta_t *func_meta,
                         lr_module_t *mod,
                         uint8_t *buf, size_t buflen,
                         lr_arena_t *arena);
    int (*compile_emit)(void *compile_ctx,
                        const lr_compile_inst_desc_t *inst_desc);
    int (*compile_set_block)(void *compile_ctx, uint32_t block_id);
    int (*compile_end)(void *compile_ctx, size_t *out_len);
    int (*compile_add_phi_copy)(void *compile_ctx, uint32_t pred_block_id,
                                uint32_t succ_block_id, uint32_t dest_vreg,
                                const lr_operand_desc_t *src_op);
    /* Flush deferred state (e.g. pending terminators) before suspend. */
    int (*compile_flush_pending)(void *compile_ctx);
    /* Get/set the current code write position (for suspend/resume). */
    size_t (*compile_get_pos)(void *compile_ctx);
    int (*compile_set_pos)(void *compile_ctx, size_t new_pos);
} lr_target_t;

const lr_target_t *lr_target_x86_64(void);
const lr_target_t *lr_target_aarch64(void);
const lr_target_t *lr_target_riscv64(void);
const lr_target_t *lr_target_riscv64gc(void);
const lr_target_t *lr_target_riscv64im(void);
const lr_target_t *lr_target_by_name(const char *name);
const lr_target_t *lr_target_host(void);
bool lr_target_is_host_compatible(const lr_target_t *t);
bool lr_target_can_compile(const lr_target_t *target, lr_compile_mode_t mode);
int lr_target_compile(const lr_target_t *target, lr_compile_mode_t mode,
                      lr_func_t *func, lr_module_t *mod,
                      uint8_t *buf, size_t buflen, size_t *out_len,
                      lr_arena_t *arena);

#endif
