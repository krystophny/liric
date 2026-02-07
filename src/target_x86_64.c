#include "target_x86_64.h"
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/*
 * x86_64 direct-emission backend: stack-based register allocation.
 *
 * All integer computation flows through RAX (primary) and RCX (secondary).
 * FP computation flows through XMM0 (primary) and XMM1 (secondary), both
 * caller-saved per System V ABI, so no save/restore needed.
 * Every IR vreg gets a stack slot addressed via RBP.
 * System V argument registers: RDI, RSI, RDX, RCX, R8, R9 (6 args).
 *
 * ISel and encoding are fused into a single compile pass.
 * A pre-scan allocates all stack slots before emitting the prologue so
 * that the sub rsp,N immediate is known up front.
 */

#define FP_SCRATCH0  X86_XMM0
#define FP_SCRATCH1  X86_XMM1

/* Per-block PHI copy list: copies to emit before the block terminator */
typedef struct phi_copy {
    uint32_t dest_vreg;
    lr_operand_t src_op;
    struct phi_copy *next;
} phi_copy_t;

/* Backend-local compile context replacing the old MIR linked-list state */
typedef struct {
    uint8_t *buf;
    size_t buflen;
    size_t pos;
    uint32_t stack_size;
    int32_t *stack_slots;
    uint32_t num_stack_slots;
    size_t block_offsets[1024];
    struct { size_t pos; uint32_t target; } fixups[4096];
    uint32_t num_fixups;
    lr_arena_t *arena;
} x86_compile_ctx_t;

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

/* Allocate a stack slot for a vreg, return rbp offset (negative) */
static int32_t alloc_slot(x86_compile_ctx_t *ctx, uint32_t vreg, uint8_t size) {
    while (vreg >= ctx->num_stack_slots) {
        uint32_t old = ctx->num_stack_slots;
        uint32_t new_cap = old == 0 ? 64 : old * 2;
        int32_t *ns = lr_arena_array(ctx->arena, int32_t, new_cap);
        if (old > 0) memcpy(ns, ctx->stack_slots, old * sizeof(int32_t));
        for (uint32_t i = old; i < new_cap; i++) ns[i] = 0;
        ctx->stack_slots = ns;
        ctx->num_stack_slots = new_cap;
    }

    if (ctx->stack_slots[vreg] != 0)
        return ctx->stack_slots[vreg];

    if (size < 8) size = 8;
    ctx->stack_size += size;
    ctx->stack_size = (ctx->stack_size + size - 1) & ~(uint32_t)(size - 1);
    int32_t offset = -(int32_t)ctx->stack_size;
    ctx->stack_slots[vreg] = offset;
    return offset;
}

/* ---- Encoding helpers (pure byte-writing, unchanged) ---- */

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

static uint8_t rex(bool w, bool r, bool x, bool b) {
    return (uint8_t)(0x40 | (w ? 8 : 0) | (r ? 4 : 0) | (x ? 2 : 0) | (b ? 1 : 0));
}

static uint8_t modrm(uint8_t mod, uint8_t reg, uint8_t rm) {
    return (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7));
}

static void encode_alu_rr(uint8_t *buf, size_t *pos, size_t len,
                           uint8_t opcode, uint8_t dst, uint8_t src, uint8_t size) {
    bool need_rex = (size == 8) || (dst >= 8) || (src >= 8);
    if (size == 2) emit_byte(buf, pos, len, 0x66);
    if (need_rex)
        emit_byte(buf, pos, len, rex(size == 8, src >= 8, false, dst >= 8));
    emit_byte(buf, pos, len, opcode);
    emit_byte(buf, pos, len, modrm(3, src, dst));
}

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

static void encode_sse_rr(uint8_t *buf, size_t *pos, size_t len,
                           uint8_t prefix, uint8_t op1, uint8_t op2,
                           uint8_t xmm_dst, uint8_t xmm_src) {
    emit_byte(buf, pos, len, prefix);
    emit_byte(buf, pos, len, 0x0F);
    emit_byte(buf, pos, len, op1);
    if (op2 != 0) emit_byte(buf, pos, len, op2);
    emit_byte(buf, pos, len, modrm(3, xmm_dst, xmm_src));
}

static void encode_sse_mem(uint8_t *buf, size_t *pos, size_t len,
                            uint8_t prefix, uint8_t op1, uint8_t op2,
                            uint8_t xmm_reg, uint8_t base, int32_t disp) {
    emit_byte(buf, pos, len, prefix);
    emit_byte(buf, pos, len, 0x0F);
    emit_byte(buf, pos, len, op1);
    if (op2 != 0) emit_byte(buf, pos, len, op2);

    uint8_t mod;
    if (disp == 0 && (base & 7) != 5) mod = 0;
    else if (disp >= -128 && disp <= 127) mod = 1;
    else mod = 2;

    emit_byte(buf, pos, len, modrm(mod, xmm_reg, base));
    if ((base & 7) == 4)
        emit_byte(buf, pos, len, 0x24);
    if (mod == 1)
        emit_byte(buf, pos, len, (uint8_t)(int8_t)disp);
    else if (mod == 2)
        emit_u32(buf, pos, len, (uint32_t)disp);
}

static void emit_setcc_byte(uint8_t *buf, size_t *pos, size_t len,
                             uint8_t x86cc, uint8_t dst_reg) {
    if (dst_reg >= 8)
        emit_byte(buf, pos, len, rex(false, false, false, true));
    emit_byte(buf, pos, len, 0x0F);
    emit_byte(buf, pos, len, (uint8_t)(0x90 + x86cc));
    emit_byte(buf, pos, len, modrm(3, 0, dst_reg));
}

