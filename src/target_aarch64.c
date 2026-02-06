#include "target_aarch64.h"
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * aarch64 ISel: stack-based register allocation mirroring the x86_64 strategy.
 *
 * All computation flows through X9 (primary) and X10 (secondary).
 * Every IR vreg gets a stack slot addressed via FP (X29).
 * AAPCS64 argument registers: X0-X7 (8 args, vs x86's 6).
 */

static lr_minst_t *minst_new(lr_arena_t *a, lr_mir_op_t op) {
    lr_minst_t *mi = lr_arena_new(a, lr_minst_t);
    mi->op = op;
    mi->size = 8;
    return mi;
}

static void mblock_append(lr_mblock_t *mb, lr_minst_t *mi) {
    if (!mb->first) mb->first = mi;
    else mb->last->next = mi;
    mb->last = mi;
}

static lr_mblock_t *mblock_new(lr_mfunc_t *mf) {
    lr_mblock_t *mb = lr_arena_new(mf->arena, lr_mblock_t);
    mb->id = mf->num_blocks++;
    mb->offset = -1;
    if (!mf->first_block) mf->first_block = mb;
    else mf->last_block->next = mb;
    mf->last_block = mb;
    return mb;
}

static int32_t alloc_slot(lr_mfunc_t *mf, uint32_t vreg, uint8_t size) {
    while (vreg >= mf->num_stack_slots) {
        uint32_t old = mf->num_stack_slots;
        uint32_t new_cap = old == 0 ? 64 : old * 2;
        int32_t *ns = lr_arena_array(mf->arena, int32_t, new_cap);
        if (old > 0) memcpy(ns, mf->stack_slots, old * sizeof(int32_t));
        for (uint32_t i = old; i < new_cap; i++) ns[i] = 0;
        mf->stack_slots = ns;
        mf->num_stack_slots = new_cap;
    }

    if (mf->stack_slots[vreg] != 0)
        return mf->stack_slots[vreg];

    if (size < 8) size = 8;
    mf->stack_size += size;
    mf->stack_size = (mf->stack_size + size - 1) & ~(uint32_t)(size - 1);
    int32_t offset = -(int32_t)mf->stack_size;
    mf->stack_slots[vreg] = offset;
    return offset;
}

static void emit_load_slot(lr_mfunc_t *mf, lr_mblock_t *mb, uint32_t vreg, uint8_t reg) {
    int32_t off = alloc_slot(mf, vreg, 8);
    lr_minst_t *mi = minst_new(mf->arena, LR_MIR_MOV);
    mi->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = reg };
    mi->src = (lr_moperand_t){ .kind = LR_MOP_MEM, .mem = { .base = A64_FP, .disp = off } };
    mi->size = 8;
    mblock_append(mb, mi);
}

static void emit_store_slot(lr_mfunc_t *mf, lr_mblock_t *mb, uint32_t vreg, uint8_t reg) {
    int32_t off = alloc_slot(mf, vreg, 8);
    lr_minst_t *mi = minst_new(mf->arena, LR_MIR_MOV);
    mi->dst = (lr_moperand_t){ .kind = LR_MOP_MEM, .mem = { .base = A64_FP, .disp = off } };
    mi->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = reg };
    mi->size = 8;
    mblock_append(mb, mi);
}

static void emit_load_operand(lr_mfunc_t *mf, lr_mblock_t *mb,
                               const lr_operand_t *op, uint8_t reg) {
    if (op->kind == LR_VAL_IMM_I64) {
        lr_minst_t *mi = minst_new(mf->arena, LR_MIR_MOV_IMM);
        mi->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = reg };
        mi->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = op->imm_i64 };
        mi->size = 8;
        mblock_append(mb, mi);
    } else if (op->kind == LR_VAL_VREG) {
        emit_load_slot(mf, mb, op->vreg, reg);
    } else if (op->kind == LR_VAL_IMM_F64) {
        int64_t imm_bits = 0;
        if (op->type && op->type->kind == LR_TYPE_FLOAT) {
            float fv = (float)op->imm_f64;
            uint32_t bits = 0;
            memcpy(&bits, &fv, sizeof(bits));
            imm_bits = (int64_t)(uint64_t)bits;
        } else {
            uint64_t bits = 0;
            memcpy(&bits, &op->imm_f64, sizeof(bits));
            imm_bits = (int64_t)bits;
        }
        lr_minst_t *mi = minst_new(mf->arena, LR_MIR_MOV_IMM);
        mi->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = reg };
        mi->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = imm_bits };
        mi->size = 8;
        mblock_append(mb, mi);
    } else if (op->kind == LR_VAL_NULL) {
        lr_minst_t *mi = minst_new(mf->arena, LR_MIR_MOV_IMM);
        mi->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = reg };
        mi->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = 0 };
        mi->size = 8;
        mblock_append(mb, mi);
    }
}

/* Floating-point helper trampolines (same as x86 ISel) */

static uint32_t fp_add_f32_bits(uint32_t a_bits, uint32_t b_bits) {
    float a, b, out;
    memcpy(&a, &a_bits, sizeof(a));
    memcpy(&b, &b_bits, sizeof(b));
    out = a + b;
    memcpy(&a_bits, &out, sizeof(out));
    return a_bits;
}

static uint32_t fp_sub_f32_bits(uint32_t a_bits, uint32_t b_bits) {
    float a, b, out;
    memcpy(&a, &a_bits, sizeof(a));
    memcpy(&b, &b_bits, sizeof(b));
    out = a - b;
    memcpy(&a_bits, &out, sizeof(out));
    return a_bits;
}

static uint32_t fp_mul_f32_bits(uint32_t a_bits, uint32_t b_bits) {
    float a, b, out;
    memcpy(&a, &a_bits, sizeof(a));
    memcpy(&b, &b_bits, sizeof(b));
    out = a * b;
    memcpy(&a_bits, &out, sizeof(out));
    return a_bits;
}

static uint32_t fp_div_f32_bits(uint32_t a_bits, uint32_t b_bits) {
    float a, b, out;
    memcpy(&a, &a_bits, sizeof(a));
    memcpy(&b, &b_bits, sizeof(b));
    out = a / b;
    memcpy(&a_bits, &out, sizeof(out));
    return a_bits;
}

static uint64_t fp_add_f64_bits(uint64_t a_bits, uint64_t b_bits) {
    double a, b, out;
    memcpy(&a, &a_bits, sizeof(a));
    memcpy(&b, &b_bits, sizeof(b));
    out = a + b;
    memcpy(&a_bits, &out, sizeof(out));
    return a_bits;
}

static uint64_t fp_sub_f64_bits(uint64_t a_bits, uint64_t b_bits) {
    double a, b, out;
    memcpy(&a, &a_bits, sizeof(a));
    memcpy(&b, &b_bits, sizeof(b));
    out = a - b;
    memcpy(&a_bits, &out, sizeof(out));
    return a_bits;
}

