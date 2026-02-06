#include "target_x86_64.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/*
 * Phase 1: minimal ISel + encoding for "ret i32 <imm>"
 *
 * Strategy: stack-based register allocation. Every IR vreg gets a stack slot.
 * All computation goes through rax (and rdx for div). This is the fastest
 * possible compile and produces correct but slow code.
 *
 * Phase 2+ will add proper arithmetic, memory ops, control flow.
 */

static lr_minst_t *minst_new(lr_arena_t *a, lr_mir_op_t op) {
    lr_minst_t *mi = lr_arena_new(a, lr_minst_t);
    mi->op = op;
    mi->size = 8;
    return mi;
}

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

/* Allocate a stack slot for a vreg, return rbp offset (negative) */
static int32_t alloc_slot(lr_mfunc_t *mf, uint32_t vreg, uint8_t size) {
    /* Grow slot array if needed */
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
    /* Align stack_size to 'size' boundary */
    mf->stack_size = (mf->stack_size + size - 1) & ~(uint32_t)(size - 1);
    int32_t offset = -(int32_t)mf->stack_size;
    mf->stack_slots[vreg] = offset;
    return offset;
}

/* Emit: mov rax, [rbp + offset] */
static void emit_load_slot(lr_mfunc_t *mf, lr_mblock_t *mb, uint32_t vreg, uint8_t reg) {
    int32_t off = alloc_slot(mf, vreg, 8);
    lr_minst_t *mi = minst_new(mf->arena, LR_MIR_MOV);
    mi->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = reg };
    mi->src = (lr_moperand_t){ .kind = LR_MOP_MEM, .mem = { .base = X86_RBP, .disp = off } };
    mi->size = 8;
    mblock_append(mb, mi);
}

/* Emit: mov [rbp + offset], rax */
static void emit_store_slot(lr_mfunc_t *mf, lr_mblock_t *mb, uint32_t vreg, uint8_t reg) {
    int32_t off = alloc_slot(mf, vreg, 8);
    lr_minst_t *mi = minst_new(mf->arena, LR_MIR_MOV);
    mi->dst = (lr_moperand_t){ .kind = LR_MOP_MEM, .mem = { .base = X86_RBP, .disp = off } };
    mi->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = reg };
    mi->size = 8;
    mblock_append(mb, mi);
}

/* Load an operand value into rax */
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