static void emit_fp_setcc(uint8_t *buf, size_t *pos, size_t len,
                           uint8_t fp_cc, uint8_t dst) {
    switch (fp_cc) {
    case LR_CC_FP_OGT:
        emit_setcc_byte(buf, pos, len, X86_CC_A, dst);
        break;
    case LR_CC_FP_OGE:
        emit_setcc_byte(buf, pos, len, X86_CC_AE, dst);
        break;
    case LR_CC_FP_ORD:
        emit_setcc_byte(buf, pos, len, X86_CC_NP, dst);
        break;
    case LR_CC_FP_UNO:
        emit_setcc_byte(buf, pos, len, X86_CC_P, dst);
        break;
    case LR_CC_FP_UEQ:
        emit_setcc_byte(buf, pos, len, X86_CC_E, dst);
        break;
    case LR_CC_FP_ULT:
        emit_setcc_byte(buf, pos, len, X86_CC_B, dst);
        break;
    case LR_CC_FP_ULE:
        emit_setcc_byte(buf, pos, len, X86_CC_BE, dst);
        break;

    case LR_CC_FP_OEQ:
        emit_setcc_byte(buf, pos, len, X86_CC_E, dst);
        emit_setcc_byte(buf, pos, len, X86_CC_NP, X86_RCX);
        encode_alu_rr(buf, pos, len, 0x21, dst, X86_RCX, 1);
        break;
    case LR_CC_FP_ONE:
        emit_setcc_byte(buf, pos, len, X86_CC_NE, dst);
        emit_setcc_byte(buf, pos, len, X86_CC_NP, X86_RCX);
        encode_alu_rr(buf, pos, len, 0x21, dst, X86_RCX, 1);
        break;
    case LR_CC_FP_OLT:
        emit_setcc_byte(buf, pos, len, X86_CC_B, dst);
        emit_setcc_byte(buf, pos, len, X86_CC_NP, X86_RCX);
        encode_alu_rr(buf, pos, len, 0x21, dst, X86_RCX, 1);
        break;
    case LR_CC_FP_OLE:
        emit_setcc_byte(buf, pos, len, X86_CC_BE, dst);
        emit_setcc_byte(buf, pos, len, X86_CC_NP, X86_RCX);
        encode_alu_rr(buf, pos, len, 0x21, dst, X86_RCX, 1);
        break;

    case LR_CC_FP_UNE:
        emit_setcc_byte(buf, pos, len, X86_CC_NE, dst);
        emit_setcc_byte(buf, pos, len, X86_CC_P, X86_RCX);
        encode_alu_rr(buf, pos, len, 0x09, dst, X86_RCX, 1);
        break;
    case LR_CC_FP_UGT:
        emit_setcc_byte(buf, pos, len, X86_CC_A, dst);
        emit_setcc_byte(buf, pos, len, X86_CC_P, X86_RCX);
        encode_alu_rr(buf, pos, len, 0x09, dst, X86_RCX, 1);
        break;
    case LR_CC_FP_UGE:
        emit_setcc_byte(buf, pos, len, X86_CC_AE, dst);
        emit_setcc_byte(buf, pos, len, X86_CC_P, X86_RCX);
        encode_alu_rr(buf, pos, len, 0x09, dst, X86_RCX, 1);
        break;

    default:
        emit_setcc_byte(buf, pos, len, X86_CC_E, dst);
        break;
    }
}

/* ---- Direct-emission ISel helpers ---- */

/* Emit: mov reg, [rbp + offset] (load vreg from stack) */
static void emit_load_slot(x86_compile_ctx_t *ctx, uint32_t vreg, uint8_t reg) {
    int32_t off = alloc_slot(ctx, vreg, 8);
    encode_mem(ctx->buf, &ctx->pos, ctx->buflen, 0x8B, reg, X86_RBP, off, 8);
}

/* Emit: mov [rbp + offset], reg (store reg to vreg stack slot) */
static void emit_store_slot(x86_compile_ctx_t *ctx, uint32_t vreg, uint8_t reg) {
    int32_t off = alloc_slot(ctx, vreg, 8);
    encode_mem(ctx->buf, &ctx->pos, ctx->buflen, 0x89, reg, X86_RBP, off, 8);
}

/* Emit: mov_imm reg, imm64 */
static void emit_mov_imm(x86_compile_ctx_t *ctx, uint8_t dst, int64_t imm) {
    if (imm >= INT32_MIN && imm <= INT32_MAX) {
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, rex(true, false, false, dst >= 8));
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0xC7);
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, 0, dst));
        emit_u32(ctx->buf, &ctx->pos, ctx->buflen, (uint32_t)(int32_t)imm);
    } else {
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, rex(true, false, false, dst >= 8));
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, (uint8_t)(0xB8 + (dst & 7)));
        emit_u64(ctx->buf, &ctx->pos, ctx->buflen, (uint64_t)imm);
    }
}

/* Load an operand value into a GPR */
static void emit_load_operand(x86_compile_ctx_t *ctx,
                               const lr_operand_t *op, uint8_t reg) {
    if (op->kind == LR_VAL_IMM_I64) {
        emit_mov_imm(ctx, reg, op->imm_i64);
    } else if (op->kind == LR_VAL_VREG) {
        emit_load_slot(ctx, op->vreg, reg);
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
        emit_mov_imm(ctx, reg, imm_bits);
    } else if (op->kind == LR_VAL_NULL) {
        emit_mov_imm(ctx, reg, 0);
    }
}

/* FP helpers: load/store FP values between stack slots and XMM regs.
 * Stack slots hold the raw bit representation; SSE2 FP load/store instructions
 * interpret the same bits as float/double. */

