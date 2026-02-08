#include "target_common.h"

bool lr_target_alloca_uses_static_storage(const lr_inst_t *inst) {
    if (!inst || inst->op != LR_OP_ALLOCA) {
        return false;
    }
    if (inst->num_operands == 0) {
        return true;
    }
    return inst->operands[0].kind == LR_VAL_IMM_I64 &&
           inst->operands[0].imm_i64 == 1;
}

size_t lr_target_alloca_elem_size(const lr_inst_t *inst, size_t min_size) {
    size_t elem_size = 0;
    if (inst && inst->type) {
        elem_size = lr_type_size(inst->type);
    }
    if (elem_size < min_size) {
        elem_size = min_size;
    }
    return elem_size;
}

bool lr_target_inst_has_result_slot(const lr_inst_t *inst) {
    if (!inst) {
        return false;
    }
    switch (inst->op) {
    case LR_OP_STORE:
    case LR_OP_BR:
    case LR_OP_CONDBR:
    case LR_OP_RET:
    case LR_OP_RET_VOID:
    case LR_OP_UNREACHABLE:
        return false;
    default:
        break;
    }
    return inst->type && inst->type->kind != LR_TYPE_VOID;
}

size_t lr_target_inst_result_slot_size(const lr_inst_t *inst, size_t min_size) {
    size_t slot_size = min_size;
    if (!lr_target_inst_has_result_slot(inst)) {
        return 0;
    }
    if (inst->type) {
        size_t type_size = lr_type_size(inst->type);
        if (type_size > slot_size) {
            slot_size = type_size;
        }
    }
    return slot_size;
}

uint8_t lr_target_cc_from_icmp(lr_icmp_pred_t pred) {
    switch (pred) {
    case LR_ICMP_EQ:  return LR_CC_EQ;
    case LR_ICMP_NE:  return LR_CC_NE;
    case LR_ICMP_SGT: return LR_CC_SGT;
    case LR_ICMP_SGE: return LR_CC_SGE;
    case LR_ICMP_SLT: return LR_CC_SLT;
    case LR_ICMP_SLE: return LR_CC_SLE;
    case LR_ICMP_UGT: return LR_CC_UGT;
    case LR_ICMP_UGE: return LR_CC_UGE;
    case LR_ICMP_ULT: return LR_CC_ULT;
    case LR_ICMP_ULE: return LR_CC_ULE;
    default:          return LR_CC_EQ;
    }
}

uint8_t lr_target_cc_from_fcmp(lr_fcmp_pred_t pred) {
    switch (pred) {
    case LR_FCMP_OEQ: return LR_CC_FP_OEQ;
    case LR_FCMP_ONE: return LR_CC_FP_ONE;
    case LR_FCMP_OGT: return LR_CC_FP_OGT;
    case LR_FCMP_OGE: return LR_CC_FP_OGE;
    case LR_FCMP_OLT: return LR_CC_FP_OLT;
    case LR_FCMP_OLE: return LR_CC_FP_OLE;
    case LR_FCMP_ORD: return LR_CC_FP_ORD;
    case LR_FCMP_UNO: return LR_CC_FP_UNO;
    case LR_FCMP_UEQ: return LR_CC_FP_UEQ;
    case LR_FCMP_UNE: return LR_CC_FP_UNE;
    case LR_FCMP_UGT: return LR_CC_FP_UGT;
    case LR_FCMP_UGE: return LR_CC_FP_UGE;
    case LR_FCMP_ULT: return LR_CC_FP_ULT;
    case LR_FCMP_ULE: return LR_CC_FP_ULE;
    default:          return LR_CC_FP_OEQ;
    }
}