static int x86_64_isel_func(lr_func_t *func, lr_mfunc_t *mf, lr_module_t *mod) {
    (void)mod;
    mf->ir_func = func;
    mf->name = func->name;

    /* Pre-allocate slots for parameters (System V: rdi, rsi, rdx, rcx, r8, r9) */
    static const uint8_t param_regs[] = { X86_RDI, X86_RSI, X86_RDX, X86_RCX, X86_R8, X86_R9 };

    /* Create machine blocks matching IR blocks */
    lr_mblock_t **mblocks = lr_arena_array(mf->arena, lr_mblock_t *, func->num_blocks);
    uint32_t bi = 0;
    for (lr_block_t *b = func->first_block; b; b = b->next) {
        mblocks[bi] = mblock_new(mf);
        bi++;
    }

    /* Emit parameter stores in first block */
    lr_mblock_t *entry_mb = mf->first_block;
    for (uint32_t i = 0; i < func->num_params && i < 6; i++) {
        emit_store_slot(mf, entry_mb, func->param_vregs[i], param_regs[i]);
    }

    /* Lower each IR instruction */
    bi = 0;
    for (lr_block_t *b = func->first_block; b; b = b->next, bi++) {
        lr_mblock_t *mb = mblocks[bi];

        for (lr_inst_t *inst = b->first; inst; inst = inst->next) {
            switch (inst->op) {
            case LR_OP_RET: {
                mb->before_term = mb->last;
                emit_load_operand(mf, mb, &inst->operands[0], X86_RAX);
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
                emit_load_operand(mf, mb, &inst->operands[0], X86_RAX);
                emit_load_operand(mf, mb, &inst->operands[1], X86_RCX);
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
                mi->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                mi->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RCX };
                mi->size = (uint8_t)lr_type_size(inst->type);
                mblock_append(mb, mi);
                emit_store_slot(mf, mb, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_MUL: {
                emit_load_operand(mf, mb, &inst->operands[0], X86_RAX);
                emit_load_operand(mf, mb, &inst->operands[1], X86_RCX);
                lr_minst_t *mi = minst_new(mf->arena, LR_MIR_IMUL);
                mi->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                mi->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RCX };
                mi->size = (uint8_t)lr_type_size(inst->type);
                mblock_append(mb, mi);
                emit_store_slot(mf, mb, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_FADD: case LR_OP_FSUB:
            case LR_OP_FMUL: case LR_OP_FDIV: {
                int64_t fn_addr = fp_helper_addr(inst->op, inst->type);
                emit_load_operand(mf, mb, &inst->operands[0], X86_RDI);
                emit_load_operand(mf, mb, &inst->operands[1], X86_RSI);
                lr_minst_t *mov = minst_new(mf->arena, LR_MIR_MOV_IMM);
                mov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_R10 };
                mov->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = fn_addr };
                mov->size = 8;
                mblock_append(mb, mov);
                lr_minst_t *call = minst_new(mf->arena, LR_MIR_CALL);
                call->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_R10 };
                mblock_append(mb, call);
                emit_store_slot(mf, mb, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_SDIV: case LR_OP_SREM: {
                emit_load_operand(mf, mb, &inst->operands[0], X86_RAX);
                emit_load_operand(mf, mb, &inst->operands[1], X86_RCX);
                /* sign-extend rax into rdx:rax */
                uint8_t sz = (uint8_t)lr_type_size(inst->type);
                lr_minst_t *cqo = minst_new(mf->arena, sz <= 4 ? LR_MIR_CDQ : LR_MIR_CQO);
                mblock_append(mb, cqo);
                lr_minst_t *mi = minst_new(mf->arena, LR_MIR_IDIV);
                mi->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RCX };
                mi->size = sz;
                mblock_append(mb, mi);
                /* quotient in rax, remainder in rdx */
                uint8_t res_reg = (inst->op == LR_OP_SREM) ? X86_RDX : X86_RAX;
                emit_store_slot(mf, mb, inst->dest, res_reg);
                break;
            }
            case LR_OP_SHL: case LR_OP_LSHR: case LR_OP_ASHR: {
                emit_load_operand(mf, mb, &inst->operands[0], X86_RAX);
                emit_load_operand(mf, mb, &inst->operands[1], X86_RCX);
                lr_mir_op_t mop;
                switch (inst->op) {
                case LR_OP_SHL:  mop = LR_MIR_SAL; break;
                case LR_OP_LSHR: mop = LR_MIR_SHR; break;
                case LR_OP_ASHR: mop = LR_MIR_SAR; break;
                default: mop = LR_MIR_SAL; break;
                }
                lr_minst_t *mi = minst_new(mf->arena, mop);
                mi->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                mi->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RCX };
                mi->size = (uint8_t)lr_type_size(inst->type);
                mblock_append(mb, mi);
                emit_store_slot(mf, mb, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_ICMP: {
                emit_load_operand(mf, mb, &inst->operands[0], X86_RAX);
                emit_load_operand(mf, mb, &inst->operands[1], X86_RCX);
                lr_minst_t *cmp = minst_new(mf->arena, LR_MIR_CMP);
                cmp->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                cmp->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RCX };
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

                /* setcc al; movzx eax, al (zero-extend result to full register) */
                lr_minst_t *set = minst_new(mf->arena, LR_MIR_SETCC);
                set->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                set->cc = cc;
                set->size = 1;
                mblock_append(mb, set);

                lr_minst_t *zx = minst_new(mf->arena, LR_MIR_MOVZX);
                zx->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                zx->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                zx->size = 1;
                mblock_append(mb, zx);

                emit_store_slot(mf, mb, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_SELECT: {
                /* cond in operands[0], true_val in [1], false_val in [2] */
                emit_load_operand(mf, mb, &inst->operands[0], X86_RAX);
                lr_minst_t *test = minst_new(mf->arena, LR_MIR_TEST);
                test->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                test->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                test->size = 1;
                mblock_append(mb, test);

                emit_load_operand(mf, mb, &inst->operands[2], X86_RAX);
                emit_load_operand(mf, mb, &inst->operands[1], X86_RCX);
                lr_minst_t *cmov = minst_new(mf->arena, LR_MIR_CMOVCC);
                cmov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                cmov->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RCX };
                cmov->cc = LR_CC_NE;
                cmov->size = 8;
                mblock_append(mb, cmov);

                emit_store_slot(mf, mb, inst->dest, X86_RAX);
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
                emit_load_operand(mf, mb, &inst->operands[0], X86_RAX);
                lr_minst_t *test = minst_new(mf->arena, LR_MIR_TEST);
                test->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                test->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
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
                size_t elem_sz = lr_type_size(inst->type);
                if (elem_sz < 8) elem_sz = 8;

                /* Check if we can use static alloca (no operands or constant count = 1) */
                bool use_static = (inst->num_operands == 0);
                if (inst->num_operands > 0 && inst->operands[0].kind == LR_VAL_IMM_I64 &&
                    inst->operands[0].imm_i64 == 1) {
                    use_static = true;
                }

                if (use_static) {
                    /* Static alloca: just allocate a stack slot, store its address */
                    mf->stack_size += (uint32_t)elem_sz;
                    mf->stack_size = (mf->stack_size + 7) & ~7u;
                    int32_t off = -(int32_t)mf->stack_size;

                    lr_minst_t *lea = minst_new(mf->arena, LR_MIR_LEA);
                    lea->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                    lea->src = (lr_moperand_t){ .kind = LR_MOP_MEM, .mem = { .base = X86_RBP, .disp = off } };
                    lea->size = 8;
                    mblock_append(mb, lea);
                    emit_store_slot(mf, mb, inst->dest, X86_RAX);
                } else {
                    /* Dynamic alloca: alloca <type>, <count_type> <count_operand> */
                    /* Load count into RAX */
                    emit_load_operand(mf, mb, &inst->operands[0], X86_RAX);

                    /* Multiply count by element size: RAX = RAX * elem_sz */
                    if (elem_sz != 1) {
                        lr_minst_t *mov_size = minst_new(mf->arena, LR_MIR_MOV_IMM);
                        mov_size->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RCX };
                        mov_size->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = (int64_t)elem_sz };
                        mov_size->size = 8;
                        mblock_append(mb, mov_size);

                        lr_minst_t *mul = minst_new(mf->arena, LR_MIR_IMUL);
                        mul->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                        mul->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RCX };
                        mul->size = 8;
                        mblock_append(mb, mul);
                    }

                    /* Align total size to 16 bytes: RAX = (RAX + 15) & ~15 */
                    lr_minst_t *add_align = minst_new(mf->arena, LR_MIR_ADD);
                    add_align->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                    add_align->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = 15 };
                    add_align->size = 8;
                    mblock_append(mb, add_align);

                    lr_minst_t *and_align = minst_new(mf->arena, LR_MIR_AND);
                    and_align->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                    and_align->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = ~15LL };
                    and_align->size = 8;
                    mblock_append(mb, and_align);

                    /* Subtract from RSP: RSP = RSP - RAX */
                    lr_minst_t *sub = minst_new(mf->arena, LR_MIR_SUB);
                    sub->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RSP };
                    sub->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                    sub->size = 8;
                    mblock_append(mb, sub);

                    /* Result pointer is now RSP, move to RAX */
                    lr_minst_t *mov_rsp = minst_new(mf->arena, LR_MIR_MOV);
                    mov_rsp->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                    mov_rsp->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RSP };
                    mov_rsp->size = 8;
                    mblock_append(mb, mov_rsp);

                    emit_store_slot(mf, mb, inst->dest, X86_RAX);
                }
                break;
            }
            case LR_OP_LOAD: {
                /* load from ptr in operands[0] */
                emit_load_operand(mf, mb, &inst->operands[0], X86_RAX);
                lr_minst_t *ld = minst_new(mf->arena, LR_MIR_MOV);
                ld->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                ld->src = (lr_moperand_t){ .kind = LR_MOP_MEM, .mem = { .base = X86_RAX, .disp = 0 } };
                ld->size = (uint8_t)lr_type_size(inst->type);
                mblock_append(mb, ld);
                emit_store_slot(mf, mb, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_STORE: {
                /* store val(operands[0]) to ptr(operands[1]) */
                emit_load_operand(mf, mb, &inst->operands[0], X86_RAX);
                emit_load_operand(mf, mb, &inst->operands[1], X86_RCX);
                lr_minst_t *st = minst_new(mf->arena, LR_MIR_MOV);
                st->dst = (lr_moperand_t){ .kind = LR_MOP_MEM, .mem = { .base = X86_RCX, .disp = 0 } };
                st->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                st->size = (uint8_t)lr_type_size(inst->operands[0].type);
                mblock_append(mb, st);
                break;
            }
            case LR_OP_GEP: {
                /* inst->type = base/pointee type for offset computation
                   operands[0] = base pointer
                   operands[1..] = indices */
                emit_load_operand(mf, mb, &inst->operands[0], X86_RAX);
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
                            emit_load_operand(mf, mb, idx_op, X86_RCX);
                            if (elem_size != 1) {
                                lr_minst_t *mov = minst_new(mf->arena, LR_MIR_MOV_IMM);
                                mov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_R10 };
                                mov->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = (int64_t)elem_size };
                                mov->size = 8;
                                mblock_append(mb, mov);
                                lr_minst_t *mul = minst_new(mf->arena, LR_MIR_IMUL);
                                mul->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RCX };
                                mul->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_R10 };
                                mul->size = 8;
                                mblock_append(mb, mul);
                            }
                            lr_minst_t *add = minst_new(mf->arena, LR_MIR_ADD);
                            add->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                            add->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RCX };
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
                            emit_load_operand(mf, mb, idx_op, X86_RCX);
                            if (elem_size != 1) {
                                lr_minst_t *mov = minst_new(mf->arena, LR_MIR_MOV_IMM);
                                mov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_R10 };
                                mov->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = (int64_t)elem_size };
                                mov->size = 8;
                                mblock_append(mb, mov);
                                lr_minst_t *mul = minst_new(mf->arena, LR_MIR_IMUL);
                                mul->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RCX };
                                mul->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_R10 };
                                mul->size = 8;
                                mblock_append(mb, mul);
                            }
                            lr_minst_t *add = minst_new(mf->arena, LR_MIR_ADD);
                            add->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                            add->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RCX };
                            add->size = 8;
                            mblock_append(mb, add);
                        }
                        cur_ty = cur_ty->array.elem;
                    }
                    if (is_const && byte_off != 0) {
                        lr_minst_t *mov = minst_new(mf->arena, LR_MIR_MOV_IMM);
                        mov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RCX };
                        mov->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = byte_off };
                        mov->size = 8;
                        mblock_append(mb, mov);
                        lr_minst_t *add = minst_new(mf->arena, LR_MIR_ADD);
                        add->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                        add->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RCX };
                        add->size = 8;
                        mblock_append(mb, add);
                    }
                }
                emit_store_slot(mf, mb, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_SEXT: {
                emit_load_operand(mf, mb, &inst->operands[0], X86_RAX);
                lr_minst_t *mi = minst_new(mf->arena, LR_MIR_MOVSX);
                mi->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                mi->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RAX };
                mi->size = (uint8_t)lr_type_size(inst->type);
                mblock_append(mb, mi);
                emit_store_slot(mf, mb, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_ZEXT: case LR_OP_TRUNC: case LR_OP_BITCAST:
            case LR_OP_PTRTOINT: case LR_OP_INTTOPTR: {
                emit_load_operand(mf, mb, &inst->operands[0], X86_RAX);
                emit_store_slot(mf, mb, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_FCMP: {
                bool is_f32 = inst->operands[0].type &&
                              inst->operands[0].type->kind == LR_TYPE_FLOAT;
                int64_t fn_addr = (int64_t)(uintptr_t)(is_f32
                    ? &fp_cmp_f32_bits : &fp_cmp_f64_bits);
                emit_load_operand(mf, mb, &inst->operands[0], X86_RDI);
                emit_load_operand(mf, mb, &inst->operands[1], X86_RSI);
                lr_minst_t *pred_mov = minst_new(mf->arena, LR_MIR_MOV_IMM);
                pred_mov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_RDX };
                pred_mov->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = (int64_t)inst->fcmp_pred };
                pred_mov->size = 8;
                mblock_append(mb, pred_mov);
                lr_minst_t *mov = minst_new(mf->arena, LR_MIR_MOV_IMM);
                mov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_R10 };
                mov->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = fn_addr };
                mov->size = 8;
                mblock_append(mb, mov);
                lr_minst_t *call = minst_new(mf->arena, LR_MIR_CALL);
                call->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_R10 };
                mblock_append(mb, call);
                emit_store_slot(mf, mb, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_SITOFP: {
                bool dst_f32 = inst->type && inst->type->kind == LR_TYPE_FLOAT;
                int64_t fn_addr = (int64_t)(uintptr_t)(dst_f32
                    ? &fp_sitofp_i64_f32 : &fp_sitofp_i64_f64);
                emit_load_operand(mf, mb, &inst->operands[0], X86_RDI);
                lr_minst_t *mov = minst_new(mf->arena, LR_MIR_MOV_IMM);
                mov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_R10 };
                mov->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = fn_addr };
                mov->size = 8;
                mblock_append(mb, mov);
                lr_minst_t *call = minst_new(mf->arena, LR_MIR_CALL);
                call->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_R10 };
                mblock_append(mb, call);
                emit_store_slot(mf, mb, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_FPTOSI: {
                bool src_f32 = inst->operands[0].type &&
                               inst->operands[0].type->kind == LR_TYPE_FLOAT;
                int64_t fn_addr = (int64_t)(uintptr_t)(src_f32
                    ? &fp_fptosi_f32_i64 : &fp_fptosi_f64_i64);
                emit_load_operand(mf, mb, &inst->operands[0], X86_RDI);
                lr_minst_t *mov = minst_new(mf->arena, LR_MIR_MOV_IMM);
                mov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_R10 };
                mov->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = fn_addr };
                mov->size = 8;
                mblock_append(mb, mov);
                lr_minst_t *call = minst_new(mf->arena, LR_MIR_CALL);
                call->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_R10 };
                mblock_append(mb, call);
                emit_store_slot(mf, mb, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_FPEXT: {
                int64_t fn_addr = (int64_t)(uintptr_t)&fp_fpext_f32_f64;
                emit_load_operand(mf, mb, &inst->operands[0], X86_RDI);
                lr_minst_t *mov = minst_new(mf->arena, LR_MIR_MOV_IMM);
                mov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_R10 };
                mov->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = fn_addr };
                mov->size = 8;
                mblock_append(mb, mov);
                lr_minst_t *call = minst_new(mf->arena, LR_MIR_CALL);
                call->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_R10 };
                mblock_append(mb, call);
                emit_store_slot(mf, mb, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_FPTRUNC: {
                int64_t fn_addr = (int64_t)(uintptr_t)&fp_fptrunc_f64_f32;
                emit_load_operand(mf, mb, &inst->operands[0], X86_RDI);
                lr_minst_t *mov = minst_new(mf->arena, LR_MIR_MOV_IMM);
                mov->dst = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_R10 };
                mov->src = (lr_moperand_t){ .kind = LR_MOP_IMM, .imm = fn_addr };
                mov->size = 8;
                mblock_append(mb, mov);
                lr_minst_t *call = minst_new(mf->arena, LR_MIR_CALL);
                call->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_R10 };
                mblock_append(mb, call);
                emit_store_slot(mf, mb, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_EXTRACTVALUE: {
                emit_load_operand(mf, mb, &inst->operands[0], X86_RAX);
                emit_store_slot(mf, mb, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_INSERTVALUE: {
                emit_load_operand(mf, mb, &inst->operands[0], X86_RAX);
                emit_store_slot(mf, mb, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_CALL: {
                /* operands[0] = callee, operands[1..] = args */
                /* Place args in System V registers */
                static const uint8_t call_regs[] = { X86_RDI, X86_RSI, X86_RDX, X86_RCX, X86_R8, X86_R9 };
                uint32_t nargs = inst->num_operands - 1;
                for (uint32_t i = 0; i < nargs && i < 6; i++) {
                    emit_load_operand(mf, mb, &inst->operands[i + 1], call_regs[i]);
                }
                /* Load callee address into r10 */
                emit_load_operand(mf, mb, &inst->operands[0], X86_R10);
                lr_minst_t *call = minst_new(mf->arena, LR_MIR_CALL);
                call->src = (lr_moperand_t){ .kind = LR_MOP_REG, .reg = X86_R10 };
                mblock_append(mb, call);
                /* result in rax */
                if (inst->type && inst->type->kind != LR_TYPE_VOID)
                    emit_store_slot(mf, mb, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_PHI: {
                /* PHI lowering: for stack-based allocation, we handle this by
                   pre-storing values. At this phase, just allocate the slot. */
                alloc_slot(mf, inst->dest, 8);
                break;
            }
            case LR_OP_UNREACHABLE: {
                /* emit ud2 or just nothing for now */
                break;
            }
            default:
                break;
            }
        }
    }

    /* Emit PHI stores: for each phi, insert stores before terminators in predecessors. */
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

                /* Build PHI moves into a temp list */
                lr_mblock_t tmp = {0};
                emit_load_operand(mf, &tmp, &inst->operands[i], X86_RAX);
                emit_store_slot(mf, &tmp, inst->dest, X86_RAX);

                /* Splice before the terminator using before_term marker */
                lr_minst_t *bt = pred_mb->before_term;
                if (bt) {
                    /* Insert after before_term, before terminator code */
                    lr_minst_t *term_start = bt->next;
                    bt->next = tmp.first;
                    tmp.last->next = term_start;
                } else {
                    /* No before_term: block starts with terminator - prepend */
                    tmp.last->next = pred_mb->first;
                    pred_mb->first = tmp.first;
                }
                /* Update before_term to point to end of inserted PHI moves
                   so subsequent PHIs for the same predecessor insert correctly */
                pred_mb->before_term = tmp.last;
            }
        }
    }

    /* Align stack to 16 bytes */
    mf->stack_size = (mf->stack_size + 15) & ~15u;

    return 0;
}