static void emit_load_fp_slot(x86_compile_ctx_t *ctx,
                               uint32_t vreg, uint8_t fpreg, uint8_t fsize) {
    int32_t off = alloc_slot(ctx, vreg, 8);
    uint8_t prefix = (fsize == 8) ? 0xF2 : 0xF3;
    encode_sse_mem(ctx->buf, &ctx->pos, ctx->buflen, prefix, 0x10, 0,
                   fpreg, X86_RBP, off);
}

static void emit_store_fp_slot(x86_compile_ctx_t *ctx,
                                uint32_t vreg, uint8_t fpreg, uint8_t fsize) {
    int32_t off = alloc_slot(ctx, vreg, 8);
    uint8_t prefix = (fsize == 8) ? 0xF2 : 0xF3;
    encode_sse_mem(ctx->buf, &ctx->pos, ctx->buflen, prefix, 0x11, 0,
                   fpreg, X86_RBP, off);
}

static void emit_load_fp_operand(x86_compile_ctx_t *ctx,
                                  const lr_operand_t *op, uint8_t fpreg,
                                  uint8_t fsize) {
    if (op->kind == LR_VAL_VREG) {
        emit_load_fp_slot(ctx, op->vreg, fpreg, fsize);
    } else {
        /* Load immediate bits into GPR, then move to XMM */
        emit_load_operand(ctx, op, X86_RAX);
        /* movq xmm, reg: 66 REX.W 0F 6E /r */
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x66);
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, rex(true, fpreg >= 8, false, false));
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x0F);
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x6E);
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, fpreg, X86_RAX));
    }
}

/* Emit prologue: push rbp; mov rbp, rsp; sub rsp, N */
static void emit_prologue(x86_compile_ctx_t *ctx) {
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x55); /* push rbp */
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, rex(true, false, false, false));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x89);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, X86_RSP, X86_RBP)); /* mov rbp, rsp */

    if (ctx->stack_size > 0) {
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, rex(true, false, false, false));
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x81);
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, 5, X86_RSP)); /* sub rsp, imm32 */
        emit_u32(ctx->buf, &ctx->pos, ctx->buflen, ctx->stack_size);
    }
}

/* Emit epilogue: mov rsp, rbp; pop rbp; ret */
static void emit_epilogue(x86_compile_ctx_t *ctx) {
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, rex(true, false, false, false));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x89);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, X86_RBP, X86_RSP)); /* mov rsp, rbp */
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x5D); /* pop rbp */
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0xC3); /* ret */
}

/* Emit PHI copies for the current block as predecessor */
static void emit_phi_copies(x86_compile_ctx_t *ctx, phi_copy_t *copies) {
    for (phi_copy_t *pc = copies; pc; pc = pc->next) {
        emit_load_operand(ctx, &pc->src_op, X86_RAX);
        emit_store_slot(ctx, pc->dest_vreg, X86_RAX);
    }
}

/* Inline encoding helpers for specific MIR-equivalent patterns */

static void emit_imul_rr(x86_compile_ctx_t *ctx, uint8_t dst, uint8_t src, uint8_t size) {
    bool need_rex = (size == 8) || (dst >= 8) || (src >= 8);
    if (need_rex)
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen,
                  rex(size == 8, dst >= 8, false, src >= 8));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x0F);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0xAF);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, dst, src));
}

static void emit_idiv_r(x86_compile_ctx_t *ctx, uint8_t src, uint8_t size) {
    bool need_rex = (size == 8) || (src >= 8);
    if (need_rex)
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen,
                  rex(size == 8, false, false, src >= 8));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0xF7);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, 7, src));
}

static void emit_shift(x86_compile_ctx_t *ctx, uint8_t ext, uint8_t dst, uint8_t size) {
    bool need_rex = (size == 8) || (dst >= 8);
    if (need_rex)
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen,
                  rex(size == 8, false, false, dst >= 8));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0xD3);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, ext, dst));
}

static void emit_setcc(x86_compile_ctx_t *ctx, uint8_t cc, uint8_t dst) {
    if (cc >= LR_CC_FP_OEQ) {
        emit_fp_setcc(ctx->buf, &ctx->pos, ctx->buflen, cc, dst);
    } else {
        uint8_t x86cc = lr_cc_to_x86(cc);
        emit_setcc_byte(ctx->buf, &ctx->pos, ctx->buflen, x86cc, dst);
    }
}

static void emit_movzx_rr(x86_compile_ctx_t *ctx, uint8_t dst, uint8_t src, uint8_t size) {
    bool need_rex = (dst >= 8) || (src >= 8);
    if (need_rex)
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen,
                  rex(false, dst >= 8, false, src >= 8));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x0F);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, (size == 1) ? 0xB6 : 0xB7);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, dst, src));
}

/* Emit a movzx mem load for sub-dword sizes: movzx reg, byte/word [base+disp] */
static void emit_movzx_mem(x86_compile_ctx_t *ctx, uint8_t dst, uint8_t base,
                            int32_t disp, uint8_t size) {
    uint8_t opcode2 = (size == 1) ? 0xB6 : 0xB7;
    bool need_rex = (dst >= 8) || (base >= 8);
    if (need_rex)
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen,
                  rex(true, dst >= 8, false, base >= 8));
    else
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, rex(true, false, false, false));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x0F);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, opcode2);

    uint8_t mod;
    if (disp == 0 && (base & 7) != 5) mod = 0;
    else if (disp >= -128 && disp <= 127) mod = 1;
    else mod = 2;
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(mod, dst, base));
    if ((base & 7) == 4) emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x24);
    if (mod == 1) emit_byte(ctx->buf, &ctx->pos, ctx->buflen, (uint8_t)(int8_t)disp);
    else if (mod == 2) emit_u32(ctx->buf, &ctx->pos, ctx->buflen, (uint32_t)disp);
}