static uint64_t fp_mul_f64_bits(uint64_t a_bits, uint64_t b_bits) {
    double a, b, out;
    memcpy(&a, &a_bits, sizeof(a));
    memcpy(&b, &b_bits, sizeof(b));
    out = a * b;
    memcpy(&a_bits, &out, sizeof(out));
    return a_bits;
}

static uint64_t fp_div_f64_bits(uint64_t a_bits, uint64_t b_bits) {
    double a, b, out;
    memcpy(&a, &a_bits, sizeof(a));
    memcpy(&b, &b_bits, sizeof(b));
    out = a / b;
    memcpy(&a_bits, &out, sizeof(out));
    return a_bits;
}

static uint64_t fp_cmp_f32_bits(uint64_t a_bits, uint64_t b_bits, uint64_t pred) {
    uint32_t in_a = (uint32_t)a_bits, in_b = (uint32_t)b_bits;
    float a, b;
    memcpy(&a, &in_a, sizeof(a));
    memcpy(&b, &in_b, sizeof(b));
    switch (pred) {
    case 0: return a == b;
    case 1: return a != b;
    case 2: return a > b;
    case 3: return a >= b;
    case 4: return a < b;
    case 5: return a <= b;
    case 6: return (a != a) || (b != b);
    default: return 0;
    }
}

static uint64_t fp_cmp_f64_bits(uint64_t a_bits, uint64_t b_bits, uint64_t pred) {
    double a, b;
    memcpy(&a, &a_bits, sizeof(a));
    memcpy(&b, &b_bits, sizeof(b));
    switch (pred) {
    case 0: return a == b;
    case 1: return a != b;
    case 2: return a > b;
    case 3: return a >= b;
    case 4: return a < b;
    case 5: return a <= b;
    case 6: return (a != a) || (b != b);
    default: return 0;
    }
}

static uint64_t fp_sitofp_i64_f32(int64_t val) {
    float f = (float)val;
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return (uint64_t)bits;
}

static uint64_t fp_sitofp_i64_f64(int64_t val) {
    double d = (double)val;
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    return bits;
}

static int64_t fp_fptosi_f32_i64(uint64_t val_bits) {
    uint32_t in = (uint32_t)val_bits;
    float f;
    memcpy(&f, &in, sizeof(f));
    return (int64_t)f;
}

static int64_t fp_fptosi_f64_i64(uint64_t val_bits) {
    double d;
    memcpy(&d, &val_bits, sizeof(d));
    return (int64_t)d;
}

static uint64_t fp_fpext_f32_f64(uint64_t val_bits) {
    uint32_t in = (uint32_t)val_bits;
    float f;
    memcpy(&f, &in, sizeof(f));
    double d = (double)f;
    uint64_t out;
    memcpy(&out, &d, sizeof(out));
    return out;
}

static uint64_t fp_fptrunc_f64_f32(uint64_t val_bits) {
    double d;
    memcpy(&d, &val_bits, sizeof(d));
    float f = (float)d;
    uint32_t out;
    memcpy(&out, &f, sizeof(out));
    return (uint64_t)out;
}

static int64_t fp_helper_addr(lr_opcode_t op, lr_type_t *type) {
    bool is_f32 = type && type->kind == LR_TYPE_FLOAT;
    if (is_f32) {
        switch (op) {
        case LR_OP_FADD: return (int64_t)(uintptr_t)&fp_add_f32_bits;
        case LR_OP_FSUB: return (int64_t)(uintptr_t)&fp_sub_f32_bits;
        case LR_OP_FMUL: return (int64_t)(uintptr_t)&fp_mul_f32_bits;
        case LR_OP_FDIV: return (int64_t)(uintptr_t)&fp_div_f32_bits;
        default: return 0;
        }
    }
    switch (op) {
    case LR_OP_FADD: return (int64_t)(uintptr_t)&fp_add_f64_bits;
    case LR_OP_FSUB: return (int64_t)(uintptr_t)&fp_sub_f64_bits;
    case LR_OP_FMUL: return (int64_t)(uintptr_t)&fp_mul_f64_bits;
    case LR_OP_FDIV: return (int64_t)(uintptr_t)&fp_div_f64_bits;
    default: return 0;
    }
}

static size_t struct_field_offset(const lr_type_t *st, uint32_t field_idx) {
    size_t off = 0;
    for (uint32_t i = 0; i < st->struc.num_fields && i < field_idx; i++) {
        if (!st->struc.packed) {
            size_t fa = lr_type_align(st->struc.fields[i]);
            off = (off + fa - 1) & ~(fa - 1);
        }
        off += lr_type_size(st->struc.fields[i]);
    }
    if (field_idx < st->struc.num_fields && !st->struc.packed) {
        size_t fa = lr_type_align(st->struc.fields[field_idx]);
        off = (off + fa - 1) & ~(fa - 1);
    }
    return off;
}

