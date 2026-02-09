#include "target_aarch64.h"
#include "target_common.h"
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
 * Stack slots are allocated lazily while emitting instructions; the prologue
 * stack adjustment is patched after emission when final frame size is known.
 */

#define FP_SCRATCH0  A64_D0
#define FP_SCRATCH1  A64_D1

typedef struct { size_t insn_pos; uint32_t target; uint8_t kind; uint8_t cond; } a64_fixup_t;

typedef struct {
    uint8_t *buf;
    size_t buflen;
    size_t pos;
    uint32_t stack_size;
    int32_t *stack_slots;
    uint32_t *stack_slot_sizes;
    uint32_t num_stack_slots;
    int32_t *static_alloca_offsets;
    uint32_t num_static_alloca_offsets;
    size_t *block_offsets;
    uint32_t num_block_offsets;
    a64_fixup_t *fixups;
    uint32_t num_fixups;
    uint32_t fixup_cap;
    lr_arena_t *arena;
    lr_objfile_ctx_t *obj_ctx;
    lr_module_t *mod;
    uint8_t *sym_defined;
    uint32_t sym_count;
} a64_compile_ctx_t;

static void set_static_alloca_offset(a64_compile_ctx_t *ctx, uint32_t vreg,
                                     int32_t offset) {
    while (vreg >= ctx->num_static_alloca_offsets) {
        uint32_t old = ctx->num_static_alloca_offsets;
        uint32_t new_cap = old == 0 ? 64 : old * 2;
        int32_t *no = lr_arena_array_uninit(ctx->arena, int32_t, new_cap);
        if (old > 0)
            memcpy(no, ctx->static_alloca_offsets, old * sizeof(int32_t));
        for (uint32_t i = old; i < new_cap; i++)
            no[i] = 0;
        ctx->static_alloca_offsets = no;
        ctx->num_static_alloca_offsets = new_cap;
    }
    ctx->static_alloca_offsets[vreg] = offset;
}