static void emit_jmp(x86_compile_ctx_t *ctx, uint32_t target_block) {
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0xE9);
    if (ctx->num_fixups < 4096) {
        ctx->fixups[ctx->num_fixups].pos = ctx->pos;
        ctx->fixups[ctx->num_fixups].target = target_block;
        ctx->num_fixups++;
    }
    emit_u32(ctx->buf, &ctx->pos, ctx->buflen, 0);
}

static void emit_jcc(x86_compile_ctx_t *ctx, uint8_t cc, uint32_t target_block) {
    uint8_t x86cc = lr_cc_to_x86(cc);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x0F);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, (uint8_t)(0x80 + x86cc));
    if (ctx->num_fixups < 4096) {
        ctx->fixups[ctx->num_fixups].pos = ctx->pos;
        ctx->fixups[ctx->num_fixups].target = target_block;
        ctx->num_fixups++;
    }
    emit_u32(ctx->buf, &ctx->pos, ctx->buflen, 0);
}

static void emit_call_r10(x86_compile_ctx_t *ctx) {
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, rex(false, false, false, true));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0xFF);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, 2, X86_R10));
}

static void emit_frame_alloc(x86_compile_ctx_t *ctx, uint32_t bytes) {
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, rex(true, false, false, false));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x81);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, 5, X86_RSP));
    emit_u32(ctx->buf, &ctx->pos, ctx->buflen, bytes);
}

static void emit_frame_free(x86_compile_ctx_t *ctx, uint32_t bytes) {
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, rex(true, false, false, false));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x81);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, 0, X86_RSP));
    emit_u32(ctx->buf, &ctx->pos, ctx->buflen, bytes);
}

/* SSE2 FP arithmetic helpers */

static void emit_sse_arith(x86_compile_ctx_t *ctx, uint8_t op1,
                            uint8_t dst, uint8_t src, uint8_t fsize) {
    uint8_t prefix = (fsize == 8) ? 0xF2 : 0xF3;
    encode_sse_rr(ctx->buf, &ctx->pos, ctx->buflen, prefix, op1, 0, dst, src);
}

static void emit_fcmp(x86_compile_ctx_t *ctx, uint8_t dst, uint8_t src, uint8_t fsize) {
    if (fsize == 8) {
        encode_sse_rr(ctx->buf, &ctx->pos, ctx->buflen, 0x66, 0x2E, 0, dst, src);
    } else {
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x0F);
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x2E);
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, dst, src));
    }
}

static void emit_cvtsi2fp(x86_compile_ctx_t *ctx, uint8_t fpreg, uint8_t gpr, uint8_t fsize) {
    uint8_t prefix = (fsize == 8) ? 0xF2 : 0xF3;
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, prefix);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen,
              rex(true, fpreg >= 8, false, gpr >= 8));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x0F);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x2A);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, fpreg, gpr));
}

static void emit_cvtfp2si(x86_compile_ctx_t *ctx, uint8_t gpr, uint8_t fpreg, uint8_t fsize) {
    uint8_t prefix = (fsize == 8) ? 0xF2 : 0xF3;
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, prefix);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen,
              rex(true, gpr >= 8, false, fpreg >= 8));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x0F);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x2C);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, gpr, fpreg));
}

static void emit_movsxd(x86_compile_ctx_t *ctx, uint8_t dst, uint8_t src) {
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, rex(true, dst >= 8, false, src >= 8));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x63);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, dst, src));
}

static void emit_cmovcc(x86_compile_ctx_t *ctx, uint8_t cc, uint8_t dst,
                          uint8_t src, uint8_t size) {
    uint8_t x86cc = lr_cc_to_x86(cc);
    bool need_rex = (size == 8) || (dst >= 8) || (src >= 8);
    if (need_rex)
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen,
                  rex(size == 8, dst >= 8, false, src >= 8));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x0F);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, (uint8_t)(0x40 + x86cc));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, dst, src));
}

/*
 * Pre-scan: walk all instructions to allocate stack slots for every vreg
 * destination and handle static allocas. This must run before the prologue
 * so that stack_size is known.
 */
static void prescan_slots(x86_compile_ctx_t *ctx, lr_func_t *func) {
    for (lr_block_t *b = func->first_block; b; b = b->next) {
        for (lr_inst_t *inst = b->first; inst; inst = inst->next) {
            switch (inst->op) {
            case LR_OP_ALLOCA: {
                size_t elem_sz = lr_type_size(inst->type);
                if (elem_sz < 8) elem_sz = 8;
                bool use_static = (inst->num_operands == 0);
                if (inst->num_operands > 0 && inst->operands[0].kind == LR_VAL_IMM_I64 &&
                    inst->operands[0].imm_i64 == 1) {
                    use_static = true;
                }
                if (use_static) {
                    ctx->stack_size += (uint32_t)elem_sz;
                    ctx->stack_size = (ctx->stack_size + 7) & ~7u;
                }
                alloc_slot(ctx, inst->dest, 8);
                break;
            }
            case LR_OP_PHI:
                alloc_slot(ctx, inst->dest, 8);
                break;
            default:
                if (inst->op != LR_OP_STORE && inst->op != LR_OP_BR &&
                    inst->op != LR_OP_CONDBR && inst->op != LR_OP_RET &&
                    inst->op != LR_OP_RET_VOID && inst->op != LR_OP_UNREACHABLE) {
                    if (inst->type && inst->type->kind != LR_TYPE_VOID)
                        alloc_slot(ctx, inst->dest, 8);
                }
                break;
            }
        }
    }
    /* Also ensure parameter vregs are allocated */
    for (uint32_t i = 0; i < func->num_params; i++)
        alloc_slot(ctx, func->param_vregs[i], 8);
}