static int aarch64_isel_func(lr_func_t *func, lr_mfunc_t *mf, lr_module_t *mod) {
    (void)mod;
    mf->ir_func = func;
    mf->name = func->name;

    /* AAPCS64: X0-X7 for first 8 integer/pointer arguments */
    static const uint8_t param_regs[] = {
        A64_X0, A64_X1, A64_X2, A64_X3, A64_X4, A64_X5, A64_X6, A64_X7
    };

    lr_mblock_t **mblocks = lr_arena_array(mf->arena, lr_mblock_t *, func->num_blocks);
    uint32_t bi = 0;
    for (lr_block_t *b = func->first_block; b; b = b->next) {
        mblocks[bi] = mblock_new(mf);
        bi++;
    }

    lr_mblock_t *entry_mb = mf->first_block;
    for (uint32_t i = 0; i < func->num_params && i < 8; i++) {
        emit_store_slot(mf, entry_mb, func->param_vregs[i], param_regs[i]);
    }
    /* Load stack-passed parameters (args 9+) from caller's frame.
       After stp x29,x30,[sp,#-16]!; mov x29,sp the caller's stack args
       are at [FP + 16], [FP + 24], ... */
    for (uint32_t i = 8; i < func->num_params; i++) {
        int32_t caller_off = 16 + (int32_t)(i - 8) * 8;
        lr_minst_t *ld = minst_new(mf->arena, LR_MIR_MOV);
        ld->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
        ld->src = (lr_moperand_t){ .kind = LR_MOP_MEM, .mem = { .base = A64_FP, .disp = caller_off } };
        ld->size = 8;
        mblock_append(entry_mb, ld);
        emit_store_slot(mf, entry_mb, func->param_vregs[i], A64_X9);
    }

    bi = 0;
    for (lr_block_t *b = func->first_block; b; b = b->next, bi++) {
        lr_mblock_t *mb = mblocks[bi];

        for (lr_inst_t *inst = b->first; inst; inst = inst->next) {
            switch (inst->op) {
            case LR_OP_RET: {
                mb->before_term = mb->last;
                emit_load_operand(mf, mb, &inst->operands[0], A64_X9);
                lr_minst_t *mov = minst_new(mf->arena, LR_MIR_MOV);
                mov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X0 };
                mov->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                mov->size = 8;
                mblock_append(mb, mov);
                lr_minst_t *mi = minst_new(mf->arena, LR_MIR_RET);
                mblock_append(mb, mi);
                break;
            }
            case LR_OP_RET_VOID: {
                mb->before_term = mb->last;
                lr_minst_t *mi = minst_new(mf->arena, LR_MIR_RET);
                mblock_append(mb, mi);
                break;
            }
            case LR_OP_ADD: case LR_OP_SUB: case LR_OP_AND:
            case LR_OP_OR: case LR_OP_XOR: {
                emit_load_operand(mf, mb, &inst->operands[0], A64_X9);
                emit_load_operand(mf, mb, &inst->operands[1], A64_X10);
                lr_mir_op_t mop;
                switch (inst->op) {
                case LR_OP_ADD: mop = LR_MIR_ADD; break;
                case LR_OP_SUB: mop = LR_MIR_SUB; break;
                case LR_OP_AND: mop = LR_MIR_AND; break;
                case LR_OP_OR:  mop = LR_MIR_OR; break;
                case LR_OP_XOR: mop = LR_MIR_XOR; break;
                default: mop = LR_MIR_ADD; break;
                }
                lr_minst_t *mi = minst_new(mf->arena, mop);
                mi->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                mi->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X10 };
                mi->size = (uint8_t)lr_type_size(inst->type);
                mblock_append(mb, mi);
                emit_store_slot(mf, mb, inst->dest, A64_X9);
                break;
            }
            case LR_OP_MUL: {
                emit_load_operand(mf, mb, &inst->operands[0], A64_X9);
                emit_load_operand(mf, mb, &inst->operands[1], A64_X10);
                lr_minst_t *mi = minst_new(mf->arena, LR_MIR_IMUL);
                mi->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                mi->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X10 };
                mi->size = (uint8_t)lr_type_size(inst->type);
                mblock_append(mb, mi);
                emit_store_slot(mf, mb, inst->dest, A64_X9);
                break;
            }
            case LR_OP_FADD: case LR_OP_FSUB:
            case LR_OP_FMUL: case LR_OP_FDIV: {
                int64_t fn_addr = fp_helper_addr(inst->op, inst->type);
                emit_load_operand(mf, mb, &inst->operands[0], A64_X0);
                emit_load_operand(mf, mb, &inst->operands[1], A64_X1);
                lr_minst_t *mov = minst_new(mf->arena, LR_MIR_MOV_IMM);
                mov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X16 };
                mov->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = fn_addr };
                mov->size = 8;
                mblock_append(mb, mov);
                lr_minst_t *call = minst_new(mf->arena, LR_MIR_CALL);
                call->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X16 };
                mblock_append(mb, call);
                emit_store_slot(mf, mb, inst->dest, A64_X0);
                break;
            }
            case LR_OP_SDIV: case LR_OP_SREM: {
                emit_load_operand(mf, mb, &inst->operands[0], A64_X9);
                emit_load_operand(mf, mb, &inst->operands[1], A64_X10);
                /* aarch64 SDIV handles sign extension natively */
                lr_minst_t *cqo = minst_new(mf->arena, LR_MIR_CDQ);
                mblock_append(mb, cqo);
                lr_minst_t *mi = minst_new(mf->arena, LR_MIR_IDIV);
                mi->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                mi->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X10 };
                mi->size = (uint8_t)lr_type_size(inst->type);
                mblock_append(mb, mi);
                if (inst->op == LR_OP_SREM) {
                    /* remainder = dividend - quotient * divisor (via MSUB) */
                    emit_store_slot(mf, mb, inst->dest, A64_X11);
                } else {
                    emit_store_slot(mf, mb, inst->dest, A64_X9);
                }
                break;
            }
            case LR_OP_SHL: case LR_OP_LSHR: case LR_OP_ASHR: {
                emit_load_operand(mf, mb, &inst->operands[0], A64_X9);
                emit_load_operand(mf, mb, &inst->operands[1], A64_X10);
                lr_mir_op_t mop;
                switch (inst->op) {
                case LR_OP_SHL:  mop = LR_MIR_SAL; break;
                case LR_OP_LSHR: mop = LR_MIR_SHR; break;
                case LR_OP_ASHR: mop = LR_MIR_SAR; break;
                default: mop = LR_MIR_SAL; break;
                }
                lr_minst_t *mi = minst_new(mf->arena, mop);
                mi->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                mi->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X10 };
                mi->size = (uint8_t)lr_type_size(inst->type);
                mblock_append(mb, mi);
                emit_store_slot(mf, mb, inst->dest, A64_X9);
                break;
            }
            case LR_OP_ICMP: {
                emit_load_operand(mf, mb, &inst->operands[0], A64_X9);
                emit_load_operand(mf, mb, &inst->operands[1], A64_X10);
                lr_minst_t *cmp = minst_new(mf->arena, LR_MIR_CMP);
                cmp->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                cmp->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X10 };
                cmp->size = (uint8_t)lr_type_size(inst->operands[0].type);
                mblock_append(mb, cmp);

                uint8_t cc;
                switch (inst->icmp_pred) {
                case LR_ICMP_EQ:  cc = LR_CC_EQ; break;
                case LR_ICMP_NE:  cc = LR_CC_NE; break;
                case LR_ICMP_SGT: cc = LR_CC_SGT; break;
                case LR_ICMP_SGE: cc = LR_CC_SGE; break;
                case LR_ICMP_SLT: cc = LR_CC_SLT; break;
                case LR_ICMP_SLE: cc = LR_CC_SLE; break;
                case LR_ICMP_UGT: cc = LR_CC_UGT; break;
                case LR_ICMP_UGE: cc = LR_CC_UGE; break;
                case LR_ICMP_ULT: cc = LR_CC_ULT; break;
                case LR_ICMP_ULE: cc = LR_CC_ULE; break;
                default: cc = LR_CC_EQ; break;
                }

                lr_minst_t *set = minst_new(mf->arena, LR_MIR_SETCC);
                set->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                set->cc = cc;
                set->size = 1;
                mblock_append(mb, set);

                lr_minst_t *zx = minst_new(mf->arena, LR_MIR_MOVZX);
                zx->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                zx->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                zx->size = 1;
                mblock_append(mb, zx);

                emit_store_slot(mf, mb, inst->dest, A64_X9);
                break;
            }
            case LR_OP_SELECT: {
                emit_load_operand(mf, mb, &inst->operands[0], A64_X9);
                lr_minst_t *test = minst_new(mf->arena, LR_MIR_TEST);
                test->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                test->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                test->size = 1;
                mblock_append(mb, test);

                emit_load_operand(mf, mb, &inst->operands[2], A64_X9);
                emit_load_operand(mf, mb, &inst->operands[1], A64_X10);
                lr_minst_t *cmov = minst_new(mf->arena, LR_MIR_CMOVCC);
                cmov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                cmov->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X10 };
                cmov->cc = LR_CC_NE;
                cmov->size = 8;
                mblock_append(mb, cmov);

                emit_store_slot(mf, mb, inst->dest, A64_X9);
                break;
            }
            case LR_OP_BR: {
                mb->before_term = mb->last;
                uint32_t target_id = inst->operands[0].block_id;
                lr_minst_t *mi = minst_new(mf->arena, LR_MIR_JMP);
                mi->dst = (lr_moperand_t){ .kind = LR_MOP_LABEL, .label = target_id };
                mblock_append(mb, mi);
                break;
            }
            case LR_OP_CONDBR: {
                mb->before_term = mb->last;
                emit_load_operand(mf, mb, &inst->operands[0], A64_X9);
                lr_minst_t *test = minst_new(mf->arena, LR_MIR_TEST);
                test->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                test->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                test->size = 1;
                mblock_append(mb, test);

                uint32_t true_id = inst->operands[1].block_id;
                uint32_t false_id = inst->operands[2].block_id;

                lr_minst_t *jcc = minst_new(mf->arena, LR_MIR_JCC);
                jcc->dst = (lr_moperand_t){ .kind = LR_MOP_LABEL, .label = true_id };
                jcc->cc = LR_CC_NE;
                mblock_append(mb, jcc);

                lr_minst_t *jmp = minst_new(mf->arena, LR_MIR_JMP);
                jmp->dst = (lr_moperand_t){ .kind = LR_MOP_LABEL, .label = false_id };
                mblock_append(mb, jmp);
                break;
            }
            case LR_OP_ALLOCA: {
                size_t sz = lr_type_size(inst->type);
                if (sz < 8) sz = 8;
                mf->stack_size += (uint32_t)sz;
                mf->stack_size = (mf->stack_size + 7) & ~7u;
                int32_t off = -(int32_t)mf->stack_size;

                lr_minst_t *lea = minst_new(mf->arena, LR_MIR_LEA);
                lea->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                lea->src = (lr_moperand_t){ .kind = LR_MOP_MEM, .mem = { .base = A64_FP, .disp = off } };
                lea->size = 8;
                mblock_append(mb, lea);
                emit_store_slot(mf, mb, inst->dest, A64_X9);
                break;
            }
            case LR_OP_LOAD: {
                emit_load_operand(mf, mb, &inst->operands[0], A64_X9);
                lr_minst_t *ld = minst_new(mf->arena, LR_MIR_MOV);
                ld->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                ld->src = (lr_moperand_t){ .kind = LR_MOP_MEM, .mem = { .base = A64_X9, .disp = 0 } };
                ld->size = (uint8_t)lr_type_size(inst->type);
                mblock_append(mb, ld);
                emit_store_slot(mf, mb, inst->dest, A64_X9);
                break;
            }
            case LR_OP_STORE: {
                emit_load_operand(mf, mb, &inst->operands[0], A64_X9);
                emit_load_operand(mf, mb, &inst->operands[1], A64_X10);
                lr_minst_t *st = minst_new(mf->arena, LR_MIR_MOV);
                st->dst = (lr_moperand_t){ .kind = LR_MOP_MEM, .mem = { .base = A64_X10, .disp = 0 } };
                st->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                st->size = (uint8_t)lr_type_size(inst->operands[0].type);
                mblock_append(mb, st);
                break;
            }
            case LR_OP_GEP: {
                emit_load_operand(mf, mb, &inst->operands[0], A64_X9);
                const lr_type_t *cur_ty = inst->type;
                for (uint32_t idx = 1; idx < inst->num_operands; idx++) {
                    const lr_operand_t *idx_op = &inst->operands[idx];
                    int64_t byte_off = 0;
                    bool is_const = (idx_op->kind == LR_VAL_IMM_I64);
                    if (idx == 1) {
                        size_t elem_size = lr_type_size(cur_ty);
                        if (is_const) {
                            byte_off = idx_op->imm_i64 * (int64_t)elem_size;
                        } else {
                            emit_load_operand(mf, mb, idx_op, A64_X10);
                            if (elem_size != 1) {
                                lr_minst_t *imov = minst_new(mf->arena, LR_MIR_MOV_IMM);
                                imov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X11 };
                                imov->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = (int64_t)elem_size };
                                imov->size = 8;
                                mblock_append(mb, imov);
                                lr_minst_t *mul = minst_new(mf->arena, LR_MIR_IMUL);
                                mul->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X10 };
                                mul->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X11 };
                                mul->size = 8;
                                mblock_append(mb, mul);
                            }
                            lr_minst_t *add = minst_new(mf->arena, LR_MIR_ADD);
                            add->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                            add->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X10 };
                            add->size = 8;
                            mblock_append(mb, add);
                        }
                    } else if (cur_ty && cur_ty->kind == LR_TYPE_STRUCT) {
                        uint32_t field = (uint32_t)idx_op->imm_i64;
                        byte_off = (int64_t)struct_field_offset(cur_ty, field);
                        if (field < cur_ty->struc.num_fields)
                            cur_ty = cur_ty->struc.fields[field];
                        is_const = true;
                    } else if (cur_ty && cur_ty->kind == LR_TYPE_ARRAY) {
                        size_t elem_size = lr_type_size(cur_ty->array.elem);
                        if (is_const) {
                            byte_off = idx_op->imm_i64 * (int64_t)elem_size;
                        } else {
                            emit_load_operand(mf, mb, idx_op, A64_X10);
                            if (elem_size != 1) {
                                lr_minst_t *imov = minst_new(mf->arena, LR_MIR_MOV_IMM);
                                imov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X11 };
                                imov->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = (int64_t)elem_size };
                                imov->size = 8;
                                mblock_append(mb, imov);
                                lr_minst_t *mul = minst_new(mf->arena, LR_MIR_IMUL);
                                mul->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X10 };
                                mul->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X11 };
                                mul->size = 8;
                                mblock_append(mb, mul);
                            }
                            lr_minst_t *add = minst_new(mf->arena, LR_MIR_ADD);
                            add->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                            add->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X10 };
                            add->size = 8;
                            mblock_append(mb, add);
                        }
                        cur_ty = cur_ty->array.elem;
                    }
                    if (is_const && byte_off != 0) {
                        lr_minst_t *imov = minst_new(mf->arena, LR_MIR_MOV_IMM);
                        imov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X10 };
                        imov->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = byte_off };
                        imov->size = 8;
                        mblock_append(mb, imov);
                        lr_minst_t *add = minst_new(mf->arena, LR_MIR_ADD);
                        add->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                        add->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X10 };
                        add->size = 8;
                        mblock_append(mb, add);
                    }
                }
                emit_store_slot(mf, mb, inst->dest, A64_X9);
                break;
            }
            case LR_OP_SEXT: {
                emit_load_operand(mf, mb, &inst->operands[0], A64_X9);
                lr_minst_t *mi = minst_new(mf->arena, LR_MIR_MOVSX);
                mi->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                mi->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                mi->size = (uint8_t)lr_type_size(inst->type);
                mblock_append(mb, mi);
                emit_store_slot(mf, mb, inst->dest, A64_X9);
                break;
            }
            case LR_OP_ZEXT: case LR_OP_TRUNC: case LR_OP_BITCAST:
            case LR_OP_PTRTOINT: case LR_OP_INTTOPTR: {
                emit_load_operand(mf, mb, &inst->operands[0], A64_X9);
                emit_store_slot(mf, mb, inst->dest, A64_X9);
                break;
            }
            case LR_OP_FCMP: {
                bool is_f32 = inst->operands[0].type &&
                              inst->operands[0].type->kind == LR_TYPE_FLOAT;
                int64_t fn_addr = (int64_t)(uintptr_t)(is_f32
                    ? &fp_cmp_f32_bits : &fp_cmp_f64_bits);
                emit_load_operand(mf, mb, &inst->operands[0], A64_X0);
                emit_load_operand(mf, mb, &inst->operands[1], A64_X1);
                lr_minst_t *pred_mov = minst_new(mf->arena, LR_MIR_MOV_IMM);
                pred_mov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X2 };
                pred_mov->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = (int64_t)inst->fcmp_pred };
                pred_mov->size = 8;
                mblock_append(mb, pred_mov);
                lr_minst_t *mov = minst_new(mf->arena, LR_MIR_MOV_IMM);
                mov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X16 };
                mov->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = fn_addr };
                mov->size = 8;
                mblock_append(mb, mov);
                lr_minst_t *call = minst_new(mf->arena, LR_MIR_CALL);
                call->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X16 };
                mblock_append(mb, call);
                emit_store_slot(mf, mb, inst->dest, A64_X0);
                break;
            }
            case LR_OP_SITOFP: {
                bool dst_f32 = inst->type && inst->type->kind == LR_TYPE_FLOAT;
                int64_t fn_addr = (int64_t)(uintptr_t)(dst_f32
                    ? &fp_sitofp_i64_f32 : &fp_sitofp_i64_f64);
                emit_load_operand(mf, mb, &inst->operands[0], A64_X0);
                lr_minst_t *mov = minst_new(mf->arena, LR_MIR_MOV_IMM);
                mov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X16 };
                mov->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = fn_addr };
                mov->size = 8;
                mblock_append(mb, mov);
                lr_minst_t *call = minst_new(mf->arena, LR_MIR_CALL);
                call->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X16 };
                mblock_append(mb, call);
                emit_store_slot(mf, mb, inst->dest, A64_X0);
                break;
            }
            case LR_OP_FPTOSI: {
                bool src_f32 = inst->operands[0].type &&
                               inst->operands[0].type->kind == LR_TYPE_FLOAT;
                int64_t fn_addr = (int64_t)(uintptr_t)(src_f32
                    ? &fp_fptosi_f32_i64 : &fp_fptosi_f64_i64);
                emit_load_operand(mf, mb, &inst->operands[0], A64_X0);
                lr_minst_t *mov = minst_new(mf->arena, LR_MIR_MOV_IMM);
                mov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X16 };
                mov->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = fn_addr };
                mov->size = 8;
                mblock_append(mb, mov);
                lr_minst_t *call = minst_new(mf->arena, LR_MIR_CALL);
                call->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X16 };
                mblock_append(mb, call);
                emit_store_slot(mf, mb, inst->dest, A64_X0);
                break;
            }
            case LR_OP_FPEXT: {
                int64_t fn_addr = (int64_t)(uintptr_t)&fp_fpext_f32_f64;
                emit_load_operand(mf, mb, &inst->operands[0], A64_X0);
                lr_minst_t *mov = minst_new(mf->arena, LR_MIR_MOV_IMM);
                mov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X16 };
                mov->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = fn_addr };
                mov->size = 8;
                mblock_append(mb, mov);
                lr_minst_t *call = minst_new(mf->arena, LR_MIR_CALL);
                call->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X16 };
                mblock_append(mb, call);
                emit_store_slot(mf, mb, inst->dest, A64_X0);
                break;
            }
            case LR_OP_FPTRUNC: {
                int64_t fn_addr = (int64_t)(uintptr_t)&fp_fptrunc_f64_f32;
                emit_load_operand(mf, mb, &inst->operands[0], A64_X0);
                lr_minst_t *mov = minst_new(mf->arena, LR_MIR_MOV_IMM);
                mov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X16 };
                mov->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = fn_addr };
                mov->size = 8;
                mblock_append(mb, mov);
                lr_minst_t *call = minst_new(mf->arena, LR_MIR_CALL);
                call->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X16 };
                mblock_append(mb, call);
                emit_store_slot(mf, mb, inst->dest, A64_X0);
                break;
            }
            case LR_OP_EXTRACTVALUE: {
                emit_load_operand(mf, mb, &inst->operands[0], A64_X9);
                emit_store_slot(mf, mb, inst->dest, A64_X9);
                break;
            }
            case LR_OP_INSERTVALUE: {
                emit_load_operand(mf, mb, &inst->operands[0], A64_X9);
                emit_store_slot(mf, mb, inst->dest, A64_X9);
                break;
            }
            case LR_OP_CALL: {
                static const uint8_t call_regs[] = {
                    A64_X0, A64_X1, A64_X2, A64_X3, A64_X4, A64_X5, A64_X6, A64_X7
                };
                uint32_t nargs = inst->num_operands - 1;
                uint32_t nstack = nargs > 8 ? nargs - 8 : 0;
                /* Round stack arg space to 16-byte alignment (AAPCS64) */
                uint32_t stack_bytes = ((nstack * 8 + 15) & ~15u);

                /* Reserve stack space for arguments beyond the first 8 */
                if (stack_bytes > 0) {
                    lr_minst_t *alloc = minst_new(mf->arena, LR_MIR_FRAME_ALLOC);
                    alloc->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = (int64_t)stack_bytes };
                    mblock_append(mb, alloc);
                }

                /* Store stack args to [SP + offset] */
                for (uint32_t i = 0; i < nstack; i++) {
                    uint32_t arg_idx = 8 + i;
                    emit_load_operand(mf, mb, &inst->operands[arg_idx + 1], A64_X9);
                    lr_minst_t *st = minst_new(mf->arena, LR_MIR_MOV);
                    st->dst = (lr_moperand_t){ .kind = LR_MOP_MEM, .mem = { .base = A64_SP, .disp = (int32_t)(i * 8) } };
                    st->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X9 };
                    st->size = 8;
                    mblock_append(mb, st);
                }

                /* Place first 8 args in AAPCS64 registers */
                for (uint32_t i = 0; i < nargs && i < 8; i++) {
                    emit_load_operand(mf, mb, &inst->operands[i + 1], call_regs[i]);
                }

                emit_load_operand(mf, mb, &inst->operands[0], A64_X16);
                lr_minst_t *call = minst_new(mf->arena, LR_MIR_CALL);
                call->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = A64_X16 };
                mblock_append(mb, call);

                /* Reclaim stack space after call */
                if (stack_bytes > 0) {
                    lr_minst_t *dealloc = minst_new(mf->arena, LR_MIR_FRAME_FREE);
                    dealloc->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = (int64_t)stack_bytes };
                    mblock_append(mb, dealloc);
                }

                if (inst->type && inst->type->kind != LR_TYPE_VOID)
                    emit_store_slot(mf, mb, inst->dest, A64_X0);
                break;
            }
            case LR_OP_PHI: {
                alloc_slot(mf, inst->dest, 8);
                break;
            }
            case LR_OP_UNREACHABLE: {
                break;
            }
            default:
                break;
            }
        }
    }

    /* Emit PHI stores: insert stores before terminators in predecessors */
    bi = 0;
    for (lr_block_t *b = func->first_block; b; b = b->next, bi++) {
        for (lr_inst_t *inst = b->first; inst; inst = inst->next) {
            if (inst->op != LR_OP_PHI) continue;
            for (uint32_t i = 0; i + 1 < inst->num_operands; i += 2) {
                uint32_t pred_id = inst->operands[i + 1].block_id;
                lr_mblock_t *pred_mb = mf->first_block;
                for (uint32_t j = 0; j < pred_id && pred_mb; j++)
                    pred_mb = pred_mb->next;
                if (!pred_mb) continue;

                lr_mblock_t tmp = {0};
                emit_load_operand(mf, &tmp, &inst->operands[i], A64_X9);
                emit_store_slot(mf, &tmp, inst->dest, A64_X9);

                lr_minst_t *bt = pred_mb->before_term;
                if (bt) {
                    lr_minst_t *term_start = bt->next;
                    bt->next = tmp.first;
                    tmp.last->next = term_start;
                } else {
                    tmp.last->next = pred_mb->first;
                    pred_mb->first = tmp.first;
                }
                pred_mb->before_term = tmp.last;
            }
        }
    }

    mf->stack_size = (mf->stack_size + 15) & ~15u;

    return 0;
}