/*
 * x86_64 binary encoder.
 *
 * We emit: push rbp; mov rbp,rsp; sub rsp,N; ... ; add rsp,N; pop rbp; ret
 */

static void emit_byte(uint8_t *buf, size_t *pos, size_t len, uint8_t b) {
    if (*pos < len) buf[*pos] = b;
    (*pos)++;
}

static void emit_u32(uint8_t *buf, size_t *pos, size_t len, uint32_t v) {
    for (int i = 0; i < 4; i++)
        emit_byte(buf, pos, len, (uint8_t)(v >> (i * 8)));
}

static void emit_u64(uint8_t *buf, size_t *pos, size_t len, uint64_t v) {
    for (int i = 0; i < 8; i++)
        emit_byte(buf, pos, len, (uint8_t)(v >> (i * 8)));
}

/* REX prefix */
static uint8_t rex(bool w, bool r, bool x, bool b) {
    return (uint8_t)(0x40 | (w ? 8 : 0) | (r ? 4 : 0) | (x ? 2 : 0) | (b ? 1 : 0));
}

/* ModRM byte */
static uint8_t modrm(uint8_t mod, uint8_t reg, uint8_t rm) {
    return (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7));
}

/* Encode reg-reg ALU instruction: op dst, src (both registers) */
static void encode_alu_rr(uint8_t *buf, size_t *pos, size_t len,
                           uint8_t opcode, uint8_t dst, uint8_t src, uint8_t size) {
    bool need_rex = (size == 8) || (dst >= 8) || (src >= 8);
    if (size == 2) emit_byte(buf, pos, len, 0x66);
    if (need_rex)
        emit_byte(buf, pos, len, rex(size == 8, src >= 8, false, dst >= 8));
    emit_byte(buf, pos, len, opcode);
    emit_byte(buf, pos, len, modrm(3, src, dst));
}

