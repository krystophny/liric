#ifndef LIRIC_TARGET_H
#define LIRIC_TARGET_H

#include "ir.h"
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

/* Machine instruction */
typedef enum lr_x86_op {
    LR_X86_MOV,
    LR_X86_MOV_IMM,
    LR_X86_ADD,
    LR_X86_SUB,
    LR_X86_IMUL,
    LR_X86_IDIV,
    LR_X86_AND,
    LR_X86_OR,
    LR_X86_XOR,
    LR_X86_SAL,
    LR_X86_SAR,
    LR_X86_SHR,
    LR_X86_CMP,
    LR_X86_TEST,
    LR_X86_JMP,
    LR_X86_JCC,
    LR_X86_SETCC,
    LR_X86_CMOVCC,
    LR_X86_RET,
    LR_X86_PUSH,
    LR_X86_POP,
    LR_X86_CALL,
    LR_X86_LEA,
    LR_X86_CDQ,
    LR_X86_CQO,
    LR_X86_MOVSX,
    LR_X86_MOVZX,
    LR_X86_NOP,
    LR_X86_SUB_RSP,   /* sub rsp, imm (prologue) */
    LR_X86_ADD_RSP,   /* add rsp, imm (epilogue) */
} lr_x86_op_t;

typedef struct lr_minst {
    lr_x86_op_t op;
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
    int32_t *stack_slots;      /* rbp offsets for each vreg stack slot */
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

#endif