/*
 * aarch64 binary encoder.
 *
 * Prologue: stp x29, x30, [sp, #-16]!; mov x29, sp; sub sp, sp, N
 * Epilogue: add sp, sp, N; ldp x29, x30, [sp], #16; ret
 */

static void emit_u32(uint8_t *buf, size_t *pos, size_t len, uint32_t insn) {
    if (*pos + 4 <= len) {
        buf[*pos + 0] = (uint8_t)(insn >> 0);
        buf[*pos + 1] = (uint8_t)(insn >> 8);
        buf[*pos + 2] = (uint8_t)(insn >> 16);
        buf[*pos + 3] = (uint8_t)(insn >> 24);
    }
    *pos += 4;
}

static void patch_u32(uint8_t *buf, size_t len, size_t pos, uint32_t insn) {
    if (pos + 4 > len) return;
    buf[pos + 0] = (uint8_t)(insn >> 0);
    buf[pos + 1] = (uint8_t)(insn >> 8);
    buf[pos + 2] = (uint8_t)(insn >> 16);
    buf[pos + 3] = (uint8_t)(insn >> 24);
}

static uint32_t enc_add_imm(bool is64, uint8_t rd, uint8_t rn, uint32_t imm12) {
    return (is64 ? 0x91000000u : 0x11000000u) | ((imm12 & 0xFFFu) << 10)
         | ((uint32_t)rn << 5) | rd;
}