/* Encode memory access: op reg, [base + disp] or op [base + disp], reg */
static void encode_mem(uint8_t *buf, size_t *pos, size_t len,
                        uint8_t opcode, uint8_t reg, uint8_t base,
                        int32_t disp, uint8_t size) {
    bool need_rex = (size == 8) || (reg >= 8) || (base >= 8);
    if (size == 2) emit_byte(buf, pos, len, 0x66);
    if (need_rex)
        emit_byte(buf, pos, len, rex(size == 8, reg >= 8, false, base >= 8));
    emit_byte(buf, pos, len, opcode);

    uint8_t mod;
    if (disp == 0 && (base & 7) != 5) mod = 0;
    else if (disp >= -128 && disp <= 127) mod = 1;
    else mod = 2;

    emit_byte(buf, pos, len, modrm(mod, reg, base));
    /* SIB byte needed when base is rsp/r12 */
    if ((base & 7) == 4)
        emit_byte(buf, pos, len, 0x24);
    if (mod == 1)
        emit_byte(buf, pos, len, (uint8_t)(int8_t)disp);
    else if (mod == 2)
        emit_u32(buf, pos, len, (uint32_t)disp);
}

static uint8_t lr_cc_to_x86(uint8_t cc) {
    switch (cc) {
    case LR_CC_EQ:  return X86_CC_E;
    case LR_CC_NE:  return X86_CC_NE;
    case LR_CC_UGT: return X86_CC_A;
    case LR_CC_UGE: return X86_CC_AE;
    case LR_CC_ULT: return X86_CC_B;
    case LR_CC_ULE: return X86_CC_BE;
    case LR_CC_SGT: return X86_CC_G;
    case LR_CC_SGE: return X86_CC_GE;
    case LR_CC_SLT: return X86_CC_L;
    case LR_CC_SLE: return X86_CC_LE;
    case LR_CC_O:   return X86_CC_O;
    case LR_CC_NO:  return X86_CC_NO;
    default:        return X86_CC_E;
    }
}