static int32_t alloc_slot(a64_compile_ctx_t *ctx, uint32_t vreg, size_t size) {
    if (vreg > 100000) return -8;
    while (vreg >= ctx->num_stack_slots) {
        uint32_t old = ctx->num_stack_slots;
        uint32_t new_cap = old == 0 ? 64 : old * 2;
        int32_t *ns = lr_arena_array_uninit(ctx->arena, int32_t, new_cap);
        uint32_t *ss = lr_arena_array_uninit(ctx->arena, uint32_t, new_cap);
        if (old > 0) memcpy(ns, ctx->stack_slots, old * sizeof(int32_t));
        if (old > 0) memcpy(ss, ctx->stack_slot_sizes, old * sizeof(uint32_t));
        for (uint32_t i = old; i < new_cap; i++) ns[i] = 0;
        for (uint32_t i = old; i < new_cap; i++) ss[i] = 0;
        ctx->stack_slots = ns;
        ctx->stack_slot_sizes = ss;
        ctx->num_stack_slots = new_cap;
    }

    if (ctx->stack_slots[vreg] != 0)
        return ctx->stack_slots[vreg];

    if (size < 8) size = 8;
    ctx->stack_size += (uint32_t)size;
    ctx->stack_size = (ctx->stack_size + (uint32_t)size - 1) & ~(uint32_t)(size - 1);
    int32_t offset = -(int32_t)ctx->stack_size;
    ctx->stack_slots[vreg] = offset;
    ctx->stack_slot_sizes[vreg] = (uint32_t)size;
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

static void attach_obj_symbol_defined_cache(a64_compile_ctx_t *ctx) {
    if (!ctx || !ctx->obj_ctx)
        return;
    ctx->sym_defined = ctx->obj_ctx->module_sym_defined;
    ctx->sym_count = ctx->obj_ctx->module_sym_count;
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
        bool defined = false;
        if (op->global_id < ctx->sym_count)
            defined = ctx->sym_defined[op->global_id] != 0;
        else
            defined = is_symbol_defined_in_module(ctx->mod, sym_name);
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
    emit_u32(ctx->buf, &ctx->pos, ctx->buflen, enc_add_imm(true, A64_SP, A64_FP, 0));
    emit_u32(ctx->buf, &ctx->pos, ctx->buflen, 0xA8C17BFDu); /* ldp x29, x30, [sp], #16 */
    emit_u32(ctx->buf, &ctx->pos, ctx->buflen, 0xD65F03C0u); /* ret */
}

static void emit_jmp_a64(a64_compile_ctx_t *ctx, uint32_t target_block) {
    if (ctx->num_fixups < ctx->fixup_cap) {
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
    if (ctx->num_fixups < ctx->fixup_cap) {
        ctx->fixups[ctx->num_fixups].insn_pos = ctx->pos;
        ctx->fixups[ctx->num_fixups].target = target_block;
        ctx->fixups[ctx->num_fixups].kind = 1;
        ctx->fixups[ctx->num_fixups].cond = cond;
        ctx->num_fixups++;
    }
    emit_u32(ctx->buf, &ctx->pos, ctx->buflen, 0x54000000u);
}

static void emit_phi_copies(a64_compile_ctx_t *ctx, lr_phi_copy_t *copies) {
    for (lr_phi_copy_t *pc = copies; pc; pc = pc->next) {
        emit_load_operand(ctx, &pc->src_op, A64_X9);
        emit_store_slot(ctx, pc->dest_vreg, A64_X9);
    }
}

static void emit_mem_copy_base_to_base(a64_compile_ctx_t *ctx,
                                       uint8_t dst_base, int32_t dst_disp,
                                       uint8_t src_base, int32_t src_disp,
                                       size_t bytes) {
    const uint8_t scratch = A64_X11;
    size_t off = 0;
    while (bytes - off >= 8) {
        emit_load(ctx->buf, &ctx->pos, ctx->buflen, scratch, src_base,
                  src_disp + (int32_t)off, 8);
        emit_store(ctx->buf, &ctx->pos, ctx->buflen, scratch, dst_base,
                   dst_disp + (int32_t)off, 8);
        off += 8;
    }
    if (bytes - off >= 4) {
        emit_load(ctx->buf, &ctx->pos, ctx->buflen, scratch, src_base,
                  src_disp + (int32_t)off, 4);
        emit_store(ctx->buf, &ctx->pos, ctx->buflen, scratch, dst_base,
                   dst_disp + (int32_t)off, 4);
        off += 4;
    }
    if (bytes - off >= 2) {
        emit_load(ctx->buf, &ctx->pos, ctx->buflen, scratch, src_base,
                  src_disp + (int32_t)off, 2);
        emit_store(ctx->buf, &ctx->pos, ctx->buflen, scratch, dst_base,
                   dst_disp + (int32_t)off, 2);
        off += 2;
    }
    if (bytes - off == 1) {
        emit_load(ctx->buf, &ctx->pos, ctx->buflen, scratch, src_base,
                  src_disp + (int32_t)off, 1);
        emit_store(ctx->buf, &ctx->pos, ctx->buflen, scratch, dst_base,
                   dst_disp + (int32_t)off, 1);
    }
}

static void emit_mem_zero_base(a64_compile_ctx_t *ctx, uint8_t dst_base,
                               int32_t dst_disp, size_t bytes) {
    const uint8_t scratch = A64_X11;
    size_t off = 0;
    emit_move_imm(ctx->buf, &ctx->pos, ctx->buflen, scratch, 0, true);
    while (bytes - off >= 8) {
        emit_store(ctx->buf, &ctx->pos, ctx->buflen, scratch, dst_base,
                   dst_disp + (int32_t)off, 8);
        off += 8;
    }
    if (bytes - off >= 4) {
        emit_store(ctx->buf, &ctx->pos, ctx->buflen, scratch, dst_base,
                   dst_disp + (int32_t)off, 4);
        off += 4;
    }
    if (bytes - off >= 2) {
        emit_store(ctx->buf, &ctx->pos, ctx->buflen, scratch, dst_base,
                   dst_disp + (int32_t)off, 2);
        off += 2;
    }
    if (bytes - off == 1) {
        emit_store(ctx->buf, &ctx->pos, ctx->buflen, scratch, dst_base,
                   dst_disp + (int32_t)off, 1);
    }
}

static bool aggregate_path_from_inst(const lr_inst_t *inst, const lr_type_t *agg_ty,
                                     size_t *field_off_out,
                                     const lr_type_t **field_ty_out) {
    if (!inst || !agg_ty) {
        return false;
    }
    return lr_aggregate_index_path(agg_ty, inst->indices, inst->num_indices,
                                   field_off_out, field_ty_out);
}

static void emit_load_vreg_mem_sized(a64_compile_ctx_t *ctx, uint32_t src_vreg,
                                     int32_t add_off, uint8_t reg, uint8_t size) {
    int32_t src_off = alloc_slot(ctx, src_vreg, 8) + add_off;
    emit_load(ctx->buf, &ctx->pos, ctx->buflen, reg, A64_FP, src_off, size);
}

static void emit_signext_index_reg(a64_compile_ctx_t *ctx, uint8_t reg,
                                   uint8_t signext_bytes) {
    uint32_t insn;
    if (signext_bytes == 0 || signext_bytes >= 8)
        return;
    insn = 0x93400000u | ((uint32_t)((signext_bytes * 8u) - 1u) << 10)
         | ((uint32_t)reg << 5) | (uint32_t)reg;
    emit_u32(ctx->buf, &ctx->pos, ctx->buflen, insn);
}

static int32_t ensure_static_alloca_offset(a64_compile_ctx_t *ctx, const lr_inst_t *inst) {
    if (inst->dest < ctx->num_static_alloca_offsets) {
        int32_t off = ctx->static_alloca_offsets[inst->dest];
        if (off != 0)
            return off;
    }

    ctx->stack_size += (uint32_t)lr_target_alloca_elem_size(inst, 8);
    ctx->stack_size = (ctx->stack_size + 7u) & ~7u;
    {
        int32_t off = -(int32_t)ctx->stack_size;
        set_static_alloca_offset(ctx, inst->dest, off);
        return off;
    }
}

static void prescan_static_alloca_offsets(a64_compile_ctx_t *ctx, const lr_func_t *func) {
    for (const lr_block_t *b = func->first_block; b; b = b->next) {
        for (const lr_inst_t *inst = b->first; inst; inst = inst->next) {
            if (inst->op != LR_OP_ALLOCA)
                continue;
            if (!lr_target_alloca_uses_static_storage(inst))
                continue;
            (void)ensure_static_alloca_offset(ctx, inst);
        }
    }
}

static void reserve_phi_dest_slots(a64_compile_ctx_t *ctx, lr_phi_copy_t **phi_copies,
                                   uint32_t num_blocks) {
    for (uint32_t bi = 0; bi < num_blocks; bi++) {
        for (lr_phi_copy_t *pc = phi_copies[bi]; pc; pc = pc->next)
            alloc_slot(ctx, pc->dest_vreg, 8);
    }
}

static size_t emit_prologue_a64(a64_compile_ctx_t *ctx) {
    emit_u32(ctx->buf, &ctx->pos, ctx->buflen, 0xA9BF7BFDu); /* stp x29, x30, [sp, #-16]! */
    emit_u32(ctx->buf, &ctx->pos, ctx->buflen, 0x910003FDu); /* mov x29, sp */
    {
        size_t imm_pos = ctx->pos;
        emit_u32(ctx->buf, &ctx->pos, ctx->buflen, enc_movz(true, A64_X15, 0, 0));
        emit_u32(ctx->buf, &ctx->pos, ctx->buflen, enc_movk(true, A64_X15, 0, 1));
        emit_u32(ctx->buf, &ctx->pos, ctx->buflen, enc_add_imm(true, A64_X14, A64_SP, 0));
        emit_u32(ctx->buf, &ctx->pos, ctx->buflen, enc_sub_reg(true, A64_X14, A64_X14, A64_X15));
        emit_u32(ctx->buf, &ctx->pos, ctx->buflen, enc_add_imm(true, A64_SP, A64_X14, 0));
        return imm_pos;
    }
}

static void patch_prologue_stack_adjust(a64_compile_ctx_t *ctx, size_t imm_pos,
                                        uint32_t frame_stack_size) {
    patch_u32(ctx->buf, ctx->buflen, imm_pos + 0,
              enc_movz(true, A64_X15, (uint16_t)(frame_stack_size & 0xFFFFu), 0));
    patch_u32(ctx->buf, ctx->buflen, imm_pos + 4,
              enc_movk(true, A64_X15, (uint16_t)((frame_stack_size >> 16) & 0xFFFFu), 1));
}

/*
 * aarch64_compile_func: single-pass ISel + encoding.
 * Replaces the old two-phase isel_func + encode_func approach.
 */
static int aarch64_compile_func(lr_func_t *func, lr_module_t *mod,
                                 uint8_t *buf, size_t buflen, size_t *out_len,
                                 lr_arena_t *arena) {
    uint32_t nb = func->num_blocks > 0 ? func->num_blocks : 1;
    uint32_t fc = nb * 2;
    a64_compile_ctx_t ctx = {
        .buf = buf,
        .buflen = buflen,
        .pos = 0,
        .stack_size = 0,
        .stack_slots = NULL,
        .stack_slot_sizes = NULL,
        .num_stack_slots = 0,
        .static_alloca_offsets = NULL,
        .num_static_alloca_offsets = 0,
        .block_offsets = lr_arena_array_uninit(arena, size_t, nb),
        .num_block_offsets = nb,
        .fixups = lr_arena_array_uninit(arena, a64_fixup_t, fc),
        .num_fixups = 0,
        .fixup_cap = fc,
        .arena = arena,
        .obj_ctx = mod ? (lr_objfile_ctx_t *)mod->obj_ctx : NULL,
        .mod = mod,
        .sym_defined = NULL,
        .sym_count = 0,
    };

    attach_obj_symbol_defined_cache(&ctx);

    lr_phi_copy_t **phi_copies = lr_build_phi_copies(ctx.arena, func);
    reserve_phi_dest_slots(&ctx, phi_copies, nb);
    prescan_static_alloca_offsets(&ctx, func);

    size_t prologue_stack_patch_pos = emit_prologue_a64(&ctx);

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

                uint8_t cc = lr_target_cc_from_icmp(inst->icmp_pred);

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
                size_t elem_sz = lr_target_alloca_elem_size(inst, 8);

                bool use_static = lr_target_alloca_uses_static_storage(inst);

                if (use_static) {
                    int32_t off = ensure_static_alloca_offset(&ctx, inst);
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
                             enc_add_imm(true, A64_X14, A64_SP, 0));
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             enc_sub_reg(true, A64_X14, A64_X14, A64_X9));
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             enc_add_imm(true, A64_SP, A64_X14, 0));
                    /* x9 now holds alloca result pointer */
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             enc_add_imm(true, A64_X9, A64_SP, 0));
                    emit_store_slot(&ctx, inst->dest, A64_X9);
                }
                break;
            }
            case LR_OP_LOAD: {
                emit_load_operand(&ctx, &inst->operands[0], A64_X9);
                size_t load_sz = lr_type_size(inst->type);
                if (load_sz == 0)
                    load_sz = 8;
                if (load_sz > 8) {
                    int32_t dst_off = alloc_slot(&ctx, inst->dest, load_sz);
                    emit_mem_copy_base_to_base(&ctx, A64_FP, dst_off, A64_X9, 0, load_sz);
                } else {
                    emit_load(ctx.buf, &ctx.pos, ctx.buflen, A64_X9, A64_X9, 0, (uint8_t)load_sz);
                    emit_store_slot(&ctx, inst->dest, A64_X9);
                }
                break;
            }
            case LR_OP_STORE: {
                emit_load_operand(&ctx, &inst->operands[1], A64_X10);
                size_t store_sz = lr_type_size(inst->operands[0].type);
                if (store_sz == 0)
                    store_sz = 8;
                if (store_sz > 8) {
                    if (inst->operands[0].kind == LR_VAL_IMM_I64 &&
                        inst->operands[0].imm_i64 == 0) {
                        emit_mem_zero_base(&ctx, A64_X10, 0, store_sz);
                        break;
                    }
                    if (inst->operands[0].kind == LR_VAL_VREG &&
                        inst->operands[0].vreg < ctx.num_stack_slots &&
                        ctx.stack_slot_sizes[inst->operands[0].vreg] > 0) {
                        uint32_t vreg = inst->operands[0].vreg;
                        int32_t src_off = alloc_slot(&ctx, vreg, 8);
                        size_t src_sz = ctx.stack_slot_sizes[vreg];
                        if (src_sz > store_sz)
                            src_sz = store_sz;
                        if (src_sz > 0) {
                            emit_mem_copy_base_to_base(&ctx, A64_X10, 0, A64_FP, src_off, src_sz);
                        }
                        if (src_sz < store_sz) {
                            emit_mem_zero_base(&ctx, A64_X10, (int32_t)src_sz, store_sz - src_sz);
                        }
                        break;
                    }
                    emit_mem_zero_base(&ctx, A64_X10, 0, store_sz);
                    break;
                }
                emit_load_operand(&ctx, &inst->operands[0], A64_X9);
                emit_store(ctx.buf, &ctx.pos, ctx.buflen, A64_X9, A64_X10, 0, (uint8_t)store_sz);
                break;
            }
            case LR_OP_GEP: {
                emit_load_operand(&ctx, &inst->operands[0], A64_X9);
                const lr_type_t *cur_ty = inst->type;
                for (uint32_t idx = 1; idx < inst->num_operands; idx++) {
                    const lr_operand_t *idx_op = &inst->operands[idx];
                    lr_gep_step_t step;
                    if (!lr_gep_analyze_step(cur_ty, idx == 1, idx_op, &step)) {
                        continue;
                    }
                    cur_ty = step.next_type;
                    if (step.is_const) {
                        if (step.const_byte_offset == 0)
                            continue;
                        emit_move_imm(ctx.buf, &ctx.pos, ctx.buflen,
                                      A64_X10, step.const_byte_offset, true);
                        emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                                 enc_add_reg(true, A64_X9, A64_X9, A64_X10));
                        continue;
                    }

                    emit_load_operand(&ctx, idx_op, A64_X10);
                    emit_signext_index_reg(&ctx, A64_X10, step.runtime_signext_bytes);
                    if (step.runtime_elem_size != 1) {
                        emit_move_imm(ctx.buf, &ctx.pos, ctx.buflen,
                                      A64_X11, (int64_t)step.runtime_elem_size, true);
                        emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                                 enc_mul(true, A64_X10, A64_X10, A64_X11));
                    }
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             enc_add_reg(true, A64_X9, A64_X9, A64_X10));
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

                uint8_t cc = lr_target_cc_from_fcmp(inst->fcmp_pred);

                emit_setcc_a64(&ctx, cc, A64_X9);
                emit_store_slot(&ctx, inst->dest, A64_X9);
                break;
            }
            case LR_OP_SITOFP: {
                uint8_t fsize = (inst->type && inst->type->kind == LR_TYPE_FLOAT) ? 4 : 8;
                emit_load_operand(&ctx, &inst->operands[0], A64_X9);
                size_t src_sz = lr_type_size(inst->operands[0].type);
                if (src_sz <= 4) {
                    /* sxtw x9, w9 — sign-extend 32-bit to 64-bit */
                    emit_u32(ctx.buf, &ctx.pos, ctx.buflen,
                             0x93407C00u | ((uint32_t)A64_X9 << 5) | A64_X9);
                }
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
                size_t field_off = 0;
                const lr_type_t *field_ty = NULL;
                size_t field_sz = 8;
                bool have_path = false;

                if (inst->num_operands > 0 && inst->operands[0].type) {
                    have_path = aggregate_path_from_inst(inst, inst->operands[0].type,
                                                         &field_off, &field_ty);
                }
                if (field_ty) {
                    field_sz = lr_type_size(field_ty);
                }
                if (field_sz == 0) {
                    field_sz = 8;
                }

                if (have_path && inst->num_operands > 0 &&
                    inst->operands[0].kind == LR_VAL_VREG) {
                    if (field_sz > 8) {
                        int32_t dst_off = alloc_slot(&ctx, inst->dest, field_sz);
                        int32_t src_off = alloc_slot(&ctx, inst->operands[0].vreg, 8) +
                                          (int32_t)field_off;
                        emit_mem_copy_base_to_base(&ctx, A64_FP, dst_off, A64_FP,
                                                   src_off, field_sz);
                    } else {
                        emit_load_vreg_mem_sized(&ctx, inst->operands[0].vreg,
                                                 (int32_t)field_off, A64_X9,
                                                 (uint8_t)field_sz);
                        emit_store_slot(&ctx, inst->dest, A64_X9);
                    }
                    break;
                }

                if (inst->num_operands > 0 &&
                    (inst->operands[0].kind == LR_VAL_UNDEF ||
                     inst->operands[0].kind == LR_VAL_NULL)) {
                    if (field_sz > 8) {
                        int32_t dst_off = alloc_slot(&ctx, inst->dest, field_sz);
                        emit_mem_zero_base(&ctx, A64_FP, dst_off, field_sz);
                    } else {
                        emit_move_imm(ctx.buf, &ctx.pos, ctx.buflen, A64_X9, 0, true);
                        emit_store_slot(&ctx, inst->dest, A64_X9);
                    }
                    break;
                }

                emit_load_operand(&ctx, &inst->operands[0], A64_X9);
                emit_store_slot(&ctx, inst->dest, A64_X9);
                break;
            }
            case LR_OP_INSERTVALUE: {
                size_t agg_sz = inst->type ? lr_type_size(inst->type) : 8;
                size_t field_off = 0;
                const lr_type_t *field_ty = NULL;
                int32_t dst_off;

                if (agg_sz < 8) {
                    agg_sz = 8;
                }
                dst_off = alloc_slot(&ctx, inst->dest, agg_sz);

                if (inst->num_operands > 0) {
                    const lr_operand_t *agg = &inst->operands[0];
                    if (agg->kind == LR_VAL_VREG) {
                        size_t src_sz = 0;
                        int32_t src_off = alloc_slot(&ctx, agg->vreg, 8);
                        if (agg->vreg < ctx.num_stack_slots) {
                            src_sz = ctx.stack_slot_sizes[agg->vreg];
                        }
                        if (src_sz > agg_sz) {
                            src_sz = agg_sz;
                        }
                        if (src_sz > 0) {
                            emit_mem_copy_base_to_base(&ctx, A64_FP, dst_off, A64_FP,
                                                       src_off, src_sz);
                        }
                        if (src_sz < agg_sz) {
                            emit_mem_zero_base(&ctx, A64_FP,
                                               dst_off + (int32_t)src_sz,
                                               agg_sz - src_sz);
                        }
                    } else if (agg->kind == LR_VAL_UNDEF || agg->kind == LR_VAL_NULL) {
                        emit_mem_zero_base(&ctx, A64_FP, dst_off, agg_sz);
                    } else if (agg_sz <= 8) {
                        emit_load_operand(&ctx, agg, A64_X9);
                        emit_store(ctx.buf, &ctx.pos, ctx.buflen, A64_X9,
                                   A64_FP, dst_off, (uint8_t)agg_sz);
                    } else {
                        emit_mem_zero_base(&ctx, A64_FP, dst_off, agg_sz);
                    }
                }

                if (inst->num_operands < 2 ||
                    !aggregate_path_from_inst(inst, inst->type, &field_off, &field_ty) ||
                    !field_ty) {
                    break;
                }

                {
                    size_t field_sz = lr_type_size(field_ty);
                    const lr_operand_t *val = &inst->operands[1];
                    if (field_sz == 0) {
                        break;
                    }
                    if (field_sz > 8) {
                        if (val->kind == LR_VAL_VREG) {
                            size_t src_sz = 0;
                            int32_t src_off = alloc_slot(&ctx, val->vreg, 8);
                            if (val->vreg < ctx.num_stack_slots) {
                                src_sz = ctx.stack_slot_sizes[val->vreg];
                            }
                            if (src_sz > field_sz) {
                                src_sz = field_sz;
                            }
                            if (src_sz > 0) {
                                emit_mem_copy_base_to_base(&ctx, A64_FP,
                                                           dst_off + (int32_t)field_off,
                                                           A64_FP, src_off, src_sz);
                            }
                            if (src_sz < field_sz) {
                                emit_mem_zero_base(&ctx, A64_FP,
                                                   dst_off + (int32_t)field_off +
                                                   (int32_t)src_sz,
                                                   field_sz - src_sz);
                            }
                        } else {
                            emit_mem_zero_base(&ctx, A64_FP,
                                               dst_off + (int32_t)field_off,
                                               field_sz);
                        }
                    } else {
                        if (val->kind == LR_VAL_UNDEF || val->kind == LR_VAL_NULL) {
                            emit_move_imm(ctx.buf, &ctx.pos, ctx.buflen, A64_X9, 0, true);
                        } else {
                            emit_load_operand(&ctx, val, A64_X9);
                        }
                        emit_store(ctx.buf, &ctx.pos, ctx.buflen, A64_X9, A64_FP,
                                   dst_off + (int32_t)field_off, (uint8_t)field_sz);
                    }
                }
                break;
            }
            case LR_OP_CALL: {
                static const uint8_t call_regs[] = {
                    A64_X0, A64_X1, A64_X2, A64_X3,
                    A64_X4, A64_X5, A64_X6, A64_X7
                };
                static const uint8_t call_fp_regs[] = {
                    A64_D0, A64_D1, A64_D2, A64_D3,
                    A64_D4, A64_D5, A64_D6, A64_D7
                };
                uint32_t nargs = inst->num_operands - 1;
                uint32_t gp_used = 0, fp_used = 0, stack_args = 0;

                bool use_fp_abi = inst->call_external_abi;
                if (inst->operands[0].kind == LR_VAL_GLOBAL)
                    use_fp_abi = true;

                if (use_fp_abi) {
                    for (uint32_t i = 0; i < nargs; i++) {
                        const lr_type_t *at = inst->operands[i + 1].type;
                        bool is_fp = at && (at->kind == LR_TYPE_FLOAT ||
                                            at->kind == LR_TYPE_DOUBLE);
                        if (is_fp) {
                            if (fp_used < 8) fp_used++; else stack_args++;
                        } else {
                            if (gp_used < 8) gp_used++; else stack_args++;
                        }
                    }
                } else {
                    stack_args = nargs > 8 ? nargs - 8 : 0;
                }

                uint32_t stack_bytes = ((stack_args * 8 + 15) & ~15u);
                if (stack_bytes > 0)
                    emit_sp_adjust(ctx.buf, &ctx.pos, ctx.buflen, stack_bytes, true);

                if (use_fp_abi) {
                    uint32_t si = 0;
                    gp_used = 0; fp_used = 0;
                    for (uint32_t i = 0; i < nargs; i++) {
                        const lr_operand_t *arg = &inst->operands[i + 1];
                        bool is_fp = arg->type &&
                                     (arg->type->kind == LR_TYPE_FLOAT ||
                                      arg->type->kind == LR_TYPE_DOUBLE);
                        uint8_t fsz = (arg->type && arg->type->kind == LR_TYPE_FLOAT) ? 4 : 8;
                        if (is_fp && fp_used < 8) {
                            emit_load_fp_operand(&ctx, arg, call_fp_regs[fp_used], fsz);
                            fp_used++;
                        } else if (!is_fp && gp_used < 8) {
                            emit_load_operand(&ctx, arg, call_regs[gp_used]);
                            gp_used++;
                        } else {
                            emit_load_operand(&ctx, arg, A64_X9);
                            emit_store(ctx.buf, &ctx.pos, ctx.buflen,
                                       A64_X9, A64_SP, (int32_t)(si * 8), 8);
                            si++;
                        }
                    }
                } else {
                    uint32_t nstack = nargs > 8 ? nargs - 8 : 0;
                    for (uint32_t i = 0; i < nstack; i++) {
                        emit_load_operand(&ctx, &inst->operands[8 + i + 1], A64_X9);
                        emit_store(ctx.buf, &ctx.pos, ctx.buflen,
                                   A64_X9, A64_SP, (int32_t)(i * 8), 8);
                    }
                    for (uint32_t i = 0; i < nargs && i < 8; i++)
                        emit_load_operand(&ctx, &inst->operands[i + 1], call_regs[i]);
                }

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

                if (inst->type && inst->type->kind != LR_TYPE_VOID) {
                    bool ret_fp = use_fp_abi && inst->type &&
                                  (inst->type->kind == LR_TYPE_FLOAT ||
                                   inst->type->kind == LR_TYPE_DOUBLE);
                    if (ret_fp) {
                        uint8_t rsz = (inst->type->kind == LR_TYPE_FLOAT) ? 4 : 8;
                        emit_store_fp_slot(&ctx, inst->dest, A64_D0, rsz);
                    } else {
                        emit_store_slot(&ctx, inst->dest, A64_X0);
                    }
                }
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

    patch_prologue_stack_adjust(&ctx, prologue_stack_patch_pos,
                                (ctx.stack_size + 15u) & ~15u);

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