static uint32_t enc_sub_imm(bool is64, uint8_t rd, uint8_t rn, uint32_t imm12) {
    return (is64 ? 0xD1000000u : 0x51000000u) | ((imm12 & 0xFFFu) << 10)
         | ((uint32_t)rn << 5) | rd;
}

static uint32_t enc_add_reg(bool is64, uint8_t rd, uint8_t rn, uint8_t rm) {
    return (is64 ? 0x8B000000u : 0x0B000000u) | ((uint32_t)rm << 16)
         | ((uint32_t)rn << 5) | rd;
}

static uint32_t enc_sub_reg(bool is64, uint8_t rd, uint8_t rn, uint8_t rm) {
    return (is64 ? 0xCB000000u : 0x4B000000u) | ((uint32_t)rm << 16)
         | ((uint32_t)rn << 5) | rd;
}

static uint32_t enc_logic_reg(uint32_t base64, bool is64, uint8_t rd,
                              uint8_t rn, uint8_t rm) {
    uint32_t base32 = base64 - 0x80000000u;
    return (is64 ? base64 : base32) | ((uint32_t)rm << 16) | ((uint32_t)rn << 5)
         | rd;
}

static uint32_t enc_subs_reg(bool is64, uint8_t rn, uint8_t rm) {
    return (is64 ? 0xEB00001Fu : 0x6B00001Fu) | ((uint32_t)rm << 16)
         | ((uint32_t)rn << 5);
}