static int x86_64_encode_func(lr_mfunc_t *mf, uint8_t *buf, size_t buflen, size_t *out_len) {
    size_t pos = 0;

    /* Prologue: push rbp; mov rbp, rsp */
    emit_byte(buf, &pos, buflen, 0x55);                    /* push rbp */
    emit_byte(buf, &pos, buflen, rex(true, false, false, false));
    emit_byte(buf, &pos, buflen, 0x89);
    emit_byte(buf, &pos, buflen, modrm(3, X86_RSP, X86_RBP)); /* mov rbp, rsp */

    /* sub rsp, stack_size */
    if (mf->stack_size > 0) {
        emit_byte(buf, &pos, buflen, rex(true, false, false, false));
        emit_byte(buf, &pos, buflen, 0x81);
        emit_byte(buf, &pos, buflen, modrm(3, 5, X86_RSP)); /* sub rsp, imm32 */
        emit_u32(buf, &pos, buflen, mf->stack_size);
    }

    /* Record block offsets for branch fixup */
    size_t block_offsets[1024];
    /* Locations of branch targets to fixup: [code_pos, target_block_id] pairs */
    struct { size_t pos; uint32_t target; } fixups[4096];
    uint32_t num_fixups = 0;

    uint32_t block_idx = 0;
    for (lr_mblock_t *mb = mf->first_block; mb; mb = mb->next, block_idx++) {
        block_offsets[block_idx] = pos;

        for (lr_minst_t *mi = mb->first; mi; mi = mi->next) {
            switch (mi->op) {
            case LR_MIR_RET:
                /* Epilogue: add rsp, N; pop rbp; ret */
                if (mf->stack_size > 0) {
                    emit_byte(buf, &pos, buflen, rex(true, false, false, false));
                    emit_byte(buf, &pos, buflen, 0x81);
                    emit_byte(buf, &pos, buflen, modrm(3, 0, X86_RSP));
                    emit_u32(buf, &pos, buflen, mf->stack_size);
                }
                emit_byte(buf, &pos, buflen, 0x5D); /* pop rbp */
                emit_byte(buf, &pos, buflen, 0xC3); /* ret */
                break;

            case LR_MIR_MOV_IMM: {
                uint8_t dst = mi->dst.reg;
                int64_t imm = mi->src.imm;
                if (imm >= INT32_MIN && imm <= INT32_MAX) {
                    /* mov reg, imm32 (sign-extended) */
                    emit_byte(buf, &pos, buflen, rex(true, false, false, dst >= 8));
                    emit_byte(buf, &pos, buflen, 0xC7);
                    emit_byte(buf, &pos, buflen, modrm(3, 0, dst));
                    emit_u32(buf, &pos, buflen, (uint32_t)(int32_t)imm);
                } else {
                    /* movabs reg, imm64 */
                    emit_byte(buf, &pos, buflen, rex(true, false, false, dst >= 8));
                    emit_byte(buf, &pos, buflen, (uint8_t)(0xB8 + (dst & 7)));
                    emit_u64(buf, &pos, buflen, (uint64_t)imm);
                }
                break;
            }

            case LR_MIR_MOV: {
                if (mi->src.kind == LR_MOP_MEM && mi->dst.kind == LR_MOP_REG) {
                    /* mov reg, [base + disp] */
                    uint8_t sz = mi->size;
                    if (sz < 4) {
                        /* Use movzx for 1/2-byte loads to avoid partial register stalls */
                        uint8_t opcode2 = (sz == 1) ? 0xB6 : 0xB7;
                        bool need_rex = (mi->dst.reg >= 8) || (mi->src.mem.base >= 8);
                        if (need_rex)
                            emit_byte(buf, &pos, buflen, rex(true, mi->dst.reg >= 8, false, mi->src.mem.base >= 8));
                        else
                            emit_byte(buf, &pos, buflen, rex(true, false, false, false));
                        emit_byte(buf, &pos, buflen, 0x0F);
                        emit_byte(buf, &pos, buflen, opcode2);

                        int32_t disp = mi->src.mem.disp;
                        uint8_t base = mi->src.mem.base;
                        uint8_t mod;
                        if (disp == 0 && (base & 7) != 5) mod = 0;
                        else if (disp >= -128 && disp <= 127) mod = 1;
                        else mod = 2;
                        emit_byte(buf, &pos, buflen, modrm(mod, mi->dst.reg, base));
                        if ((base & 7) == 4) emit_byte(buf, &pos, buflen, 0x24);
                        if (mod == 1) emit_byte(buf, &pos, buflen, (uint8_t)(int8_t)disp);
                        else if (mod == 2) emit_u32(buf, &pos, buflen, (uint32_t)disp);
                    } else {
                        encode_mem(buf, &pos, buflen, 0x8B, mi->dst.reg,
                                   mi->src.mem.base, mi->src.mem.disp, sz);
                    }
                } else if (mi->dst.kind == LR_MOP_MEM && mi->src.kind == LR_MOP_REG) {
                    /* mov [base + disp], reg */
                    encode_mem(buf, &pos, buflen, 0x89, mi->src.reg,
                               mi->dst.mem.base, mi->dst.mem.disp, mi->size);
                } else if (mi->src.kind == LR_MOP_REG && mi->dst.kind == LR_MOP_REG) {
                    /* mov reg, reg */
                    encode_alu_rr(buf, &pos, buflen, 0x89, mi->dst.reg, mi->src.reg, mi->size);
                }
                break;
            }

            case LR_MIR_ADD:
                encode_alu_rr(buf, &pos, buflen, 0x01, mi->dst.reg, mi->src.reg, mi->size);
                break;
            case LR_MIR_SUB:
                encode_alu_rr(buf, &pos, buflen, 0x29, mi->dst.reg, mi->src.reg, mi->size);
                break;
            case LR_MIR_AND:
                encode_alu_rr(buf, &pos, buflen, 0x21, mi->dst.reg, mi->src.reg, mi->size);
                break;
            case LR_MIR_OR:
                encode_alu_rr(buf, &pos, buflen, 0x09, mi->dst.reg, mi->src.reg, mi->size);
                break;
            case LR_MIR_XOR:
                encode_alu_rr(buf, &pos, buflen, 0x31, mi->dst.reg, mi->src.reg, mi->size);
                break;

            case LR_MIR_IMUL: {
                /* imul dst, src (two-operand form: 0F AF /r) */
                bool need_rex = (mi->size == 8) || (mi->dst.reg >= 8) || (mi->src.reg >= 8);
                if (need_rex)
                    emit_byte(buf, &pos, buflen, rex(mi->size == 8, mi->dst.reg >= 8, false, mi->src.reg >= 8));
                emit_byte(buf, &pos, buflen, 0x0F);
                emit_byte(buf, &pos, buflen, 0xAF);
                emit_byte(buf, &pos, buflen, modrm(3, mi->dst.reg, mi->src.reg));
                break;
            }

            case LR_MIR_IDIV: {
                /* idiv src (one-operand: F7 /7) */
                bool need_rex = (mi->size == 8) || (mi->src.reg >= 8);
                if (need_rex)
                    emit_byte(buf, &pos, buflen, rex(mi->size == 8, false, false, mi->src.reg >= 8));
                emit_byte(buf, &pos, buflen, 0xF7);
                emit_byte(buf, &pos, buflen, modrm(3, 7, mi->src.reg));
                break;
            }

            case LR_MIR_CDQ:
                emit_byte(buf, &pos, buflen, 0x99);
                break;
            case LR_MIR_CQO:
                emit_byte(buf, &pos, buflen, rex(true, false, false, false));
                emit_byte(buf, &pos, buflen, 0x99);
                break;

            case LR_MIR_SAL: case LR_MIR_SAR: case LR_MIR_SHR: {
                /* shift dst, cl : D3 /4(sal) /7(sar) /5(shr) */
                uint8_t ext;
                switch (mi->op) {
                case LR_MIR_SAL: ext = 4; break;
                case LR_MIR_SAR: ext = 7; break;
                case LR_MIR_SHR: ext = 5; break;
                default: ext = 4; break;
                }
                bool need_rex = (mi->size == 8) || (mi->dst.reg >= 8);
                if (need_rex)
                    emit_byte(buf, &pos, buflen, rex(mi->size == 8, false, false, mi->dst.reg >= 8));
                emit_byte(buf, &pos, buflen, 0xD3);
                emit_byte(buf, &pos, buflen, modrm(3, ext, mi->dst.reg));
                break;
            }

            case LR_MIR_CMP:
                encode_alu_rr(buf, &pos, buflen, 0x39, mi->dst.reg, mi->src.reg, mi->size);
                break;

            case LR_MIR_TEST:
                encode_alu_rr(buf, &pos, buflen, 0x85, mi->dst.reg, mi->src.reg, mi->size);
                break;

            case LR_MIR_SETCC: {
                /* 0F 9x /0 */
                uint8_t x86cc = lr_cc_to_x86(mi->cc);
                if (mi->dst.reg >= 8)
                    emit_byte(buf, &pos, buflen, rex(false, false, false, true));
                emit_byte(buf, &pos, buflen, 0x0F);
                emit_byte(buf, &pos, buflen, (uint8_t)(0x90 + x86cc));
                emit_byte(buf, &pos, buflen, modrm(3, 0, mi->dst.reg));
                break;
            }

            case LR_MIR_CMOVCC: {
                /* 0F 4x /r */
                uint8_t x86cc = lr_cc_to_x86(mi->cc);
                bool need_rex = (mi->size == 8) || (mi->dst.reg >= 8) || (mi->src.reg >= 8);
                if (need_rex)
                    emit_byte(buf, &pos, buflen, rex(mi->size == 8, mi->dst.reg >= 8, false, mi->src.reg >= 8));
                emit_byte(buf, &pos, buflen, 0x0F);
                emit_byte(buf, &pos, buflen, (uint8_t)(0x40 + x86cc));
                emit_byte(buf, &pos, buflen, modrm(3, mi->dst.reg, mi->src.reg));
                break;
            }

            case LR_MIR_JMP: {
                /* jmp rel32 */
                emit_byte(buf, &pos, buflen, 0xE9);
                if (num_fixups < 4096) {
                    fixups[num_fixups].pos = pos;
                    fixups[num_fixups].target = mi->dst.label;
                    num_fixups++;
                }
                emit_u32(buf, &pos, buflen, 0);
                break;
            }

            case LR_MIR_JCC: {
                /* 0F 8x rel32 */
                uint8_t x86cc = lr_cc_to_x86(mi->cc);
                emit_byte(buf, &pos, buflen, 0x0F);
                emit_byte(buf, &pos, buflen, (uint8_t)(0x80 + x86cc));
                if (num_fixups < 4096) {
                    fixups[num_fixups].pos = pos;
                    fixups[num_fixups].target = mi->dst.label;
                    num_fixups++;
                }
                emit_u32(buf, &pos, buflen, 0);
                break;
            }

            case LR_MIR_LEA:
                encode_mem(buf, &pos, buflen, 0x8D, mi->dst.reg,
                           mi->src.mem.base, mi->src.mem.disp, 8);
                break;

            case LR_MIR_CALL: {
                /* call *reg: FF /2 */
                if (mi->src.reg >= 8)
                    emit_byte(buf, &pos, buflen, rex(false, false, false, true));
                emit_byte(buf, &pos, buflen, 0xFF);
                emit_byte(buf, &pos, buflen, modrm(3, 2, mi->src.reg));
                break;
            }

            case LR_MIR_MOVSX: {
                /* movsxd rax, eax: 48 63 C0 */
                emit_byte(buf, &pos, buflen, rex(true, mi->dst.reg >= 8, false, mi->src.reg >= 8));
                emit_byte(buf, &pos, buflen, 0x63);
                emit_byte(buf, &pos, buflen, modrm(3, mi->dst.reg, mi->src.reg));
                break;
            }

            case LR_MIR_MOVZX: {
                /* movzx eax, al: 0F B6 C0 (byte to dword, clears upper bits) */
                bool need_rex = (mi->dst.reg >= 8) || (mi->src.reg >= 8);
                if (need_rex)
                    emit_byte(buf, &pos, buflen, rex(false, mi->dst.reg >= 8, false, mi->src.reg >= 8));
                emit_byte(buf, &pos, buflen, 0x0F);
                emit_byte(buf, &pos, buflen, (mi->size == 1) ? 0xB6 : 0xB7);
                emit_byte(buf, &pos, buflen, modrm(3, mi->dst.reg, mi->src.reg));
                break;
            }

            default:
                break;
            }
        }
    }

    /* Fix up branch targets */
    for (uint32_t i = 0; i < num_fixups; i++) {
        size_t fix_pos = fixups[i].pos;
        uint32_t target = fixups[i].target;
        if (target < block_idx && fix_pos + 4 <= buflen) {
            int32_t rel = (int32_t)((int64_t)block_offsets[target] - (int64_t)(fix_pos + 4));
            buf[fix_pos + 0] = (uint8_t)(rel);
            buf[fix_pos + 1] = (uint8_t)(rel >> 8);
            buf[fix_pos + 2] = (uint8_t)(rel >> 16);
            buf[fix_pos + 3] = (uint8_t)(rel >> 24);
        }
    }

    *out_len = pos;
    return 0;
}

