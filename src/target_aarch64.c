#include "target_aarch64.h"
#include "objfile.h"
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * aarch64 direct-emission backend: stack-based register allocation.
 *
 * All integer computation flows through X9 (primary) and X10 (secondary).
 * FP computation flows through D0 (primary) and D1 (secondary), both
 * caller-saved per AAPCS64, so no save/restore needed.
 * Every IR vreg gets a stack slot addressed via FP (X29).
 * AAPCS64 argument registers: X0-X7 (8 args).
 *
 * ISel and encoding are fused into a single compile pass.
 * A pre-scan allocates all stack slots before emitting the prologue so
 * that the sub sp,N immediate is known up front.
 */

#define FP_SCRATCH0  A64_D0
#define FP_SCRATCH1  A64_D1

typedef struct phi_copy {
    uint32_t dest_vreg;
    lr_operand_t src_op;
    struct phi_copy *next;
} phi_copy_t;

typedef struct {
    uint8_t *buf;
    size_t buflen;
    size_t pos;
    uint32_t stack_size;
    int32_t *stack_slots;
    uint32_t num_stack_slots;
    size_t block_offsets[1024];
    struct { size_t insn_pos; uint32_t target; uint8_t kind; uint8_t cond; } fixups[4096];
    uint32_t num_fixups;
    lr_arena_t *arena;
    lr_objfile_ctx_t *obj_ctx;
    lr_module_t *mod;
} a64_compile_ctx_t;

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