static uint32_t enc_ands_reg(bool is64, uint8_t rn, uint8_t rm) {
    return (is64 ? 0xEA00001Fu : 0x6A00001Fu) | ((uint32_t)rm << 16)
         | ((uint32_t)rn << 5);
}

static uint32_t enc_mul(bool is64, uint8_t rd, uint8_t rn, uint8_t rm) {
    return (is64 ? 0x9B007C00u : 0x1B007C00u) | ((uint32_t)rm << 16)
         | ((uint32_t)rn << 5) | rd;
}

static uint32_t enc_sdiv(bool is64, uint8_t rd, uint8_t rn, uint8_t rm) {
    return (is64 ? 0x9AC00C00u : 0x1AC00C00u) | ((uint32_t)rm << 16)
         | ((uint32_t)rn << 5) | rd;
}

static uint32_t enc_msub(bool is64, uint8_t rd, uint8_t rn, uint8_t rm, uint8_t ra) {
    return (is64 ? 0x9B008000u : 0x1B008000u) | ((uint32_t)rm << 16)
         | ((uint32_t)ra << 10) | ((uint32_t)rn << 5) | rd;
}

static uint32_t enc_shiftv(lr_mir_op_t op, bool is64, uint8_t rd, uint8_t rn,
                           uint8_t rm) {
    uint32_t base;
    switch (op) {
    case LR_MIR_SHR: base = is64 ? 0x9AC02400u : 0x1AC02400u; break;
    case LR_MIR_SAR: base = is64 ? 0x9AC02800u : 0x1AC02800u; break;
    default:         base = is64 ? 0x9AC02000u : 0x1AC02000u; break;
    }
    return base | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rd;
}

static uint32_t enc_csel(bool is64, uint8_t rd, uint8_t rn, uint8_t rm,
                         uint8_t cond) {
    uint32_t base = is64 ? 0x9A800000u : 0x1A800000u;
    return base | ((uint32_t)rm << 16) | ((uint32_t)(cond & 0xF) << 12)
         | ((uint32_t)rn << 5) | rd;
}

