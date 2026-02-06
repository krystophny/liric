#ifndef LIRIC_TARGET_H
#define LIRIC_TARGET_H

#include "ir.h"
#include <stdbool.h>
#include <stdint.h>

/* Machine instruction operand */
typedef enum lr_mop_kind {
    LR_MOP_REG,
    LR_MOP_IMM,
    LR_MOP_MEM,    /* [base + disp] */
    LR_MOP_LABEL,
} lr_mop_kind_t;

typedef struct lr_moperand {
    lr_mop_kind_t kind;
    union {
        uint8_t reg;
        int64_t imm;
        struct { uint8_t base; int32_t disp; } mem;
        uint32_t label;
    };
} lr_moperand_t;

/* Target-neutral MIR opcodes shared by all backends */
typedef enum lr_mir_op {
    LR_MIR_MOV,
    LR_MIR_MOV_IMM,
    LR_MIR_ADD,
    LR_MIR_SUB,
    LR_MIR_IMUL,
    LR_MIR_IDIV,
    LR_MIR_AND,
    LR_MIR_OR,
    LR_MIR_XOR,
    LR_MIR_SAL,
    LR_MIR_SAR,
    LR_MIR_SHR,
    LR_MIR_CMP,
    LR_MIR_TEST,
    LR_MIR_JMP,
    LR_MIR_JCC,
    LR_MIR_SETCC,
    LR_MIR_CMOVCC,
    LR_MIR_RET,
    LR_MIR_PUSH,
    LR_MIR_POP,
    LR_MIR_CALL,
    LR_MIR_LEA,
    LR_MIR_CDQ,
    LR_MIR_CQO,
    LR_MIR_MOVSX,
    LR_MIR_MOVZX,
    LR_MIR_NOP,
    LR_MIR_FRAME_ALLOC,
    LR_MIR_FRAME_FREE,
} lr_mir_op_t;

/* Target-neutral condition codes used by JCC/SETCC/CMOVCC */
enum {
    LR_CC_EQ = 0, LR_CC_NE, LR_CC_UGT, LR_CC_UGE, LR_CC_ULT, LR_CC_ULE,
    LR_CC_SGT, LR_CC_SGE, LR_CC_SLT, LR_CC_SLE,
    LR_CC_O, LR_CC_NO,
};

typedef struct lr_minst {
    lr_mir_op_t op;
    lr_moperand_t dst;
    lr_moperand_t src;
    uint8_t size;      /* operand size: 1, 2, 4, or 8 bytes */
    uint8_t cc;        /* condition code for jcc/setcc/cmovcc */
    struct lr_minst *next;
} lr_minst_t;

typedef struct lr_mblock {
    uint32_t id;
    int32_t offset;    /* code offset once laid out, -1 if not yet placed */
    lr_minst_t *first;
    lr_minst_t *last;
    lr_minst_t *before_term;  /* last instruction before terminator sequence */
    struct lr_mblock *next;
} lr_mblock_t;

/* Machine function: result of ISel */
typedef struct lr_mfunc {
    char *name;
    lr_mblock_t *first_block;
    lr_mblock_t *last_block;
    uint32_t num_blocks;
    uint32_t stack_size;       /* total stack frame size */
    uint32_t num_stack_slots;
    int32_t *stack_slots;      /* frame pointer offsets for each vreg stack slot */
    lr_arena_t *arena;
    lr_func_t *ir_func;
} lr_mfunc_t;

typedef struct lr_target {
    const char *name;
    uint8_t ptr_size;

    int (*isel_func)(lr_func_t *func, lr_mfunc_t *mf, lr_module_t *mod);
    int (*encode_func)(lr_mfunc_t *mf, uint8_t *buf, size_t buflen, size_t *out_len);
    int (*print_inst)(const lr_minst_t *mi, char *buf, size_t len);
} lr_target_t;

const lr_target_t *lr_target_x86_64(void);
const lr_target_t *lr_target_aarch64(void);
const lr_target_t *lr_target_by_name(const char *name);
const lr_target_t *lr_target_host(void);
bool lr_target_is_host_compatible(const lr_target_t *t);

#endif
