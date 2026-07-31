#ifndef LIRIC_IR_SHARED_H
#define LIRIC_IR_SHARED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Explicit, append-only opcode values. These numbers are frozen public ABI:
   inserting a value before an existing one requires bumping
   LIRIC_SESSION_ABI_VERSION. New opcodes are appended before LR_OP_COUNT. */
typedef enum lr_opcode {
    LR_OP_RET = 0,
    LR_OP_RET_VOID = 1,
    LR_OP_BR = 2,
    LR_OP_CONDBR = 3,
    LR_OP_UNREACHABLE = 4,
    LR_OP_ADD = 5,
    LR_OP_SUB = 6,
    LR_OP_MUL = 7,
    LR_OP_SDIV = 8,
    LR_OP_SREM = 9,
    LR_OP_UDIV = 10,
    LR_OP_UREM = 11,
    LR_OP_AND = 12,
    LR_OP_OR = 13,
    LR_OP_XOR = 14,
    LR_OP_SHL = 15,
    LR_OP_LSHR = 16,
    LR_OP_ASHR = 17,
    LR_OP_FADD = 18,
    LR_OP_FSUB = 19,
    LR_OP_FMUL = 20,
    LR_OP_FDIV = 21,
    LR_OP_FREM = 22,
    LR_OP_FNEG = 23,
    LR_OP_ICMP = 24,
    LR_OP_FCMP = 25,
    LR_OP_ALLOCA = 26,
    LR_OP_LOAD = 27,
    LR_OP_STORE = 28,
    LR_OP_GEP = 29,
    LR_OP_CALL = 30,
    LR_OP_PHI = 31,
    LR_OP_SELECT = 32,
    LR_OP_SEXT = 33,
    LR_OP_ZEXT = 34,
    LR_OP_TRUNC = 35,
    LR_OP_BITCAST = 36,
    LR_OP_PTRTOINT = 37,
    LR_OP_INTTOPTR = 38,
    LR_OP_SITOFP = 39,
    LR_OP_UITOFP = 40,
    LR_OP_FPTOSI = 41,
    LR_OP_FPTOUI = 42,
    LR_OP_FPEXT = 43,
    LR_OP_FPTRUNC = 44,
    LR_OP_EXTRACTVALUE = 45,
    LR_OP_INSERTVALUE = 46,
} lr_opcode_t;

/* Count sentinel, kept outside the enum so exhaustive switches stay clean. */
enum { LR_OP_COUNT = LR_OP_INSERTVALUE + 1 };

typedef enum lr_fcmp_pred {
    LR_FCMP_FALSE,
    LR_FCMP_OEQ, LR_FCMP_OGT, LR_FCMP_OGE, LR_FCMP_OLT, LR_FCMP_OLE, LR_FCMP_ONE, LR_FCMP_ORD,
    LR_FCMP_UEQ, LR_FCMP_UGT, LR_FCMP_UGE, LR_FCMP_ULT, LR_FCMP_ULE, LR_FCMP_UNE, LR_FCMP_UNO,
    LR_FCMP_TRUE,
} lr_fcmp_pred_t;

typedef struct lr_operand_desc {
    int kind;
    union {
        uint32_t vreg;
        int64_t imm_i64;
        double imm_f64;
        uint32_t block_id;
        uint32_t global_id;
    };
    struct lr_type *type;
    int64_t global_offset;
} lr_operand_desc_t;

typedef struct lr_phi_copy_desc {
    uint32_t dest_vreg;
    lr_operand_desc_t src_op;
} lr_phi_copy_desc_t;

/* Explicit, append-only operand-kind values; same freezing rule as opcodes. */
enum {
    LR_OP_KIND_VREG    = 0,
    LR_OP_KIND_IMM_I64 = 1,
    LR_OP_KIND_IMM_F64 = 2,
    LR_OP_KIND_BLOCK   = 3,
    LR_OP_KIND_GLOBAL  = 4,
    LR_OP_KIND_NULL    = 5,
    LR_OP_KIND_UNDEF   = 6,
};

enum { LR_OP_KIND_COUNT = LR_OP_KIND_UNDEF + 1 };

#ifdef __cplusplus
}
#endif

#endif