static uint32_t enc_movz(bool is64, uint8_t rd, uint16_t imm16, uint8_t shift16) {
    uint32_t base = is64 ? 0xD2800000u : 0x52800000u;
    return base | ((uint32_t)(shift16 & 3) << 21) | ((uint32_t)imm16 << 5) | rd;
}

static uint32_t enc_movk(bool is64, uint8_t rd, uint16_t imm16, uint8_t shift16) {
    uint32_t base = is64 ? 0xF2800000u : 0x72800000u;
    return base | ((uint32_t)(shift16 & 3) << 21) | ((uint32_t)imm16 << 5) | rd;
}

static uint32_t enc_ldur(uint8_t size, uint8_t rt, uint8_t rn, int32_t imm9) {
    uint32_t base;
    switch (size) {
    case 1: base = 0x38400000u; break;
    case 2: base = 0x78400000u; break;
    case 4: base = 0xB8400000u; break;
    default: base = 0xF8400000u; break;
    }
    return base | ((uint32_t)(imm9 & 0x1FF) << 12) | ((uint32_t)rn << 5) | rt;
}

static uint32_t enc_stur(uint8_t size, uint8_t rt, uint8_t rn, int32_t imm9) {
    uint32_t base;
    switch (size) {
    case 1: base = 0x38000000u; break;
    case 2: base = 0x78000000u; break;
    case 4: base = 0xB8000000u; break;
    default: base = 0xF8000000u; break;
    }
    return base | ((uint32_t)(imm9 & 0x1FF) << 12) | ((uint32_t)rn << 5) | rt;
}

static void emit_move_imm(uint8_t *buf, size_t *pos, size_t len, uint8_t rd,
                          int64_t imm, bool is64) {
    uint64_t v = (uint64_t)imm;
    if (!is64) v &= 0xFFFFFFFFu;

    emit_u32(buf, pos, len, enc_movz(is64, rd, (uint16_t)(v & 0xFFFFu), 0));
    for (uint8_t s = 1; s < (is64 ? 4 : 2); s++) {
        uint16_t part = (uint16_t)((v >> (16u * s)) & 0xFFFFu);
        if (part != 0)
            emit_u32(buf, pos, len, enc_movk(is64, rd, part, s));
    }
}

static void emit_sp_adjust(uint8_t *buf, size_t *pos, size_t len, uint32_t amount,
                           bool subtract) {
    while (amount > 0) {
        uint32_t chunk = amount > 4095u ? 4095u : amount;
        uint32_t insn = subtract ? enc_sub_imm(true, A64_SP, A64_SP, chunk)
                                 : enc_add_imm(true, A64_SP, A64_SP, chunk);
        emit_u32(buf, pos, len, insn);
        amount -= chunk;
    }
}

static void emit_addr(uint8_t *buf, size_t *pos, size_t len, uint8_t rd,
                      uint8_t base, int32_t disp) {
    if (disp >= 0 && disp <= 4095) {
        emit_u32(buf, pos, len, enc_add_imm(true, rd, base, (uint32_t)disp));
        return;
    }
    if (disp < 0 && disp >= -4095) {
        emit_u32(buf, pos, len, enc_sub_imm(true, rd, base, (uint32_t)(-disp)));
        return;
    }

    emit_move_imm(buf, pos, len, A64_X15, disp, true);
    emit_u32(buf, pos, len, enc_add_reg(true, rd, base, A64_X15));
}

static void emit_load(uint8_t *buf, size_t *pos, size_t len, uint8_t rt,
                      uint8_t rn, int32_t disp, uint8_t size) {
    if (disp >= -256 && disp <= 255) {
        emit_u32(buf, pos, len, enc_ldur(size, rt, rn, disp));
        return;
    }
    emit_addr(buf, pos, len, A64_X15, rn, disp);
    emit_u32(buf, pos, len, enc_ldur(size, rt, A64_X15, 0));
}

static void emit_store(uint8_t *buf, size_t *pos, size_t len, uint8_t rt,
                       uint8_t rn, int32_t disp, uint8_t size) {
    if (disp >= -256 && disp <= 255) {
        emit_u32(buf, pos, len, enc_stur(size, rt, rn, disp));
        return;
    }
    emit_addr(buf, pos, len, A64_X15, rn, disp);
    emit_u32(buf, pos, len, enc_stur(size, rt, A64_X15, 0));
}

static void emit_mov_reg(uint8_t *buf, size_t *pos, size_t len, uint8_t rd,
                         uint8_t rm, bool is64) {
    emit_u32(buf, pos, len, enc_logic_reg(0xAA000000u, is64, rd, A64_SP, rm));
}

static uint8_t lr_cc_to_a64(uint8_t cc) {
    switch (cc) {
    case LR_CC_EQ:  return 0;  /* eq */
    case LR_CC_NE:  return 1;  /* ne */
    case LR_CC_UGT: return 8;  /* hi */
    case LR_CC_UGE: return 2;  /* hs/cs */
    case LR_CC_ULT: return 3;  /* lo/cc */
    case LR_CC_ULE: return 9;  /* ls */
    case LR_CC_SGT: return 12; /* gt */
    case LR_CC_SGE: return 10; /* ge */
    case LR_CC_SLT: return 11; /* lt */
    case LR_CC_SLE: return 13; /* le */
    case LR_CC_O:   return 6;  /* vs */
    case LR_CC_NO:  return 7;  /* vc */
    default:        return 0;
    }
}

