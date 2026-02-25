#ifndef LIRIC_IR_SHARED_H
#define LIRIC_IR_SHARED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    LR_OP_UDIV,
    LR_OP_UREM,
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
    LR_OP_FREM,
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
    LR_OP_UITOFP,
    LR_OP_FPTOSI,
    LR_OP_FPTOUI,
    LR_OP_FPEXT,
    LR_OP_FPTRUNC,
    LR_OP_EXTRACTVALUE,
    LR_OP_INSERTVALUE,
} lr_opcode_t;

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

enum {
    LR_OP_KIND_VREG    = 0,
    LR_OP_KIND_IMM_I64 = 1,
    LR_OP_KIND_IMM_F64 = 2,
    LR_OP_KIND_BLOCK   = 3,
    LR_OP_KIND_GLOBAL  = 4,
    LR_OP_KIND_NULL    = 5,
    LR_OP_KIND_UNDEF   = 6,
};

#ifdef __cplusplus
}
#endif

#endif