/*
 * Build per-block PHI copy lists.
 * For each PHI instruction: %dest = phi [val0, %bb0], [val1, %bb1], ...
 * Add a copy {dest_vreg, src_op} to block bb0's list, bb1's list, etc.
 */
static phi_copy_t **build_phi_copies(x86_compile_ctx_t *ctx, lr_func_t *func) {
    phi_copy_t **copies = lr_arena_array(ctx->arena, phi_copy_t *, func->num_blocks);
    for (uint32_t i = 0; i < func->num_blocks; i++)
        copies[i] = NULL;

    for (lr_block_t *b = func->first_block; b; b = b->next) {
        for (lr_inst_t *inst = b->first; inst; inst = inst->next) {
            if (inst->op != LR_OP_PHI) continue;
            for (uint32_t i = 0; i + 1 < inst->num_operands; i += 2) {
                uint32_t pred_id = inst->operands[i + 1].block_id;
                if (pred_id >= func->num_blocks) continue;
                phi_copy_t *pc = lr_arena_new(ctx->arena, phi_copy_t);
                pc->dest_vreg = inst->dest;
                pc->src_op = inst->operands[i];
                pc->next = copies[pred_id];
                copies[pred_id] = pc;
            }
        }
    }
    return copies;
}

/*
 * x86_64_compile_func: single-pass ISel + encoding.
 * Replaces the old two-phase isel_func + encode_func approach.
 */