static int aarch64_encode_func(lr_mfunc_t *mf, uint8_t *buf, size_t buflen,
                               size_t *out_len) {
    size_t pos = 0;
    size_t block_offsets[1024];
    struct fixup_t {
        size_t insn_pos;
        uint32_t target;
        uint8_t kind;
        uint8_t cond;
    } fixups[4096];
    uint32_t nfix = 0;
    uint32_t block_idx = 0;

    emit_u32(buf, &pos, buflen, 0xA9BF7BFDu); /* stp x29, x30, [sp, #-16]! */
    emit_u32(buf, &pos, buflen, 0x910003FDu); /* mov x29, sp */
    if (mf->stack_size > 0)
        emit_sp_adjust(buf, &pos, buflen, mf->stack_size, true);

    for (lr_mblock_t *mb = mf->first_block; mb; mb = mb->next, block_idx++) {
        block_offsets[block_idx] = pos;

        for (lr_minst_t *mi = mb->first; mi; mi = mi->next) {
            bool is64 = mi->size > 4;
            uint8_t dst = mi->dst.reg;
            uint8_t src = mi->src.reg;

            switch (mi->op) {
            case LR_MIR_RET:
                if (mf->stack_size > 0)
                    emit_sp_adjust(buf, &pos, buflen, mf->stack_size, false);
                emit_u32(buf, &pos, buflen, 0xA8C17BFDu); /* ldp x29, x30, [sp], #16 */
                emit_u32(buf, &pos, buflen, 0xD65F03C0u); /* ret */
                break;

            case LR_MIR_MOV_IMM:
                emit_move_imm(buf, &pos, buflen, dst, mi->src.imm, is64);
                break;

            case LR_MIR_MOV:
                if (mi->src.kind == LR_MOP_MEM && mi->dst.kind == LR_MOP_REG) {
                    emit_load(buf, &pos, buflen, dst, mi->src.mem.base,
                              mi->src.mem.disp, mi->size);
                } else if (mi->dst.kind == LR_MOP_MEM && mi->src.kind == LR_MOP_REG) {
                    emit_store(buf, &pos, buflen, src, mi->dst.mem.base,
                               mi->dst.mem.disp, mi->size);
                } else if (mi->src.kind == LR_MOP_REG && mi->dst.kind == LR_MOP_REG) {
                    emit_mov_reg(buf, &pos, buflen, dst, src, is64);
                }
                break;

            case LR_MIR_ADD:
                emit_u32(buf, &pos, buflen, enc_add_reg(is64, dst, dst, src));
                break;
            case LR_MIR_SUB:
                emit_u32(buf, &pos, buflen, enc_sub_reg(is64, dst, dst, src));
                break;
            case LR_MIR_AND:
                emit_u32(buf, &pos, buflen,
                         enc_logic_reg(0x8A000000u, is64, dst, dst, src));
                break;
            case LR_MIR_OR:
                emit_u32(buf, &pos, buflen,
                         enc_logic_reg(0xAA000000u, is64, dst, dst, src));
                break;
            case LR_MIR_XOR:
                emit_u32(buf, &pos, buflen,
                         enc_logic_reg(0xCA000000u, is64, dst, dst, src));
                break;

            case LR_MIR_IMUL:
                emit_u32(buf, &pos, buflen, enc_mul(is64, dst, dst, src));
                break;

            case LR_MIR_IDIV: {
                /* dst = X9 (dividend/quotient), src = X10 (divisor) */
                emit_mov_reg(buf, &pos, buflen, A64_X11, dst, is64);
                emit_u32(buf, &pos, buflen, enc_sdiv(is64, dst, dst, src));
                /* remainder into X11 = dividend - quotient * divisor */
                emit_u32(buf, &pos, buflen, enc_msub(is64, A64_X11, dst, src, A64_X11));
                break;
            }

            case LR_MIR_CDQ:
            case LR_MIR_CQO:
                break;

            case LR_MIR_SAL:
            case LR_MIR_SAR:
            case LR_MIR_SHR:
                emit_u32(buf, &pos, buflen, enc_shiftv(mi->op, is64, dst, dst, src));
                break;

            case LR_MIR_CMP:
                emit_u32(buf, &pos, buflen, enc_subs_reg(is64, dst, src));
                break;

            case LR_MIR_TEST:
                emit_u32(buf, &pos, buflen, enc_ands_reg(is64, dst, src));
                break;

            case LR_MIR_SETCC: {
                uint8_t cond = lr_cc_to_a64(mi->cc);
                emit_move_imm(buf, &pos, buflen, dst, 1, false);
                emit_u32(buf, &pos, buflen, enc_csel(false, dst, dst, A64_SP, cond));
                break;
            }

            case LR_MIR_MOVZX:
                if (mi->src.kind == LR_MOP_REG && mi->dst.kind == LR_MOP_REG && dst != src)
                    emit_mov_reg(buf, &pos, buflen, dst, src, false);
                break;

            case LR_MIR_MOVSX:
                if (is64)
                    emit_u32(buf, &pos, buflen,
                             0x93407C00u | ((uint32_t)src << 5) | dst); /* sxtw */
                break;

            case LR_MIR_CMOVCC: {
                uint8_t cond = lr_cc_to_a64(mi->cc);
                emit_u32(buf, &pos, buflen, enc_csel(is64, dst, src, dst, cond));
                break;
            }

            case LR_MIR_JMP:
                if (nfix < 4096) {
                    fixups[nfix].insn_pos = pos;
                    fixups[nfix].target = mi->dst.label;
                    fixups[nfix].kind = 0;
                    fixups[nfix].cond = 0;
                    nfix++;
                }
                emit_u32(buf, &pos, buflen, 0x14000000u);
                break;

            case LR_MIR_JCC: {
                uint8_t cond = lr_cc_to_a64(mi->cc);
                if (nfix < 4096) {
                    fixups[nfix].insn_pos = pos;
                    fixups[nfix].target = mi->dst.label;
                    fixups[nfix].kind = 1;
                    fixups[nfix].cond = cond;
                    nfix++;
                }
                emit_u32(buf, &pos, buflen, 0x54000000u);
                break;
            }

            case LR_MIR_LEA:
                emit_addr(buf, &pos, buflen, dst, mi->src.mem.base,
                          mi->src.mem.disp);
                break;

            case LR_MIR_CALL:
                emit_u32(buf, &pos, buflen, 0xD63F0000u | ((uint32_t)src << 5));
                break;

            case LR_MIR_FRAME_ALLOC:
                emit_sp_adjust(buf, &pos, buflen, (uint32_t)mi->src.imm, true);
                break;

            case LR_MIR_FRAME_FREE:
                emit_sp_adjust(buf, &pos, buflen, (uint32_t)mi->src.imm, false);
                break;

            default:
                break;
            }
        }
    }

    for (uint32_t i = 0; i < nfix; i++) {
        const struct fixup_t *fx = &fixups[i];
        if (fx->target >= block_idx) continue;

        int64_t target_pos = (int64_t)block_offsets[fx->target];
        int64_t here = (int64_t)fx->insn_pos;
        int64_t imm = (target_pos - here) / 4;

        if (fx->kind == 0) {
            if (imm >= -(1LL << 25) && imm < (1LL << 25)) {
                uint32_t insn = 0x14000000u | ((uint32_t)imm & 0x03FFFFFFu);
                patch_u32(buf, buflen, fx->insn_pos, insn);
            }
        } else {
            if (imm >= -(1LL << 18) && imm < (1LL << 18)) {
                uint32_t insn = 0x54000000u | (((uint32_t)imm & 0x7FFFFu) << 5)
                              | (fx->cond & 0xF);
                patch_u32(buf, buflen, fx->insn_pos, insn);
            }
        }
    }

    *out_len = pos;
    return 0;
}

static int aarch64_print_inst(const lr_minst_t *mi, char *buf, size_t len) {
    (void)mi;
    return snprintf(buf, len, "aarch64-op");
}

static const lr_target_t aarch64_target = {
    .name = "aarch64",
    .ptr_size = 8,
    .isel_func = aarch64_isel_func,
    .encode_func = aarch64_encode_func,
    .print_inst = aarch64_print_inst,
};

const lr_target_t *lr_target_aarch64(void) {
    return &aarch64_target;
}