static int32_t alloc_slot(a64_compile_ctx_t *ctx, uint32_t vreg, uint8_t size) {
    if (vreg > 100000) return -8;
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

static uint32_t enc_lslv(bool is64, uint8_t rd, uint8_t rn, uint8_t rm) {
    return (is64 ? 0x9AC02000u : 0x1AC02000u) | ((uint32_t)rm << 16)
         | ((uint32_t)rn << 5) | rd;
}

static uint32_t enc_lsrv(bool is64, uint8_t rd, uint8_t rn, uint8_t rm) {
    return (is64 ? 0x9AC02400u : 0x1AC02400u) | ((uint32_t)rm << 16)
         | ((uint32_t)rn << 5) | rd;
}

static uint32_t enc_asrv(bool is64, uint8_t rd, uint8_t rn, uint8_t rm) {
    return (is64 ? 0x9AC02800u : 0x1AC02800u) | ((uint32_t)rm << 16)
         | ((uint32_t)rn << 5) | rd;
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

static uint32_t enc_fp_ldur(uint8_t fsize, uint8_t ft, uint8_t rn, int32_t imm9) {
    uint32_t base = (fsize == 4) ? 0xBC400000u : 0xFC400000u;
    return base | ((uint32_t)(imm9 & 0x1FF) << 12) | ((uint32_t)rn << 5) | ft;
}

static uint32_t enc_fp_stur(uint8_t fsize, uint8_t ft, uint8_t rn, int32_t imm9) {
    uint32_t base = (fsize == 4) ? 0xBC000000u : 0xFC000000u;
    return base | ((uint32_t)(imm9 & 0x1FF) << 12) | ((uint32_t)rn << 5) | ft;
}

static void emit_fp_load(uint8_t *buf, size_t *pos, size_t len, uint8_t ft,
                         uint8_t rn, int32_t disp, uint8_t fsize) {
    if (disp >= -256 && disp <= 255) {
        emit_u32(buf, pos, len, enc_fp_ldur(fsize, ft, rn, disp));
        return;
    }
    emit_addr(buf, pos, len, A64_X15, rn, disp);
    emit_u32(buf, pos, len, enc_fp_ldur(fsize, ft, A64_X15, 0));
}

static void emit_fp_store(uint8_t *buf, size_t *pos, size_t len, uint8_t ft,
                          uint8_t rn, int32_t disp, uint8_t fsize) {
    if (disp >= -256 && disp <= 255) {
        emit_u32(buf, pos, len, enc_fp_stur(fsize, ft, rn, disp));
        return;
    }
    emit_addr(buf, pos, len, A64_X15, rn, disp);
    emit_u32(buf, pos, len, enc_fp_stur(fsize, ft, A64_X15, 0));
}

static uint32_t enc_fadd(uint8_t fsize, uint8_t rd, uint8_t rn, uint8_t rm) {
    uint32_t base = (fsize == 8) ? 0x1E602800u : 0x1E202800u;
    return base | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rd;
}

static uint32_t enc_fsub(uint8_t fsize, uint8_t rd, uint8_t rn, uint8_t rm) {
    uint32_t base = (fsize == 8) ? 0x1E603800u : 0x1E203800u;
    return base | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rd;
}

static uint32_t enc_fmul(uint8_t fsize, uint8_t rd, uint8_t rn, uint8_t rm) {
    uint32_t base = (fsize == 8) ? 0x1E600800u : 0x1E200800u;
    return base | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rd;
}

static uint32_t enc_fdiv(uint8_t fsize, uint8_t rd, uint8_t rn, uint8_t rm) {
    uint32_t base = (fsize == 8) ? 0x1E601800u : 0x1E201800u;
    return base | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rd;
}

static uint32_t enc_fneg(uint8_t fsize, uint8_t rd, uint8_t rn) {
    uint32_t base = (fsize == 8) ? 0x1E614000u : 0x1E214000u;
    return base | ((uint32_t)rn << 5) | rd;
}

static uint32_t enc_fcmp(uint8_t fsize, uint8_t rn, uint8_t rm) {
    uint32_t base = (fsize == 8) ? 0x1E602000u : 0x1E202000u;
    return base | ((uint32_t)rm << 16) | ((uint32_t)rn << 5);
}

static uint32_t enc_scvtf(uint8_t fsize, uint8_t fd, uint8_t xn) {
    uint32_t base = (fsize == 8) ? 0x9E620000u : 0x9E220000u;
    return base | ((uint32_t)xn << 5) | fd;
}

static uint32_t enc_fcvtzs(uint8_t fsize, uint8_t xd, uint8_t fn) {
    uint32_t base = (fsize == 8) ? 0x9E780000u : 0x9E380000u;
    return base | ((uint32_t)fn << 5) | xd;
}

static uint32_t enc_fcvt_f32_to_f64(uint8_t dd, uint8_t sn) {
    return 0x1E22C000u | ((uint32_t)sn << 5) | dd;
}

static uint32_t enc_fcvt_f64_to_f32(uint8_t sd, uint8_t dn) {
    return 0x1E624000u | ((uint32_t)dn << 5) | sd;
}

static uint32_t enc_fmov_from_gpr(uint8_t fsize, uint8_t fd, uint8_t xn) {
    uint32_t base = (fsize == 8) ? 0x9E670000u : 0x1E270000u;
    return base | ((uint32_t)xn << 5) | fd;
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

static uint8_t lr_fp_cc_to_a64(uint8_t cc) {
    switch (cc) {
    case LR_CC_FP_OEQ: return 0;  /* EQ: ordered equal */
    case LR_CC_FP_OGT: return 12; /* GT: ordered greater */
    case LR_CC_FP_OGE: return 10; /* GE: ordered greater or equal */
    case LR_CC_FP_OLT: return 4;  /* MI: N=1 (only set for ordered less) */
    case LR_CC_FP_OLE: return 9;  /* LS: C=0 or Z=1 (less or equal) */
    case LR_CC_FP_ORD: return 7;  /* VC: V=0 (not unordered) */
    case LR_CC_FP_UNO: return 6;  /* VS: V=1 (unordered) */
    case LR_CC_FP_UNE: return 1;  /* NE: not equal (includes unordered) */
    case LR_CC_FP_UGT: return 8;  /* HI: C=1 and Z=0 (greater or unordered) */
    case LR_CC_FP_UGE: return 2;  /* HS: C=1 (>= or unordered) */
    case LR_CC_FP_ULT: return 11; /* LT: N!=V (less or unordered) */
    case LR_CC_FP_ULE: return 13; /* LE: Z=1 or N!=V (<=, unordered) */
    default:           return 0;
    }
}

/* ---- Direct-emission ISel helpers ---- */

static void emit_load_slot(a64_compile_ctx_t *ctx, uint32_t vreg, uint8_t reg) {
    int32_t off = alloc_slot(ctx, vreg, 8);
    emit_load(ctx->buf, &ctx->pos, ctx->buflen, reg, A64_FP, off, 8);
}

static void emit_store_slot(a64_compile_ctx_t *ctx, uint32_t vreg, uint8_t reg) {
    int32_t off = alloc_slot(ctx, vreg, 8);
    emit_store(ctx->buf, &ctx->pos, ctx->buflen, reg, A64_FP, off, 8);
}

static bool is_symbol_defined_in_module(lr_module_t *mod, const char *name) {
    for (lr_func_t *f = mod->first_func; f; f = f->next) {
        if (f->first_block && strcmp(f->name, name) == 0)
            return true;
    }
    for (lr_global_t *g = mod->first_global; g; g = g->next) {
        if (!g->is_external && g->name && strcmp(g->name, name) == 0)
            return true;
    }
    return false;
}

static void emit_load_operand(a64_compile_ctx_t *ctx,
                               const lr_operand_t *op, uint8_t reg) {
    if (op->kind == LR_VAL_IMM_I64) {
        emit_move_imm(ctx->buf, &ctx->pos, ctx->buflen, reg, op->imm_i64, true);
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
        emit_move_imm(ctx->buf, &ctx->pos, ctx->buflen, reg, imm_bits, true);
    } else if (op->kind == LR_VAL_NULL || op->kind == LR_VAL_UNDEF) {
        emit_move_imm(ctx->buf, &ctx->pos, ctx->buflen, reg, 0, true);
    } else if (op->kind == LR_VAL_GLOBAL && ctx->obj_ctx) {
        const char *sym_name = lr_module_symbol_name(ctx->mod,
                                                      op->global_id);
        if (!sym_name) {
            emit_move_imm(ctx->buf, &ctx->pos, ctx->buflen, reg, 0, true);
            return;
        }
        bool defined = is_symbol_defined_in_module(ctx->mod, sym_name);
        uint32_t sym_idx = lr_obj_ensure_symbol(ctx->obj_ctx, sym_name,
                                                 false, 0, 0);
        size_t adrp_off = ctx->pos;
        emit_u32(ctx->buf, &ctx->pos, ctx->buflen,
                 0x90000000u | (uint32_t)reg);
        if (defined) {
            lr_obj_add_reloc(ctx->obj_ctx, (uint32_t)adrp_off, sym_idx,
                              LR_RELOC_ARM64_PAGE21);
            size_t add_off = ctx->pos;
            emit_u32(ctx->buf, &ctx->pos, ctx->buflen,
                     0x91000000u | ((uint32_t)reg << 5) | (uint32_t)reg);
            lr_obj_add_reloc(ctx->obj_ctx, (uint32_t)add_off, sym_idx,
                              LR_RELOC_ARM64_PAGEOFF12);
        } else {
            lr_obj_add_reloc(ctx->obj_ctx, (uint32_t)adrp_off, sym_idx,
                              LR_RELOC_ARM64_GOT_LOAD_PAGE21);
            size_t ldr_off = ctx->pos;
            /* LDR Xreg, [Xreg, #0] — load pointer from GOT entry */
            emit_u32(ctx->buf, &ctx->pos, ctx->buflen,
                     0xF9400000u | ((uint32_t)reg << 5) | (uint32_t)reg);
            lr_obj_add_reloc(ctx->obj_ctx, (uint32_t)ldr_off, sym_idx,
                              LR_RELOC_ARM64_GOT_LOAD_PAGEOFF12);
        }
    }
}

static void emit_load_fp_slot(a64_compile_ctx_t *ctx,
                               uint32_t vreg, uint8_t fpreg, uint8_t fsize) {
    int32_t off = alloc_slot(ctx, vreg, 8);
    emit_fp_load(ctx->buf, &ctx->pos, ctx->buflen, fpreg, A64_FP, off, fsize);
}

static void emit_store_fp_slot(a64_compile_ctx_t *ctx,
                                uint32_t vreg, uint8_t fpreg, uint8_t fsize) {
    int32_t off = alloc_slot(ctx, vreg, 8);
    emit_fp_store(ctx->buf, &ctx->pos, ctx->buflen, fpreg, A64_FP, off, fsize);
}

static void emit_load_fp_operand(a64_compile_ctx_t *ctx,
                                  const lr_operand_t *op, uint8_t fpreg,
                                  uint8_t fsize) {
    if (op->kind == LR_VAL_VREG) {
        emit_load_fp_slot(ctx, op->vreg, fpreg, fsize);
    } else {
        emit_load_operand(ctx, op, A64_X9);
        emit_u32(ctx->buf, &ctx->pos, ctx->buflen,
                 enc_fmov_from_gpr(fsize, fpreg, A64_X9));
    }
}

static void emit_setcc_a64(a64_compile_ctx_t *ctx, uint8_t cc, uint8_t dst) {
    if (cc >= LR_CC_FP_OEQ) {
        if (cc == LR_CC_FP_ONE) {
            emit_move_imm(ctx->buf, &ctx->pos, ctx->buflen, dst, 0, false);
            emit_move_imm(ctx->buf, &ctx->pos, ctx->buflen, A64_X15, 1, false);
            emit_u32(ctx->buf, &ctx->pos, ctx->buflen,
                     enc_csel(false, dst, A64_X15, dst, 4));  /* MI */
            emit_u32(ctx->buf, &ctx->pos, ctx->buflen,
                     enc_csel(false, dst, A64_X15, dst, 12)); /* GT */
        } else if (cc == LR_CC_FP_UEQ) {
            emit_move_imm(ctx->buf, &ctx->pos, ctx->buflen, dst, 0, false);
            emit_move_imm(ctx->buf, &ctx->pos, ctx->buflen, A64_X15, 1, false);
            emit_u32(ctx->buf, &ctx->pos, ctx->buflen,
                     enc_csel(false, dst, A64_X15, dst, 0));  /* EQ */
            emit_u32(ctx->buf, &ctx->pos, ctx->buflen,
                     enc_csel(false, dst, A64_X15, dst, 6));  /* VS */
        } else {
            uint8_t cond = lr_fp_cc_to_a64(cc);
            emit_move_imm(ctx->buf, &ctx->pos, ctx->buflen, dst, 1, false);
            emit_u32(ctx->buf, &ctx->pos, ctx->buflen,
                     enc_csel(false, dst, dst, A64_SP, cond));
        }
    } else {
        uint8_t cond = lr_cc_to_a64(cc);
        emit_move_imm(ctx->buf, &ctx->pos, ctx->buflen, dst, 1, false);
        emit_u32(ctx->buf, &ctx->pos, ctx->buflen,
                 enc_csel(false, dst, dst, A64_SP, cond));
    }
}

static void emit_epilogue_a64(a64_compile_ctx_t *ctx) {
    if (ctx->stack_size > 0)
        emit_sp_adjust(ctx->buf, &ctx->pos, ctx->buflen, ctx->stack_size, false);
    emit_u32(ctx->buf, &ctx->pos, ctx->buflen, 0xA8C17BFDu); /* ldp x29, x30, [sp], #16 */
    emit_u32(ctx->buf, &ctx->pos, ctx->buflen, 0xD65F03C0u); /* ret */
}

static void emit_jmp_a64(a64_compile_ctx_t *ctx, uint32_t target_block) {
    if (ctx->num_fixups < 4096) {
        ctx->fixups[ctx->num_fixups].insn_pos = ctx->pos;
        ctx->fixups[ctx->num_fixups].target = target_block;
        ctx->fixups[ctx->num_fixups].kind = 0;
        ctx->fixups[ctx->num_fixups].cond = 0;
        ctx->num_fixups++;
    }
    emit_u32(ctx->buf, &ctx->pos, ctx->buflen, 0x14000000u);
}

static void emit_jcc_a64(a64_compile_ctx_t *ctx, uint8_t cc, uint32_t target_block) {
    uint8_t cond = lr_cc_to_a64(cc);
    if (ctx->num_fixups < 4096) {
        ctx->fixups[ctx->num_fixups].insn_pos = ctx->pos;
        ctx->fixups[ctx->num_fixups].target = target_block;
        ctx->fixups[ctx->num_fixups].kind = 1;
        ctx->fixups[ctx->num_fixups].cond = cond;
        ctx->num_fixups++;
    }
    emit_u32(ctx->buf, &ctx->pos, ctx->buflen, 0x54000000u);
}

static void emit_phi_copies(a64_compile_ctx_t *ctx, phi_copy_t *copies) {
    for (phi_copy_t *pc = copies; pc; pc = pc->next) {
        emit_load_operand(ctx, &pc->src_op, A64_X9);
        emit_store_slot(ctx, pc->dest_vreg, A64_X9);
    }
}

/*
 * Pre-scan: walk all instructions to allocate stack slots for every vreg
 * destination and handle static allocas. This must run before the prologue
 * so that stack_size is known.
 */
static void prescan_slots(a64_compile_ctx_t *ctx, lr_func_t *func) {
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
    for (uint32_t i = 0; i < func->num_params; i++)
        alloc_slot(ctx, func->param_vregs[i], 8);
}

/*
 * Build per-block PHI copy lists.
 * For each PHI instruction: %dest = phi [val0, %bb0], [val1, %bb1], ...
 * Add a copy {dest_vreg, src_op} to block bb0's list, bb1's list, etc.
 */
static phi_copy_t **build_phi_copies(a64_compile_ctx_t *ctx, lr_func_t *func) {
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
 * aarch64_compile_func: single-pass ISel + encoding.
 * Replaces the old two-phase isel_func + encode_func approach.
 */
static int aarch64_compile_func(lr_func_t *func, lr_module_t *mod,
                                 uint8_t *buf, size_t buflen, size_t *out_len,
                                 lr_arena_t *arena) {
    a64_compile_ctx_t ctx = {
        .buf = buf,
        .buflen = buflen,
        .pos = 0,
        .stack_size = 0,
        .stack_slots = NULL,
        .num_stack_slots = 0,
        .num_fixups = 0,
        .arena = arena,
        .obj_ctx = mod ? (lr_objfile_ctx_t *)mod->obj_ctx : NULL,
        .mod = mod,
    };

    prescan_slots(&ctx, func);

    ctx.stack_size = (ctx.stack_size + 15) & ~15u;

    phi_copy_t **phi_copies = build_phi_copies(&ctx, func);

    /* Prologue: stp x29, x30, [sp, #-16]!; mov x29, sp; sub sp, sp, N */
    emit_u32(ctx.buf, &ctx.pos, ctx.buflen, 0xA9BF7BFDu); /* stp x29, x30, [sp, #-16]! */
    emit_u32(ctx.buf, &ctx.pos, ctx.buflen, 0x910003FDu); /* mov x29, sp */
    if (ctx.stack_size > 0)
        emit_sp_adjust(ctx.buf, &ctx.pos, ctx.buflen, ctx.stack_size, true);

    /* Store parameters: first 8 from registers, rest from caller frame */
    static const uint8_t param_regs[] = {
        A64_X0, A64_X1, A64_X2, A64_X3, A64_X4, A64_X5, A64_X6, A64_X7
    };
    for (uint32_t i = 0; i < func->num_params && i < 8; i++)
        emit_store_slot(&ctx, func->param_vregs[i], param_regs[i]);
    for (uint32_t i = 8; i < func->num_params; i++) {
        int32_t caller_off = 16 + (int32_t)(i - 8) * 8;
        emit_load(ctx.buf, &ctx.pos, ctx.buflen, A64_X9, A64_FP, caller_off, 8);
        emit_store_slot(&ctx, func->param_vregs[i], A64_X9);
    }

    /* Walk IR blocks and instructions, emitting code directly */
    uint32_t bi = 0;
    for (lr_block_t *b = func->first_block; b; b = b->next, bi++) {
        ctx.block_offsets[bi] = ctx.pos;

        for (lr_inst_t *inst = b->first; inst; inst = inst->next) {
            switch (inst->op) {
            case LR_OP_RET: {
                emit_phi_copies(&ctx, phi_copies[bi]);
                emit_load_operand(&ctx, &inst->operands[0], A64_X9);
                emit_mov_reg(ctx.buf, &ctx.pos, ctx.buflen, A64_X0, A64_X9, true);
                emit_epilogue_a64(&ctx);
                break;
            }
            case LR_OP_RET_VOID: {
                emit_phi_copies(&ctx, phi_copies[bi]);
                emit_epilogue_a64(&ctx);
                break;
            }
            case LR_OP_ADD: case LR_OP_SUB: case LR_OP_AND:
            case LR_OP_OR: case LR_OP_XOR: {
                emit_load_operand(&ctx, &inst->operands[0], A64_X9);
                emit_load_operand(&ctx, &inst->operands[1], A64_X10);
                bool is64 = lr_type_size(inst->type) > 4;
                switch (inst->op) {
                case LR_OP_ADD:
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             enc_add_reg(is64, A64_X9, A64_X9, A64_X10));
                    break;
                case LR_OP_SUB:
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             enc_sub_reg(is64, A64_X9, A64_X9, A64_X10));
                    break;
                case LR_OP_AND:
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             enc_logic_reg(0x8A000000u, is64, A64_X9, A64_X9, A64_X10));
                    break;
                case LR_OP_OR:
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             enc_logic_reg(0xAA000000u, is64, A64_X9, A64_X9, A64_X10));
                    break;
                case LR_OP_XOR:
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             enc_logic_reg(0xCA000000u, is64, A64_X9, A64_X9, A64_X10));
                    break;
                default: break;
                }
                emit_store_slot(&ctx, inst->dest, A64_X9);
                break;
            }
            case LR_OP_MUL: {
                emit_load_operand(&ctx, &inst->operands[0], A64_X9);
                emit_load_operand(&ctx, &inst->operands[1], A64_X10);
                bool is64 = lr_type_size(inst->type) > 4;
                emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                         enc_mul(is64, A64_X9, A64_X9, A64_X10));
                emit_store_slot(&ctx, inst->dest, A64_X9);
                break;
            }
            case LR_OP_FADD: case LR_OP_FSUB:
            case LR_OP_FMUL: case LR_OP_FDIV: {
                uint8_t fsize = (inst->type && inst->type->kind == LR_TYPE_FLOAT) ? 4 : 8;
                emit_load_fp_operand(&ctx, &inst->operands[0], FP_SCRATCH0, fsize);
                emit_load_fp_operand(&ctx, &inst->operands[1], FP_SCRATCH1, fsize);
                switch (inst->op) {
                case LR_OP_FADD:
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             enc_fadd(fsize, FP_SCRATCH0, FP_SCRATCH0, FP_SCRATCH1));
                    break;
                case LR_OP_FSUB:
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             enc_fsub(fsize, FP_SCRATCH0, FP_SCRATCH0, FP_SCRATCH1));
                    break;
                case LR_OP_FMUL:
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             enc_fmul(fsize, FP_SCRATCH0, FP_SCRATCH0, FP_SCRATCH1));
                    break;
                case LR_OP_FDIV:
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             enc_fdiv(fsize, FP_SCRATCH0, FP_SCRATCH0, FP_SCRATCH1));
                    break;
                default: break;
                }
                emit_store_fp_slot(&ctx, inst->dest, FP_SCRATCH0, fsize);
                break;
            }
            case LR_OP_FNEG: {
                uint8_t fsize = (inst->type && inst->type->kind == LR_TYPE_FLOAT) ? 4 : 8;
                emit_load_fp_operand(&ctx, &inst->operands[0], FP_SCRATCH0, fsize);
                emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                         enc_fneg(fsize, FP_SCRATCH0, FP_SCRATCH0));
                emit_store_fp_slot(&ctx, inst->dest, FP_SCRATCH0, fsize);
                break;
            }
            case LR_OP_SDIV: case LR_OP_SREM: {
                emit_load_operand(&ctx, &inst->operands[0], A64_X9);
                emit_load_operand(&ctx, &inst->operands[1], A64_X10);
                bool is64 = lr_type_size(inst->type) > 4;
                emit_mov_reg(ctx.buf, &ctx.pos, ctx.buflen, A64_X11, A64_X9, is64);
                emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                         enc_sdiv(is64, A64_X9, A64_X9, A64_X10));
                emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                         enc_msub(is64, A64_X11, A64_X9, A64_X10, A64_X11));
                if (inst->op == LR_OP_SREM)
                    emit_store_slot(&ctx, inst->dest, A64_X11);
                else
                    emit_store_slot(&ctx, inst->dest, A64_X9);
                break;
            }
            case LR_OP_SHL: case LR_OP_LSHR: case LR_OP_ASHR: {
                emit_load_operand(&ctx, &inst->operands[0], A64_X9);
                emit_load_operand(&ctx, &inst->operands[1], A64_X10);
                bool is64 = lr_type_size(inst->type) > 4;
                switch (inst->op) {
                case LR_OP_SHL:
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             enc_lslv(is64, A64_X9, A64_X9, A64_X10));
                    break;
                case LR_OP_LSHR:
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             enc_lsrv(is64, A64_X9, A64_X9, A64_X10));
                    break;
                case LR_OP_ASHR:
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             enc_asrv(is64, A64_X9, A64_X9, A64_X10));
                    break;
                default: break;
                }
                emit_store_slot(&ctx, inst->dest, A64_X9);
                break;
            }
            case LR_OP_ICMP: {
                emit_load_operand(&ctx, &inst->operands[0], A64_X9);
                emit_load_operand(&ctx, &inst->operands[1], A64_X10);
                bool is64 = lr_type_size(inst->operands[0].type) > 4;
                emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                         enc_subs_reg(is64, A64_X9, A64_X10));

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

                emit_setcc_a64(&ctx, cc, A64_X9);
                emit_store_slot(&ctx, inst->dest, A64_X9);
                break;
            }
            case LR_OP_SELECT: {
                emit_load_operand(&ctx, &inst->operands[0], A64_X9);
                emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                         enc_ands_reg(false, A64_X9, A64_X9));
                emit_load_operand(&ctx, &inst->operands[2], A64_X9);  /* false value */
                emit_load_operand(&ctx, &inst->operands[1], A64_X10); /* true value */
                uint8_t cond = lr_cc_to_a64(LR_CC_NE);
                emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                         enc_csel(true, A64_X9, A64_X10, A64_X9, cond));
                emit_store_slot(&ctx, inst->dest, A64_X9);
                break;
            }
            case LR_OP_BR: {
                emit_phi_copies(&ctx, phi_copies[bi]);
                emit_jmp_a64(&ctx, inst->operands[0].block_id);
                break;
            }
            case LR_OP_CONDBR: {
                emit_phi_copies(&ctx, phi_copies[bi]);
                emit_load_operand(&ctx, &inst->operands[0], A64_X9);
                emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                         enc_ands_reg(false, A64_X9, A64_X9));
                emit_jcc_a64(&ctx, LR_CC_NE, inst->operands[1].block_id);
                emit_jmp_a64(&ctx, inst->operands[2].block_id);
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
                    /* Re-walk prescan in order to find the FP offset for this
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
                            if (si->num_operands > 0 &&
                                si->operands[0].kind == LR_VAL_IMM_I64 &&
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
                    emit_addr(ctx.buf, &ctx.pos, ctx.buflen, A64_X9, A64_FP, off);
                    emit_store_slot(&ctx, inst->dest, A64_X9);
                } else {
                    emit_load_operand(&ctx, &inst->operands[0], A64_X9);
                    if (elem_sz != 1) {
                        emit_move_imm(ctx.buf, &ctx.pos, ctx.buflen,
                                      A64_X10, (int64_t)elem_sz, true);
                        emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                                 enc_mul(true, A64_X9, A64_X9, A64_X10));
                    }
                    /* Align to 16: X9 = (X9 + 15) & ~15 */
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             enc_add_imm(true, A64_X9, A64_X9, 15));
                    emit_move_imm(ctx.buf, &ctx.pos, ctx.buflen, A64_X10, ~15LL, true);
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             enc_logic_reg(0x8A000000u, true, A64_X9, A64_X9, A64_X10));
                    /* sub sp, sp, X9 */
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             enc_sub_reg(true, A64_SP, A64_SP, A64_X9));
                    /* mov X9, sp */
                    emit_mov_reg(ctx.buf, &ctx.pos, ctx.buflen, A64_X9, A64_SP, true);
                    emit_store_slot(&ctx, inst->dest, A64_X9);
                }
                break;
            }
            case LR_OP_LOAD: {
                emit_load_operand(&ctx, &inst->operands[0], A64_X9);
                uint8_t sz = (uint8_t)lr_type_size(inst->type);
                emit_load(ctx.buf, &ctx.pos, ctx.buflen, A64_X9, A64_X9, 0, sz);
                emit_store_slot(&ctx, inst->dest, A64_X9);
                break;
            }
            case LR_OP_STORE: {
                emit_load_operand(&ctx, &inst->operands[0], A64_X9);
                emit_load_operand(&ctx, &inst->operands[1], A64_X10);
                uint8_t sz = (uint8_t)lr_type_size(inst->operands[0].type);
                emit_store(ctx.buf, &ctx.pos, ctx.buflen, A64_X9, A64_X10, 0, sz);
                break;
            }
            case LR_OP_GEP: {
                emit_load_operand(&ctx, &inst->operands[0], A64_X9);
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
                            emit_load_operand(&ctx, idx_op, A64_X10);
                            if (idx_op->type && idx_op->type->kind == LR_TYPE_I32) {
                                emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                                         0x93407C00u |
                                         ((uint32_t)A64_X10 << 5) | A64_X10); /* sxtw */
                            }
                            if (elem_size != 1) {
                                emit_move_imm(ctx.buf, &ctx.pos, ctx.buflen,
                                              A64_X11, (int64_t)elem_size, true);
                                emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                                         enc_mul(true, A64_X10, A64_X10, A64_X11));
                            }
                            emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                                     enc_add_reg(true, A64_X9, A64_X9, A64_X10));
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
                            emit_load_operand(&ctx, idx_op, A64_X10);
                            if (idx_op->type && idx_op->type->kind == LR_TYPE_I32) {
                                emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                                         0x93407C00u |
                                         ((uint32_t)A64_X10 << 5) | A64_X10); /* sxtw */
                            }
                            if (elem_size != 1) {
                                emit_move_imm(ctx.buf, &ctx.pos, ctx.buflen,
                                              A64_X11, (int64_t)elem_size, true);
                                emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                                         enc_mul(true, A64_X10, A64_X10, A64_X11));
                            }
                            emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                                     enc_add_reg(true, A64_X9, A64_X9, A64_X10));
                        }
                        cur_ty = cur_ty->array.elem;
                    }
                    if (is_const && byte_off != 0) {
                        emit_move_imm(ctx.buf, &ctx.pos, ctx.buflen,
                                      A64_X10, byte_off, true);
                        emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                                 enc_add_reg(true, A64_X9, A64_X9, A64_X10));
                    }
                }
                emit_store_slot(&ctx, inst->dest, A64_X9);
                break;
            }
            case LR_OP_SEXT: {
                emit_load_operand(&ctx, &inst->operands[0], A64_X9);
                bool is64 = lr_type_size(inst->type) > 4;
                if (is64)
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             0x93407C00u | ((uint32_t)A64_X9 << 5) | A64_X9); /* sxtw */
                emit_store_slot(&ctx, inst->dest, A64_X9);
                break;
            }
            case LR_OP_ZEXT: case LR_OP_TRUNC: case LR_OP_BITCAST:
            case LR_OP_PTRTOINT: case LR_OP_INTTOPTR: {
                emit_load_operand(&ctx, &inst->operands[0], A64_X9);
                emit_store_slot(&ctx, inst->dest, A64_X9);
                break;
            }
            case LR_OP_FCMP: {
                uint8_t fsize = (inst->operands[0].type &&
                                 inst->operands[0].type->kind == LR_TYPE_FLOAT) ? 4 : 8;
                emit_load_fp_operand(&ctx, &inst->operands[0], FP_SCRATCH0, fsize);
                emit_load_fp_operand(&ctx, &inst->operands[1], FP_SCRATCH1, fsize);
                emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                         enc_fcmp(fsize, FP_SCRATCH0, FP_SCRATCH1));

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

                emit_setcc_a64(&ctx, cc, A64_X9);
                emit_store_slot(&ctx, inst->dest, A64_X9);
                break;
            }
            case LR_OP_SITOFP: {
                uint8_t fsize = (inst->type && inst->type->kind == LR_TYPE_FLOAT) ? 4 : 8;
                emit_load_operand(&ctx, &inst->operands[0], A64_X9);
                emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                         enc_scvtf(fsize, FP_SCRATCH0, A64_X9));
                emit_store_fp_slot(&ctx, inst->dest, FP_SCRATCH0, fsize);
                break;
            }
            case LR_OP_FPTOSI: {
                uint8_t fsize = (inst->operands[0].type &&
                                 inst->operands[0].type->kind == LR_TYPE_FLOAT) ? 4 : 8;
                emit_load_fp_operand(&ctx, &inst->operands[0], FP_SCRATCH0, fsize);
                emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                         enc_fcvtzs(fsize, A64_X9, FP_SCRATCH0));
                emit_store_slot(&ctx, inst->dest, A64_X9);
                break;
            }
            case LR_OP_FPEXT: {
                emit_load_fp_operand(&ctx, &inst->operands[0], FP_SCRATCH0, 4);
                emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                         enc_fcvt_f32_to_f64(FP_SCRATCH0, FP_SCRATCH0));
                emit_store_fp_slot(&ctx, inst->dest, FP_SCRATCH0, 8);
                break;
            }
            case LR_OP_FPTRUNC: {
                emit_load_fp_operand(&ctx, &inst->operands[0], FP_SCRATCH0, 8);
                emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                         enc_fcvt_f64_to_f32(FP_SCRATCH0, FP_SCRATCH0));
                emit_store_fp_slot(&ctx, inst->dest, FP_SCRATCH0, 4);
                break;
            }
            case LR_OP_EXTRACTVALUE: {
                emit_load_operand(&ctx, &inst->operands[0], A64_X9);
                emit_store_slot(&ctx, inst->dest, A64_X9);
                break;
            }
            case LR_OP_INSERTVALUE: {
                emit_load_operand(&ctx, &inst->operands[0], A64_X9);
                emit_store_slot(&ctx, inst->dest, A64_X9);
                break;
            }
            case LR_OP_CALL: {
                static const uint8_t call_regs[] = {
                    A64_X0, A64_X1, A64_X2, A64_X3,
                    A64_X4, A64_X5, A64_X6, A64_X7
                };
                uint32_t nargs = inst->num_operands - 1;
                uint32_t nstack = nargs > 8 ? nargs - 8 : 0;
                uint32_t stack_bytes = ((nstack * 8 + 15) & ~15u);

                if (stack_bytes > 0)
                    emit_sp_adjust(ctx.buf, &ctx.pos, ctx.buflen, stack_bytes, true);

                for (uint32_t i = 0; i < nstack; i++) {
                    emit_load_operand(&ctx, &inst->operands[8 + i + 1], A64_X9);
                    emit_store(ctx.buf, &ctx.pos, ctx.buflen,
                               A64_X9, A64_SP, (int32_t)(i * 8), 8);
                }

                for (uint32_t i = 0; i < nargs && i < 8; i++)
                    emit_load_operand(&ctx, &inst->operands[i + 1], call_regs[i]);

                if (ctx.obj_ctx &&
                    inst->operands[0].kind == LR_VAL_GLOBAL) {
                    const char *sym_name = lr_module_symbol_name(
                        ctx.mod, inst->operands[0].global_id);
                    if (sym_name) {
                        uint32_t sym_idx = lr_obj_ensure_symbol(
                            ctx.obj_ctx, sym_name, false, 0, 0);
                        size_t bl_off = ctx.pos;
                        emit_u32(ctx.buf, &ctx.pos, ctx.buflen, 0x94000000u);
                        lr_obj_add_reloc(ctx.obj_ctx, (uint32_t)bl_off,
                                          sym_idx, LR_RELOC_ARM64_BRANCH26);
                    } else {
                        emit_u32(ctx.buf, &ctx.pos, ctx.buflen, 0xD503201Fu);
                    }
                } else {
                    emit_load_operand(&ctx, &inst->operands[0], A64_X16);
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             0xD63F0000u | ((uint32_t)A64_X16 << 5));
                }

                if (stack_bytes > 0)
                    emit_sp_adjust(ctx.buf, &ctx.pos, ctx.buflen, stack_bytes, false);

                if (inst->type && inst->type->kind != LR_TYPE_VOID)
                    emit_store_slot(&ctx, inst->dest, A64_X0);
                break;
            }
            case LR_OP_PHI:
                break;
            case LR_OP_UNREACHABLE:
                break;
            default:
                break;
            }
        }
    }

    /* Empty function body: emit implicit return */
    if (!func->first_block)
        emit_epilogue_a64(&ctx);

    /* Fix up branch targets */
    for (uint32_t i = 0; i < ctx.num_fixups; i++) {
        if (ctx.fixups[i].target >= bi) continue;

        int64_t target_pos = (int64_t)ctx.block_offsets[ctx.fixups[i].target];
        int64_t here = (int64_t)ctx.fixups[i].insn_pos;
        int64_t imm = (target_pos - here) / 4;

        if (ctx.fixups[i].kind == 0) {
            if (imm >= -(1LL << 25) && imm < (1LL << 25)) {
                uint32_t insn = 0x14000000u | ((uint32_t)imm & 0x03FFFFFFu);
                patch_u32(buf, buflen, ctx.fixups[i].insn_pos, insn);
            }
        } else {
            if (imm >= -(1LL << 18) && imm < (1LL << 18)) {
                uint32_t insn = 0x54000000u
                              | (((uint32_t)imm & 0x7FFFFu) << 5)
                              | (ctx.fixups[i].cond & 0xF);
                patch_u32(buf, buflen, ctx.fixups[i].insn_pos, insn);
            }
        }
    }

    *out_len = ctx.pos;
    if (ctx.pos > buflen)
        return -1;
    return 0;
}

static const lr_target_t aarch64_target = {
    .name = "aarch64",
    .ptr_size = 8,
    .compile_func = aarch64_compile_func,
};

const lr_target_t *lr_target_aarch64(void) {
    return &aarch64_target;
}
