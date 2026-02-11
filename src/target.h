#ifndef LIRIC_TARGET_H
#define LIRIC_TARGET_H

#include "ir.h"
#include <stdbool.h>
#include <stdint.h>

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

    int (*compile_func)(lr_func_t *func, lr_module_t *mod,
                        uint8_t *buf, size_t buflen, size_t *out_len,
                        lr_arena_t *arena);
} lr_target_t;

const lr_target_t *lr_target_x86_64(void);
const lr_target_t *lr_target_aarch64(void);
const lr_target_t *lr_target_riscv64(void);
const lr_target_t *lr_target_riscv64gc(void);
const lr_target_t *lr_target_riscv64im(void);
const lr_target_t *lr_target_by_name(const char *name);
const lr_target_t *lr_target_host(void);
bool lr_target_is_host_compatible(const lr_target_t *t);

#endif