static int x86_64_print_inst(const lr_minst_t *mi, char *buf, size_t len) {
    static const char *reg_names_64[] = {
        "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
        "r8","r9","r10","r11","r12","r13","r14","r15"
    };
    static const char *reg_names_32[] = {
        "eax","ecx","edx","ebx","esp","ebp","esi","edi",
        "r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d"
    };
    const char **rn = (mi->size <= 4) ? reg_names_32 : reg_names_64;

    switch (mi->op) {
    case LR_MIR_RET:     return snprintf(buf, len, "ret");
    case LR_MIR_MOV_IMM: return snprintf(buf, len, "mov %s, %ld", rn[mi->dst.reg], (long)mi->src.imm);
    case LR_MIR_MOV:
        if (mi->src.kind == LR_MOP_MEM)
            return snprintf(buf, len, "mov %s, [%s%+d]", rn[mi->dst.reg], reg_names_64[mi->src.mem.base], mi->src.mem.disp);
        if (mi->dst.kind == LR_MOP_MEM)
            return snprintf(buf, len, "mov [%s%+d], %s", reg_names_64[mi->dst.mem.base], mi->dst.mem.disp, rn[mi->src.reg]);
        return snprintf(buf, len, "mov %s, %s", rn[mi->dst.reg], rn[mi->src.reg]);
    case LR_MIR_ADD:  return snprintf(buf, len, "add %s, %s", rn[mi->dst.reg], rn[mi->src.reg]);
    case LR_MIR_SUB:  return snprintf(buf, len, "sub %s, %s", rn[mi->dst.reg], rn[mi->src.reg]);
    case LR_MIR_IMUL: return snprintf(buf, len, "imul %s, %s", rn[mi->dst.reg], rn[mi->src.reg]);
    case LR_MIR_AND:  return snprintf(buf, len, "and %s, %s", rn[mi->dst.reg], rn[mi->src.reg]);
    case LR_MIR_OR:   return snprintf(buf, len, "or %s, %s", rn[mi->dst.reg], rn[mi->src.reg]);
    case LR_MIR_XOR:  return snprintf(buf, len, "xor %s, %s", rn[mi->dst.reg], rn[mi->src.reg]);
    case LR_MIR_CMP:  return snprintf(buf, len, "cmp %s, %s", rn[mi->dst.reg], rn[mi->src.reg]);
    case LR_MIR_JMP:  return snprintf(buf, len, "jmp .L%u", mi->dst.label);
    case LR_MIR_JCC:  return snprintf(buf, len, "j%u .L%u", mi->cc, mi->dst.label);
    case LR_MIR_LEA:  return snprintf(buf, len, "lea %s, [%s%+d]", reg_names_64[mi->dst.reg], reg_names_64[mi->src.mem.base], mi->src.mem.disp);
    default: return snprintf(buf, len, "<?>");
    }
}

static const lr_target_t x86_64_target = {
    .name = "x86_64",
    .ptr_size = 8,
    .isel_func = x86_64_isel_func,
    .encode_func = x86_64_encode_func,
    .print_inst = x86_64_print_inst,
};

const lr_target_t *lr_target_x86_64(void) {
    return &x86_64_target;
}