static int x86_64_compile_func(lr_func_t *func, lr_module_t *mod,
                                uint8_t *buf, size_t buflen, size_t *out_len,
                                lr_arena_t *arena) {
    (void)mod;

    x86_compile_ctx_t ctx = {
        .buf = buf,
        .buflen = buflen,
        .pos = 0,
        .stack_size = 0,
        .stack_slots = NULL,
        .num_stack_slots = 0,
        .num_fixups = 0,
        .arena = arena,
    };

    /* Pre-scan to allocate all slots and compute static alloca sizes */
    prescan_slots(&ctx, func);

    /* Align stack to 16 bytes */
    ctx.stack_size = (ctx.stack_size + 15) & ~15u;

    /* Build PHI copy lists */
    phi_copy_t **phi_copies = build_phi_copies(&ctx, func);

    /* Emit prologue */
    emit_prologue(&ctx);

    /* Store parameters: first 6 from registers, rest from caller frame */
    static const uint8_t param_regs[] = { X86_RDI, X86_RSI, X86_RDX, X86_RCX, X86_R8, X86_R9 };
    for (uint32_t i = 0; i < func->num_params && i < 6; i++)
        emit_store_slot(&ctx, func->param_vregs[i], param_regs[i]);
    for (uint32_t i = 6; i < func->num_params; i++) {
        int32_t caller_off = 16 + (int32_t)(i - 6) * 8;
        encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x8B, X86_RAX, X86_RBP, caller_off, 8);
        emit_store_slot(&ctx, func->param_vregs[i], X86_RAX);
    }

    /* Walk IR blocks and instructions, emitting code directly */
    uint32_t bi = 0;
    for (lr_block_t *b = func->first_block; b; b = b->next, bi++) {
        ctx.block_offsets[bi] = ctx.pos;

        for (lr_inst_t *inst = b->first; inst; inst = inst->next) {
            switch (inst->op) {
            case LR_OP_RET: {
                emit_phi_copies(&ctx, phi_copies[bi]);
                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                emit_epilogue(&ctx);
                break;
            }
            case LR_OP_RET_VOID: {
                emit_phi_copies(&ctx, phi_copies[bi]);
                emit_epilogue(&ctx);
                break;
            }
            case LR_OP_ADD: case LR_OP_SUB: case LR_OP_AND:
            case LR_OP_OR: case LR_OP_XOR: {
                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                emit_load_operand(&ctx, &inst->operands[1], X86_RCX);
                uint8_t opcode;
                switch (inst->op) {
                case LR_OP_ADD: opcode = 0x01; break;
                case LR_OP_SUB: opcode = 0x29; break;
                case LR_OP_AND: opcode = 0x21; break;
                case LR_OP_OR:  opcode = 0x09; break;
                case LR_OP_XOR: opcode = 0x31; break;
                default: opcode = 0x01; break;
                }
                encode_alu_rr(ctx.buf, &ctx.pos, ctx.buflen, opcode,
                              X86_RAX, X86_RCX, (uint8_t)lr_type_size(inst->type));
                emit_store_slot(&ctx, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_MUL: {
                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                emit_load_operand(&ctx, &inst->operands[1], X86_RCX);
                emit_imul_rr(&ctx, X86_RAX, X86_RCX, (uint8_t)lr_type_size(inst->type));
                emit_store_slot(&ctx, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_FADD: case LR_OP_FSUB:
            case LR_OP_FMUL: case LR_OP_FDIV: {
                uint8_t fsize = (inst->type && inst->type->kind == LR_TYPE_FLOAT) ? 4 : 8;
                emit_load_fp_operand(&ctx, &inst->operands[0], FP_SCRATCH0, fsize);
                emit_load_fp_operand(&ctx, &inst->operands[1], FP_SCRATCH1, fsize);
                uint8_t op1;
                switch (inst->op) {
                case LR_OP_FADD: op1 = 0x58; break;
                case LR_OP_FSUB: op1 = 0x5C; break;
                case LR_OP_FMUL: op1 = 0x59; break;
                case LR_OP_FDIV: op1 = 0x5E; break;
                default: op1 = 0x58; break;
                }
                emit_sse_arith(&ctx, op1, FP_SCRATCH0, FP_SCRATCH1, fsize);
                emit_store_fp_slot(&ctx, inst->dest, FP_SCRATCH0, fsize);
                break;
            }
            case LR_OP_FNEG: {
                uint8_t fsize = (inst->type && inst->type->kind == LR_TYPE_FLOAT) ? 4 : 8;
                emit_load_fp_operand(&ctx, &inst->operands[0], FP_SCRATCH1, fsize);
                /* xorpd dst, dst (zero): 66 0F 57 /r */
                encode_sse_rr(ctx.buf, &ctx.pos, ctx.buflen, 0x66, 0x57, 0,
                              FP_SCRATCH0, FP_SCRATCH0);
                /* subsd/subss dst, src */
                uint8_t prefix = (fsize == 8) ? 0xF2 : 0xF3;
                encode_sse_rr(ctx.buf, &ctx.pos, ctx.buflen, prefix, 0x5C, 0,
                              FP_SCRATCH0, FP_SCRATCH1);
                emit_store_fp_slot(&ctx, inst->dest, FP_SCRATCH0, fsize);
                break;
            }
            case LR_OP_SDIV: case LR_OP_SREM: {
                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                emit_load_operand(&ctx, &inst->operands[1], X86_RCX);
                uint8_t sz = (uint8_t)lr_type_size(inst->type);
                if (sz <= 4) {
                    /* cdq */
                    emit_byte(ctx.buf, &ctx.pos, ctx.buflen, 0x99);
                } else {
                    /* cqo: REX.W 0x99 */
                    emit_byte(ctx.buf, &ctx.pos, ctx.buflen, rex(true, false, false, false));
                    emit_byte(ctx.buf, &ctx.pos, ctx.buflen, 0x99);
                }
                emit_idiv_r(&ctx, X86_RCX, sz);
                uint8_t res_reg = (inst->op == LR_OP_SREM) ? X86_RDX : X86_RAX;
                emit_store_slot(&ctx, inst->dest, res_reg);
                break;
            }
            case LR_OP_SHL: case LR_OP_LSHR: case LR_OP_ASHR: {
                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                emit_load_operand(&ctx, &inst->operands[1], X86_RCX);
                uint8_t ext;
                switch (inst->op) {
                case LR_OP_SHL:  ext = 4; break;
                case LR_OP_LSHR: ext = 5; break;
                case LR_OP_ASHR: ext = 7; break;
                default: ext = 4; break;
                }
                emit_shift(&ctx, ext, X86_RAX, (uint8_t)lr_type_size(inst->type));
                emit_store_slot(&ctx, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_ICMP: {
                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                emit_load_operand(&ctx, &inst->operands[1], X86_RCX);
                encode_alu_rr(ctx.buf, &ctx.pos, ctx.buflen, 0x39,
                              X86_RAX, X86_RCX, (uint8_t)lr_type_size(inst->operands[0].type));

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

                emit_setcc(&ctx, cc, X86_RAX);
                emit_movzx_rr(&ctx, X86_RAX, X86_RAX, 1);
                emit_store_slot(&ctx, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_SELECT: {
                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                encode_alu_rr(ctx.buf, &ctx.pos, ctx.buflen, 0x85,
                              X86_RAX, X86_RAX, 1);
                emit_load_operand(&ctx, &inst->operands[2], X86_RAX);
                emit_load_operand(&ctx, &inst->operands[1], X86_RCX);
                emit_cmovcc(&ctx, LR_CC_NE, X86_RAX, X86_RCX, 8);
                emit_store_slot(&ctx, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_BR: {
                emit_phi_copies(&ctx, phi_copies[bi]);
                uint32_t target_id = inst->operands[0].block_id;
                emit_jmp(&ctx, target_id);
                break;
            }
            case LR_OP_CONDBR: {
                emit_phi_copies(&ctx, phi_copies[bi]);
                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                encode_alu_rr(ctx.buf, &ctx.pos, ctx.buflen, 0x85,
                              X86_RAX, X86_RAX, 1);
                uint32_t true_id = inst->operands[1].block_id;
                uint32_t false_id = inst->operands[2].block_id;
                emit_jcc(&ctx, LR_CC_NE, true_id);
                emit_jmp(&ctx, false_id);
                break;
            }
            case LR_OP_ALLOCA: {
                size_t elem_sz = lr_type_size(inst->type);
                if (elem_sz < 8) elem_sz = 8;

                bool use_static = (inst->num_operands == 0);
                if (inst->num_operands > 0 && inst->operands[0].kind == LR_VAL_IMM_I64 &&
                    inst->operands[0].imm_i64 == 1) {
                    use_static = true;
                }

                if (use_static) {
                    /* Re-walk prescan in order to find the RBP offset for this
                     * alloca's data region. We must replicate the exact same
                     * stack_size bumps that prescan_slots performed, including
                     * the alloc_slot calls for dest vregs, so that the alloca
                     * data offsets land at the right positions. */
                    uint32_t sim_ss = 0;
                    int32_t off = 0;
                    for (lr_block_t *sb = func->first_block; sb; sb = sb->next) {
                        for (lr_inst_t *si = sb->first; si; si = si->next) {
                            if (si->op != LR_OP_ALLOCA) continue;
                            size_t esz = lr_type_size(si->type);
                            if (esz < 8) esz = 8;
                            bool si_static = (si->num_operands == 0);
                            if (si->num_operands > 0 && si->operands[0].kind == LR_VAL_IMM_I64 &&
                                si->operands[0].imm_i64 == 1) {
                                si_static = true;
                            }
                            if (!si_static) continue;
                            sim_ss += (uint32_t)esz;
                            sim_ss = (sim_ss + 7) & ~7u;
                            if (si == inst) {
                                off = -(int32_t)sim_ss;
                                goto found_alloca;
                            }
                            /* Simulate the alloc_slot bump for dest vreg */
                            sim_ss += 8;
                            sim_ss = (sim_ss + 7) & ~7u;
                        }
                    }
                    found_alloca:;
                    /* lea rax, [rbp + off] */
                    encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x8D, X86_RAX, X86_RBP, off, 8);
                    emit_store_slot(&ctx, inst->dest, X86_RAX);
                } else {
                    /* Dynamic alloca */
                    emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                    if (elem_sz != 1) {
                        emit_mov_imm(&ctx, X86_RCX, (int64_t)elem_sz);
                        emit_imul_rr(&ctx, X86_RAX, X86_RCX, 8);
                    }
                    /* Align to 16: rax = (rax + 15) & ~15 */
                    emit_mov_imm(&ctx, X86_RCX, 15);
                    encode_alu_rr(ctx.buf, &ctx.pos, ctx.buflen, 0x01, X86_RAX, X86_RCX, 8);
                    emit_mov_imm(&ctx, X86_RCX, ~15LL);
                    encode_alu_rr(ctx.buf, &ctx.pos, ctx.buflen, 0x21, X86_RAX, X86_RCX, 8);
                    /* sub rsp, rax */
                    encode_alu_rr(ctx.buf, &ctx.pos, ctx.buflen, 0x29, X86_RSP, X86_RAX, 8);
                    /* mov rax, rsp */
                    encode_alu_rr(ctx.buf, &ctx.pos, ctx.buflen, 0x89, X86_RAX, X86_RSP, 8);
                    emit_store_slot(&ctx, inst->dest, X86_RAX);
                }
                break;
            }
            case LR_OP_LOAD: {
                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                uint8_t sz = (uint8_t)lr_type_size(inst->type);
                if (sz < 4) {
                    emit_movzx_mem(&ctx, X86_RAX, X86_RAX, 0, sz);
                } else {
                    encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x8B, X86_RAX, X86_RAX, 0, sz);
                }
                emit_store_slot(&ctx, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_STORE: {
                emit_load_operand(&ctx, &inst->operands[1], X86_RCX);
                size_t store_sz = lr_type_size(inst->operands[0].type);

                if (store_sz > 8 &&
                    inst->operands[0].kind == LR_VAL_IMM_I64 &&
                    inst->operands[0].imm_i64 == 0) {
                    emit_mov_imm(&ctx, X86_RAX, 0);
                    size_t rem = store_sz;
                    int32_t off = 0;
                    while (rem >= 8) {
                        encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x89, X86_RAX, X86_RCX, off, 8);
                        rem -= 8; off += 8;
                    }
                    if (rem >= 4) {
                        encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x89, X86_RAX, X86_RCX, off, 4);
                        rem -= 4; off += 4;
                    }
                    if (rem >= 2) {
                        encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x89, X86_RAX, X86_RCX, off, 2);
                        rem -= 2; off += 2;
                    }
                    if (rem == 1) {
                        encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x88, X86_RAX, X86_RCX, off, 1);
                    }
                    break;
                }

                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                uint8_t opcode = ((uint8_t)store_sz == 1) ? 0x88 : 0x89;
                encode_mem(ctx.buf, &ctx.pos, ctx.buflen, opcode, X86_RAX, X86_RCX, 0, (uint8_t)store_sz);
                break;
            }
            case LR_OP_GEP: {
                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
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
                            emit_load_operand(&ctx, idx_op, X86_RCX);
                            if (elem_size != 1) {
                                emit_mov_imm(&ctx, X86_R10, (int64_t)elem_size);
                                emit_imul_rr(&ctx, X86_RCX, X86_R10, 8);
                            }
                            encode_alu_rr(ctx.buf, &ctx.pos, ctx.buflen, 0x01, X86_RAX, X86_RCX, 8);
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
                            emit_load_operand(&ctx, idx_op, X86_RCX);
                            if (elem_size != 1) {
                                emit_mov_imm(&ctx, X86_R10, (int64_t)elem_size);
                                emit_imul_rr(&ctx, X86_RCX, X86_R10, 8);
                            }
                            encode_alu_rr(ctx.buf, &ctx.pos, ctx.buflen, 0x01, X86_RAX, X86_RCX, 8);
                        }
                        cur_ty = cur_ty->array.elem;
                    }
                    if (is_const && byte_off != 0) {
                        emit_mov_imm(&ctx, X86_RCX, byte_off);
                        encode_alu_rr(ctx.buf, &ctx.pos, ctx.buflen, 0x01, X86_RAX, X86_RCX, 8);
                    }
                }
                emit_store_slot(&ctx, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_SEXT: {
                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                emit_movsxd(&ctx, X86_RAX, X86_RAX);
                emit_store_slot(&ctx, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_ZEXT: case LR_OP_TRUNC: case LR_OP_BITCAST:
            case LR_OP_PTRTOINT: case LR_OP_INTTOPTR: {
                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                emit_store_slot(&ctx, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_FCMP: {
                uint8_t fsize = (inst->operands[0].type &&
                                 inst->operands[0].type->kind == LR_TYPE_FLOAT) ? 4 : 8;
                emit_load_fp_operand(&ctx, &inst->operands[0], FP_SCRATCH0, fsize);
                emit_load_fp_operand(&ctx, &inst->operands[1], FP_SCRATCH1, fsize);
                emit_fcmp(&ctx, FP_SCRATCH0, FP_SCRATCH1, fsize);

                uint8_t cc;
                switch (inst->fcmp_pred) {
                case LR_FCMP_OEQ: cc = LR_CC_FP_OEQ; break;
                case LR_FCMP_ONE: cc = LR_CC_FP_ONE; break;
                case LR_FCMP_OGT: cc = LR_CC_FP_OGT; break;
                case LR_FCMP_OGE: cc = LR_CC_FP_OGE; break;
                case LR_FCMP_OLT: cc = LR_CC_FP_OLT; break;
                case LR_FCMP_OLE: cc = LR_CC_FP_OLE; break;
                case LR_FCMP_ORD: cc = LR_CC_FP_ORD; break;
                case LR_FCMP_UNO: cc = LR_CC_FP_UNO; break;
                case LR_FCMP_UEQ: cc = LR_CC_FP_UEQ; break;
                case LR_FCMP_UNE: cc = LR_CC_FP_UNE; break;
                case LR_FCMP_UGT: cc = LR_CC_FP_UGT; break;
                case LR_FCMP_UGE: cc = LR_CC_FP_UGE; break;
                case LR_FCMP_ULT: cc = LR_CC_FP_ULT; break;
                case LR_FCMP_ULE: cc = LR_CC_FP_ULE; break;
                default:          cc = LR_CC_FP_OEQ; break;
                }

                emit_setcc(&ctx, cc, X86_RAX);
                emit_movzx_rr(&ctx, X86_RAX, X86_RAX, 1);
                emit_store_slot(&ctx, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_SITOFP: {
                uint8_t fsize = (inst->type && inst->type->kind == LR_TYPE_FLOAT) ? 4 : 8;
                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                emit_cvtsi2fp(&ctx, FP_SCRATCH0, X86_RAX, fsize);
                emit_store_fp_slot(&ctx, inst->dest, FP_SCRATCH0, fsize);
                break;
            }
            case LR_OP_FPTOSI: {
                uint8_t fsize = (inst->operands[0].type &&
                                 inst->operands[0].type->kind == LR_TYPE_FLOAT) ? 4 : 8;
                emit_load_fp_operand(&ctx, &inst->operands[0], FP_SCRATCH0, fsize);
                emit_cvtfp2si(&ctx, X86_RAX, FP_SCRATCH0, fsize);
                emit_store_slot(&ctx, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_FPEXT: {
                emit_load_fp_operand(&ctx, &inst->operands[0], FP_SCRATCH0, 4);
                /* cvtss2sd: F3 0F 5A /r */
                encode_sse_rr(ctx.buf, &ctx.pos, ctx.buflen, 0xF3, 0x5A, 0,
                              FP_SCRATCH0, FP_SCRATCH0);
                emit_store_fp_slot(&ctx, inst->dest, FP_SCRATCH0, 8);
                break;
            }
            case LR_OP_FPTRUNC: {
                emit_load_fp_operand(&ctx, &inst->operands[0], FP_SCRATCH0, 8);
                /* cvtsd2ss: F2 0F 5A /r */
                encode_sse_rr(ctx.buf, &ctx.pos, ctx.buflen, 0xF2, 0x5A, 0,
                              FP_SCRATCH0, FP_SCRATCH0);
                emit_store_fp_slot(&ctx, inst->dest, FP_SCRATCH0, 4);
                break;
            }
            case LR_OP_EXTRACTVALUE: {
                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                emit_store_slot(&ctx, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_INSERTVALUE: {
                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                emit_store_slot(&ctx, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_CALL: {
                static const uint8_t call_regs[] = { X86_RDI, X86_RSI, X86_RDX, X86_RCX, X86_R8, X86_R9 };
                uint32_t nargs = inst->num_operands - 1;
                uint32_t nstack = nargs > 6 ? nargs - 6 : 0;
                uint32_t stack_bytes = ((nstack * 8 + 15) & ~15u);

                if (stack_bytes > 0)
                    emit_frame_alloc(&ctx, stack_bytes);

                /* Store stack args in forward order to [RSP + offset] */
                for (uint32_t i = 0; i < nstack; i++) {
                    uint32_t arg_idx = 6 + i;
                    emit_load_operand(&ctx, &inst->operands[arg_idx + 1], X86_RAX);
                    encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x89, X86_RAX,
                               X86_RSP, (int32_t)(i * 8), 8);
                }

                /* Place first 6 args in System V registers */
                for (uint32_t i = 0; i < nargs && i < 6; i++)
                    emit_load_operand(&ctx, &inst->operands[i + 1], call_regs[i]);

                /* Clear %al for variadic call convention */
                emit_mov_imm(&ctx, X86_RAX, 0);

                /* Load callee into r10 and call */
                emit_load_operand(&ctx, &inst->operands[0], X86_R10);
                emit_call_r10(&ctx);

                if (stack_bytes > 0)
                    emit_frame_free(&ctx, stack_bytes);

                if (inst->type && inst->type->kind != LR_TYPE_VOID)
                    emit_store_slot(&ctx, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_PHI:
                /* Handled via phi_copies; slot already allocated in prescan */
                break;
            case LR_OP_UNREACHABLE:
                break;
            default:
                break;
            }
        }
    }

    /* Fix up branch targets */
    for (uint32_t i = 0; i < ctx.num_fixups; i++) {
        size_t fix_pos = ctx.fixups[i].pos;
        uint32_t target = ctx.fixups[i].target;
        if (target < bi && fix_pos + 4 <= ctx.buflen) {
            int32_t rel = (int32_t)((int64_t)ctx.block_offsets[target] - (int64_t)(fix_pos + 4));
            buf[fix_pos + 0] = (uint8_t)(rel);
            buf[fix_pos + 1] = (uint8_t)(rel >> 8);
            buf[fix_pos + 2] = (uint8_t)(rel >> 16);
            buf[fix_pos + 3] = (uint8_t)(rel >> 24);
        }
    }

    *out_len = ctx.pos;
    if (ctx.pos > buflen)
        return -1;
    return 0;
}

static const lr_target_t x86_64_target = {
    .name = "x86_64",
    .ptr_size = 8,
    .compile_func = x86_64_compile_func,
};

const lr_target_t *lr_target_x86_64(void) {
    return &x86_64_target;
}
