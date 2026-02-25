#include "target_aarch64.h"
#include "target_common.h"
#include "target_shared.h"
#include "objfile.h"
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

typedef struct {
    size_t insn_pos;
    size_t target_pos_hint;
    uint32_t target;
    uint32_t source;
    uint8_t kind;
    uint8_t cond;
} a64_fixup_t;

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
    size_t *block_entry_offsets;
    uint32_t num_block_offsets;
    a64_fixup_t *fixups;
    uint32_t num_fixups;
    uint32_t fixup_cap;
    lr_arena_t *arena;
    lr_objfile_ctx_t *obj_ctx;
    lr_module_t *mod;
    uint8_t *sym_defined;
    lr_func_t **sym_funcs;
    uint32_t sym_count;
    uint32_t x9_holds_vreg;
    uint32_t x10_holds_vreg;
    bool func_uses_fp_abi;
    bool func_is_vararg;
    int32_t vararg_stack_start_off;
    const char *func_name;
} a64_compile_ctx_t;

static size_t align_up_size(size_t value, size_t align) {
    if (align <= 1)
        return value;
    return ((value + align - 1) / align) * align;
}

static bool is_fp_abi_type(const lr_type_t *type) {
    return type &&
           (type->kind == LR_TYPE_FLOAT || type->kind == LR_TYPE_DOUBLE);
}

static uint8_t fp_abi_size(const lr_type_t *type) {
    return (type && type->kind == LR_TYPE_FLOAT) ? 4 : 8;
}

static void invalidate_cached_reg_a64(a64_compile_ctx_t *ctx, uint8_t reg) {
    if (!ctx) return;
    if (reg == A64_X9) ctx->x9_holds_vreg = UINT32_MAX;
    if (reg == A64_X10) ctx->x10_holds_vreg = UINT32_MAX;
}

static void invalidate_cached_gprs_a64(a64_compile_ctx_t *ctx) {
    if (!ctx) return;
    ctx->x9_holds_vreg = UINT32_MAX;
    ctx->x10_holds_vreg = UINT32_MAX;
}

static bool cached_reg_holds_vreg_a64(const a64_compile_ctx_t *ctx, uint8_t reg, uint32_t vreg) {
    if (!ctx) return false;
    if (reg == A64_X9) return ctx->x9_holds_vreg == vreg;
    if (reg == A64_X10) return ctx->x10_holds_vreg == vreg;
    return false;
}

static void set_cached_reg_vreg_a64(a64_compile_ctx_t *ctx, uint8_t reg, uint32_t vreg) {
    if (!ctx) return;
    if (reg == A64_X9) ctx->x9_holds_vreg = vreg;
    if (reg == A64_X10) ctx->x10_holds_vreg = vreg;
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

    if (ctx->stack_slots[vreg] != 0) {
        if ((uint32_t)size <= ctx->stack_slot_sizes[vreg])
            return ctx->stack_slots[vreg];
        /* Existing slot too small; allocate a larger one and rebind vreg. */
    }

    if (size < 8) size = 8;
    ctx->stack_size = (uint32_t)align_up_size(ctx->stack_size, 8);
    ctx->stack_size += (uint32_t)size;
    int32_t offset = -(int32_t)ctx->stack_size;
    if (getenv("LIRIC_DBG_A64_SLOTS") != NULL) {
        fprintf(stderr,
                "[a64 slot] func=%s vreg=%u off=%d size=%zu\n",
                ctx->func_name ? ctx->func_name : "<anon>",
                vreg, offset, size);
    }
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

static void emit_move_imm_ctx(a64_compile_ctx_t *ctx, uint8_t rd,
                              int64_t imm, bool is64) {
    emit_move_imm(ctx->buf, &ctx->pos, ctx->buflen, rd, imm, is64);
    invalidate_cached_reg_a64(ctx, rd);
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

static uint32_t enc_ucvtf(uint8_t fsize, uint8_t fd, uint8_t xn) {
    uint32_t base = (fsize == 8) ? 0x9E630000u : 0x9E230000u;
    return base | ((uint32_t)xn << 5) | fd;
}

static uint32_t enc_fcvtzs(uint8_t fsize, uint8_t xd, uint8_t fn) {
    uint32_t base = (fsize == 8) ? 0x9E780000u : 0x9E380000u;
    return base | ((uint32_t)fn << 5) | xd;
}

static uint32_t enc_fcvtzu(uint8_t fsize, uint8_t xd, uint8_t fn) {
    uint32_t base = (fsize == 8) ? 0x9E790000u : 0x9E390000u;
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
    {
        const char *watch_off = getenv("LIRIC_DBG_A64_WATCH_OFF");
        if (watch_off && watch_off[0] != '\0' &&
            off == (int32_t)strtol(watch_off, NULL, 10)) {
            fprintf(stderr,
                    "[a64 slot-use] func=%s kind=load vreg=%u off=%d pos=%zu\n",
                    ctx->func_name ? ctx->func_name : "<anon>",
                    vreg, off, ctx->pos);
        }
    }
    emit_load(ctx->buf, &ctx->pos, ctx->buflen, reg, A64_FP, off, 8);
    set_cached_reg_vreg_a64(ctx, reg, vreg);
}

static void emit_store_slot(a64_compile_ctx_t *ctx, uint32_t vreg, uint8_t reg) {
    int32_t off = alloc_slot(ctx, vreg, 8);
    {
        const char *watch_off = getenv("LIRIC_DBG_A64_WATCH_OFF");
        if (watch_off && watch_off[0] != '\0' &&
            off == (int32_t)strtol(watch_off, NULL, 10)) {
            fprintf(stderr,
                    "[a64 slot-use] func=%s kind=store vreg=%u off=%d pos=%zu\n",
                    ctx->func_name ? ctx->func_name : "<anon>",
                    vreg, off, ctx->pos);
        }
    }
    emit_store(ctx->buf, &ctx->pos, ctx->buflen, reg, A64_FP, off, 8);
    set_cached_reg_vreg_a64(ctx, reg, vreg);
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

static lr_func_t *find_module_function(lr_module_t *mod, const char *name) {
    if (!mod || !name)
        return NULL;
    for (lr_func_t *f = mod->first_func; f; f = f->next) {
        if (f->name && strcmp(f->name, name) == 0)
            return f;
    }
    return NULL;
}

static void attach_obj_symbol_meta_cache(a64_compile_ctx_t *ctx) {
    if (!ctx || !ctx->obj_ctx)
        return;
    ctx->sym_defined = ctx->obj_ctx->module_sym_defined;
    ctx->sym_funcs = ctx->obj_ctx->module_sym_funcs;
    ctx->sym_count = ctx->obj_ctx->module_sym_count;
}

static bool direct_call_uses_external_fp_abi(
    a64_compile_ctx_t *cc, const lr_operand_t *callee_op, bool call_external_abi,
    bool call_vararg, uint32_t call_fixed_args, bool *out_vararg,
    uint32_t *out_fixed_args) {
    lr_func_t *callee_func = NULL;
    if (out_vararg)
        *out_vararg = call_vararg;
    if (out_fixed_args)
        *out_fixed_args = call_fixed_args;

    if (!cc || !cc->mod || !callee_op)
        return call_external_abi;

    if (callee_op->kind == LR_VAL_GLOBAL) {
        if (callee_op->global_id < cc->sym_count && cc->sym_funcs)
            callee_func = cc->sym_funcs[callee_op->global_id];
        if (callee_func) {
            bool callee_vararg = callee_func->vararg || call_vararg;
            if (out_vararg)
                *out_vararg = callee_vararg;
            if (out_fixed_args && *out_fixed_args == 0u &&
                callee_func->num_params > 0u) {
                *out_fixed_args = callee_func->num_params;
            }
            /* Keep C varargs ABI-compatible even for module-defined callees.
               Internal direct ABI does not model platform va_list register
               spill areas (notably Darwin stack-only variadics). */
            if (callee_vararg)
                return true;
            return callee_func->first_block == NULL ||
                   callee_func->uses_llvm_abi;
        }
        if (callee_op->global_id < cc->sym_count) {
            if (cc->sym_defined)
                return cc->sym_defined[callee_op->global_id] == 0;
            return true;
        }
        {
            const char *sym_name = lr_module_symbol_name(cc->mod,
                                                         callee_op->global_id);
            if (!sym_name)
                return call_external_abi;
            callee_func = find_module_function(cc->mod, sym_name);
            if (callee_func) {
                bool callee_vararg = callee_func->vararg || call_vararg;
                if (out_vararg)
                    *out_vararg = callee_vararg;
                if (out_fixed_args && *out_fixed_args == 0u &&
                    callee_func->num_params > 0u) {
                    *out_fixed_args = callee_func->num_params;
                }
                if (callee_vararg)
                    return true;
                return callee_func->first_block == NULL ||
                       callee_func->uses_llvm_abi;
            }
            return !is_symbol_defined_in_module(cc->mod, sym_name);
        }
    }

    return call_external_abi;
}

static const char *normalize_llvm_symbol_name(const char *name) {
    if (!name)
        return NULL;
    while (*name == '\1' || *name == '_')
        name++;
    return name;
}

static bool is_llvm_va_start_name(const char *name) {
    name = normalize_llvm_symbol_name(name);
    return name && strncmp(name, "llvm.va_start", 13) == 0;
}

static bool is_llvm_va_end_name(const char *name) {
    name = normalize_llvm_symbol_name(name);
    return name && strncmp(name, "llvm.va_end", 11) == 0;
}

static bool is_llvm_va_copy_name(const char *name) {
    name = normalize_llvm_symbol_name(name);
    return name && strncmp(name, "llvm.va_copy", 12) == 0;
}

static uint8_t llvm_objectsize_bits(const char *name) {
    name = normalize_llvm_symbol_name(name);
    if (!name)
        return 0;
    if (strcmp(name, "llvm.objectsize.i64") == 0 ||
        strncmp(name, "llvm.objectsize.i64.", 20) == 0) {
        return 64;
    }
    if (strcmp(name, "llvm.objectsize.i32") == 0 ||
        strncmp(name, "llvm.objectsize.i32.", 20) == 0) {
        return 32;
    }
    return 0;
}

typedef enum a64_int_cmp_intrinsic_kind {
    A64_INT_CMP_INTRIN_NONE = 0,
    A64_INT_CMP_INTRIN_UMAX,
    A64_INT_CMP_INTRIN_UMIN,
    A64_INT_CMP_INTRIN_SMAX,
    A64_INT_CMP_INTRIN_SMIN
} a64_int_cmp_intrinsic_kind_t;

static a64_int_cmp_intrinsic_kind_t classify_llvm_int_cmp_intrinsic(const char *name,
                                                                     bool *is64_out) {
    name = normalize_llvm_symbol_name(name);
    if (is64_out)
        *is64_out = true;
    if (!name)
        return A64_INT_CMP_INTRIN_NONE;
    if (strcmp(name, "llvm.umax.i64") == 0)
        return A64_INT_CMP_INTRIN_UMAX;
    if (strcmp(name, "llvm.umin.i64") == 0)
        return A64_INT_CMP_INTRIN_UMIN;
    if (strcmp(name, "llvm.smax.i64") == 0)
        return A64_INT_CMP_INTRIN_SMAX;
    if (strcmp(name, "llvm.smin.i64") == 0)
        return A64_INT_CMP_INTRIN_SMIN;
    if (strcmp(name, "llvm.umax.i32") == 0) {
        if (is64_out)
            *is64_out = false;
        return A64_INT_CMP_INTRIN_UMAX;
    }
    if (strcmp(name, "llvm.umin.i32") == 0) {
        if (is64_out)
            *is64_out = false;
        return A64_INT_CMP_INTRIN_UMIN;
    }
    if (strcmp(name, "llvm.smax.i32") == 0) {
        if (is64_out)
            *is64_out = false;
        return A64_INT_CMP_INTRIN_SMAX;
    }
    if (strcmp(name, "llvm.smin.i32") == 0) {
        if (is64_out)
            *is64_out = false;
        return A64_INT_CMP_INTRIN_SMIN;
    }
    return A64_INT_CMP_INTRIN_NONE;
}

static uint8_t int_type_width_bits(const lr_type_t *type) {
    size_t fallback_bits = 64;
    if (!type)
        return (uint8_t)fallback_bits;
    switch (type->kind) {
    case LR_TYPE_I1:  return 1;
    case LR_TYPE_I8:  return 8;
    case LR_TYPE_I16: return 16;
    case LR_TYPE_I32: return 32;
    case LR_TYPE_I64: return 64;
    case LR_TYPE_PTR: return 64;
    default:
        break;
    }
    fallback_bits = lr_type_size(type) * 8;
    if (fallback_bits == 0 || fallback_bits > 64)
        fallback_bits = 64;
    return (uint8_t)fallback_bits;
}

static bool emit_copy_from_cached_scratch_a64(a64_compile_ctx_t *ctx,
                                              uint32_t vreg, uint8_t dst_reg) {
    uint8_t src_reg;

    if (!ctx)
        return false;
    if (dst_reg == A64_X9 && cached_reg_holds_vreg_a64(ctx, A64_X10, vreg)) {
        src_reg = A64_X10;
    } else if (dst_reg == A64_X10 &&
               cached_reg_holds_vreg_a64(ctx, A64_X9, vreg)) {
        src_reg = A64_X9;
    } else {
        return false;
    }

    emit_mov_reg(ctx->buf, &ctx->pos, ctx->buflen, dst_reg, src_reg, true);
    set_cached_reg_vreg_a64(ctx, dst_reg, vreg);
    return true;
}

static void emit_load_operand(a64_compile_ctx_t *ctx,
                               const lr_operand_t *op, uint8_t reg) {
    if (op->kind == LR_VAL_IMM_I64) {
        emit_move_imm_ctx(ctx, reg, op->imm_i64, true);
    } else if (op->kind == LR_VAL_VREG) {
        int32_t static_alloca_off = lr_target_lookup_static_alloca_offset(
            ctx->static_alloca_offsets, ctx->num_static_alloca_offsets,
            op->vreg);
        {
            const char *watch_env = getenv("LIRIC_DBG_A64_WATCH_VREG");
            if (watch_env && watch_env[0] != '\0') {
                uint32_t watch = (uint32_t)strtoul(watch_env, NULL, 10);
                if (op->vreg == watch) {
                    fprintf(stderr,
                            "[a64 watch-load] func=%s vreg=%u static_off=%d nstatic=%u pos=%zu\n",
                            ctx->func_name ? ctx->func_name : "<anon>",
                            op->vreg,
                            static_alloca_off,
                            ctx->num_static_alloca_offsets,
                            ctx->pos);
                }
            }
        }
        if (static_alloca_off != 0 &&
            getenv("LIRIC_DISABLE_STATIC_ALLOCA_ADDR") == NULL) {
            if (getenv("LIRIC_DBG_A64_STATIC_ALLOCA") != NULL) {
                fprintf(stderr,
                        "[a64 static-alloca-load] func=%s vreg=%u off=%d pos=%zu\n",
                        ctx->func_name ? ctx->func_name : "<anon>",
                        op->vreg, static_alloca_off, ctx->pos);
            }
            emit_addr(ctx->buf, &ctx->pos, ctx->buflen, reg, A64_FP,
                      static_alloca_off);
            set_cached_reg_vreg_a64(ctx, reg, op->vreg);
            return;
        }
        if (cached_reg_holds_vreg_a64(ctx, reg, op->vreg))
            return;
        if (emit_copy_from_cached_scratch_a64(ctx, op->vreg, reg))
            return;
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
        emit_move_imm_ctx(ctx, reg, imm_bits, true);
    } else if (op->kind == LR_VAL_NULL || op->kind == LR_VAL_UNDEF) {
        emit_move_imm_ctx(ctx, reg, 0, true);
    } else if (op->kind == LR_VAL_GLOBAL && ctx->obj_ctx) {
        const char *sym_name = lr_module_symbol_name(ctx->mod,
                                                      op->global_id);
        if (!sym_name) {
            emit_move_imm_ctx(ctx, reg, 0, true);
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
        if (op->global_offset != 0) {
            /* Constant-GEP globals carry byte addends in global_offset. */
            uint8_t add_reg = (reg == A64_X15) ? A64_X14 : A64_X15;
            emit_move_imm_ctx(ctx, add_reg, op->global_offset, true);
            emit_u32(ctx->buf, &ctx->pos, ctx->buflen,
                     enc_add_reg(true, reg, reg, add_reg));
        }
        invalidate_cached_reg_a64(ctx, reg);
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
            emit_move_imm_ctx(ctx, dst, 0, false);
            emit_move_imm_ctx(ctx, A64_X15, 1, false);
            emit_u32(ctx->buf, &ctx->pos, ctx->buflen,
                     enc_csel(false, dst, A64_X15, dst, 4));  /* MI */
            emit_u32(ctx->buf, &ctx->pos, ctx->buflen,
                     enc_csel(false, dst, A64_X15, dst, 12)); /* GT */
        } else if (cc == LR_CC_FP_UEQ) {
            emit_move_imm_ctx(ctx, dst, 0, false);
            emit_move_imm_ctx(ctx, A64_X15, 1, false);
            emit_u32(ctx->buf, &ctx->pos, ctx->buflen,
                     enc_csel(false, dst, A64_X15, dst, 0));  /* EQ */
            emit_u32(ctx->buf, &ctx->pos, ctx->buflen,
                     enc_csel(false, dst, A64_X15, dst, 6));  /* VS */
        } else {
            uint8_t cond = lr_fp_cc_to_a64(cc);
            emit_move_imm_ctx(ctx, dst, 1, false);
            emit_u32(ctx->buf, &ctx->pos, ctx->buflen,
                     enc_csel(false, dst, dst, A64_SP, cond));
        }
    } else {
        uint8_t cond = lr_cc_to_a64(cc);
        emit_move_imm_ctx(ctx, dst, 1, false);
        emit_u32(ctx->buf, &ctx->pos, ctx->buflen,
                 enc_csel(false, dst, dst, A64_SP, cond));
    }
    invalidate_cached_reg_a64(ctx, dst);
}

static void emit_epilogue_a64(a64_compile_ctx_t *ctx) {
    emit_u32(ctx->buf, &ctx->pos, ctx->buflen, enc_add_imm(true, A64_SP, A64_FP, 0));
    emit_u32(ctx->buf, &ctx->pos, ctx->buflen, 0xA8C17BFDu); /* ldp x29, x30, [sp], #16 */
    emit_u32(ctx->buf, &ctx->pos, ctx->buflen, 0xD65F03C0u); /* ret */
}

static void emit_jmp_a64(a64_compile_ctx_t *ctx, uint32_t target_block) {
    if (ctx->num_fixups < ctx->fixup_cap) {
        size_t hint = SIZE_MAX;
        ctx->fixups[ctx->num_fixups].insn_pos = ctx->pos;
        if (target_block < ctx->num_block_offsets) {
            size_t entry = ctx->block_entry_offsets[target_block];
            size_t off = ctx->block_offsets[target_block];
            if (entry != SIZE_MAX && off != SIZE_MAX)
                hint = entry < off ? entry : off;
            else if (entry != SIZE_MAX)
                hint = entry;
            else if (off != SIZE_MAX)
                hint = off;
        }
        ctx->fixups[ctx->num_fixups].target_pos_hint = hint;
        ctx->fixups[ctx->num_fixups].target = target_block;
        ctx->fixups[ctx->num_fixups].kind = 0;
        ctx->fixups[ctx->num_fixups].cond = 0;
        ctx->num_fixups++;
    }
    emit_u32(ctx->buf, &ctx->pos, ctx->buflen, 0x14000000u);
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
    emit_move_imm_ctx(ctx, scratch, 0, true);
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

static void emit_load_vreg_mem_sized(a64_compile_ctx_t *ctx, uint32_t src_vreg,
                                     int32_t add_off, uint8_t reg, uint8_t size) {
    int32_t src_off = alloc_slot(ctx, src_vreg, 8) + add_off;
    emit_load(ctx->buf, &ctx->pos, ctx->buflen, reg, A64_FP, src_off, size);
}

static size_t vreg_slot_size(const a64_compile_ctx_t *ctx, uint32_t vreg) {
    if (!ctx || vreg >= ctx->num_stack_slots || ctx->stack_slot_sizes[vreg] == 0)
        return 8;
    return (size_t)ctx->stack_slot_sizes[vreg];
}

static bool vreg_uses_indirect_aggregate_storage(a64_compile_ctx_t *ctx,
                                                 uint32_t vreg,
                                                 size_t logical_size) {
    int32_t alloca_off;
    size_t slot_sz;
    if (!ctx || logical_size <= 8)
        return false;
    alloca_off = lr_target_lookup_static_alloca_offset(
        ctx->static_alloca_offsets, ctx->num_static_alloca_offsets, vreg);
    if (alloca_off != 0)
        return false;
    slot_sz = vreg_slot_size(ctx, vreg);
    return slot_sz == 8;
}

static void emit_copy_vreg_value_bytes_to_base(a64_compile_ctx_t *ctx,
                                               uint32_t src_vreg,
                                               size_t value_sz,
                                               uint8_t dst_base,
                                               int32_t dst_disp) {
    int32_t alloca_off;
    int32_t src_off;
    size_t src_sz;
    if (!ctx || value_sz == 0)
        return;

    alloca_off = lr_target_lookup_static_alloca_offset(
        ctx->static_alloca_offsets, ctx->num_static_alloca_offsets, src_vreg);
    if (alloca_off != 0) {
        emit_mem_copy_base_to_base(ctx, dst_base, dst_disp,
                                   A64_FP, alloca_off, value_sz);
        return;
    }

    src_off = alloc_slot(ctx, src_vreg, 8);
    src_sz = vreg_slot_size(ctx, src_vreg);
    if (src_sz >= value_sz) {
        emit_mem_copy_base_to_base(ctx, dst_base, dst_disp,
                                   A64_FP, src_off, value_sz);
        return;
    }

    if (src_sz == 8 && value_sz > 8) {
        emit_load(ctx->buf, &ctx->pos, ctx->buflen, A64_X10, A64_FP, src_off, 8);
        emit_mem_copy_base_to_base(ctx, dst_base, dst_disp,
                                   A64_X10, 0, value_sz);
        return;
    }

    if (src_sz > 0) {
        emit_mem_copy_base_to_base(ctx, dst_base, dst_disp,
                                   A64_FP, src_off, src_sz);
    }
    if (src_sz < value_sz) {
        emit_mem_zero_base(ctx, dst_base, dst_disp + (int32_t)src_sz,
                           value_sz - src_sz);
    }
}

static void emit_phi_copy_value(a64_compile_ctx_t *cc,
                                uint32_t dest_vreg,
                                const lr_operand_t *src_op) {
    size_t dst_sz;
    int32_t dst_off;
    if (!cc || !src_op)
        return;
    dst_sz = vreg_slot_size(cc, dest_vreg);
    if (dst_sz <= 8) {
        emit_load_operand(cc, src_op, A64_X9);
        emit_store_slot(cc, dest_vreg, A64_X9);
        return;
    }

    dst_off = alloc_slot(cc, dest_vreg, dst_sz);
    if (src_op->kind == LR_VAL_VREG) {
        emit_copy_vreg_value_bytes_to_base(cc, src_op->vreg, dst_sz,
                                           A64_FP, dst_off);
        return;
    }
    if (src_op->kind == LR_VAL_UNDEF || src_op->kind == LR_VAL_NULL) {
        emit_mem_zero_base(cc, A64_FP, dst_off, dst_sz);
        return;
    }

    emit_load_operand(cc, src_op, A64_X9);
    emit_store(cc->buf, &cc->pos, cc->buflen, A64_X9, A64_FP, dst_off, 8);
    emit_mem_zero_base(cc, A64_FP, dst_off + 8, dst_sz - 8);
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

/* ---- Streaming direct-emission ISel ------------------------------------ */

typedef struct a64_stream_phi_copy {
    uint32_t pred_block_id;
    uint32_t succ_block_id;
    uint32_t dest_vreg;
    lr_operand_t src_op;
    bool emitted;
} a64_stream_phi_copy_t;

typedef struct a64_deferred_term {
    bool pending;
    lr_opcode_t op;
    lr_type_t *type;
    uint32_t dest;
    lr_operand_t ops[4];
    uint32_t num_ops;
    uint32_t block_id;
} a64_deferred_term_t;

typedef struct a64_direct_ctx {
    a64_compile_ctx_t cc;
    size_t prologue_patch_pos;
    lr_compile_mode_t mode;
    uint32_t current_block_id;
    bool has_current_block;
    bool block_offset_pending;
    uint32_t next_vreg;
    lr_type_t *ret_type;
    a64_stream_phi_copy_t *phi_copies;
    uint32_t phi_copy_count;
    uint32_t phi_copy_cap;
    a64_deferred_term_t deferred;
} a64_direct_ctx_t;

static lr_operand_t a64_operand_from_desc(const lr_operand_desc_t *desc) {
    lr_operand_t out;
    memset(&out, 0, sizeof(out));
    if (!desc) {
        out.kind = LR_VAL_UNDEF;
        return out;
    }
    out.kind = (lr_operand_kind_t)desc->kind;
    out.type = desc->type;
    out.global_offset = desc->global_offset;
    switch (desc->kind) {
    case LR_OP_KIND_VREG:
        out.vreg = desc->vreg;
        break;
    case LR_OP_KIND_IMM_I64:
        out.imm_i64 = desc->imm_i64;
        break;
    case LR_OP_KIND_IMM_F64:
        out.imm_f64 = desc->imm_f64;
        break;
    case LR_OP_KIND_BLOCK:
        out.block_id = desc->block_id;
        break;
    case LR_OP_KIND_GLOBAL:
        out.global_id = desc->global_id;
        break;
    default:
        break;
    }
    return out;
}

static void a64_direct_note_vregs(a64_direct_ctx_t *ctx,
                                  const lr_compile_inst_desc_t *desc) {
    if (desc->dest != 0 && desc->dest >= ctx->next_vreg)
        ctx->next_vreg = desc->dest + 1u;
    for (uint32_t i = 0; i < desc->num_operands; i++) {
        if (desc->operands[i].kind == LR_OP_KIND_VREG &&
            desc->operands[i].vreg >= ctx->next_vreg)
            ctx->next_vreg = desc->operands[i].vreg + 1u;
    }
}

static int a64_direct_ensure_fixup_cap(a64_direct_ctx_t *ctx) {
    a64_compile_ctx_t *cc = &ctx->cc;
    if (cc->num_fixups < cc->fixup_cap)
        return 0;
    uint32_t new_cap = cc->fixup_cap == 0 ? 16u : cc->fixup_cap * 2u;
    a64_fixup_t *nf = lr_arena_array_uninit(cc->arena, a64_fixup_t, new_cap);
    if (!nf)
        return -1;
    if (cc->fixup_cap > 0)
        memcpy(nf, cc->fixups, sizeof(a64_fixup_t) * cc->fixup_cap);
    cc->fixups = nf;
    cc->fixup_cap = new_cap;
    return 0;
}

static int a64_direct_ensure_block_offsets(a64_direct_ctx_t *ctx,
                                           uint32_t block_id) {
    a64_compile_ctx_t *cc = &ctx->cc;
    if (block_id < cc->num_block_offsets)
        return 0;
    uint32_t new_cap = cc->num_block_offsets == 0 ? 8u : cc->num_block_offsets;
    while (new_cap <= block_id)
        new_cap *= 2u;
    size_t *nb = lr_arena_array_uninit(cc->arena, size_t, new_cap);
    size_t *ne = lr_arena_array_uninit(cc->arena, size_t, new_cap);
    if (!nb || !ne)
        return -1;
    if (cc->num_block_offsets > 0) {
        memcpy(nb, cc->block_offsets,
               sizeof(size_t) * cc->num_block_offsets);
        memcpy(ne, cc->block_entry_offsets,
               sizeof(size_t) * cc->num_block_offsets);
    }
    for (uint32_t i = cc->num_block_offsets; i < new_cap; i++)
        nb[i] = SIZE_MAX;
    for (uint32_t i = cc->num_block_offsets; i < new_cap; i++)
        ne[i] = SIZE_MAX;
    cc->block_offsets = nb;
    cc->block_entry_offsets = ne;
    cc->num_block_offsets = new_cap;
    return 0;
}

static int a64_direct_ensure_phi_copy_cap(a64_direct_ctx_t *ctx) {
    if (ctx->phi_copy_count < ctx->phi_copy_cap)
        return 0;
    uint32_t new_cap = ctx->phi_copy_cap == 0 ? 8u : ctx->phi_copy_cap * 2u;
    a64_stream_phi_copy_t *np = lr_arena_array_uninit(
        ctx->cc.arena, a64_stream_phi_copy_t, new_cap);
    if (!np)
        return -1;
    if (ctx->phi_copy_cap > 0)
        memcpy(np, ctx->phi_copies,
               sizeof(a64_stream_phi_copy_t) * ctx->phi_copy_cap);
    ctx->phi_copies = np;
    ctx->phi_copy_cap = new_cap;
    return 0;
}

static void a64_direct_emit_phi_copies_for_edge(a64_direct_ctx_t *ctx,
                                                uint32_t pred,
                                                uint32_t succ) {
    a64_compile_ctx_t *cc = &ctx->cc;
    uint32_t stage_base = ctx->next_vreg;
    uint32_t staged = 0;

    /* PHI inputs are parallel: stage sources first, then write destinations. */
    for (uint32_t i = 0; i < ctx->phi_copy_count; i++) {
        size_t dst_sz;
        uint32_t tmp_vreg;
        int32_t tmp_off;
        const lr_operand_t *src_op;
        if (ctx->phi_copies[i].pred_block_id != pred ||
            ctx->phi_copies[i].succ_block_id != succ)
            continue;
        src_op = &ctx->phi_copies[i].src_op;
        dst_sz = vreg_slot_size(cc, ctx->phi_copies[i].dest_vreg);
        if (dst_sz < 8)
            dst_sz = 8;
        if (getenv("LIRIC_DBG_A64_PHI") != NULL) {
            fprintf(stderr,
                    "[a64 phi edge] func=%s pred=%u succ=%u phase=stage dest=%u src_kind=%d src_vreg=%u\n",
                    cc->func_name ? cc->func_name : "<anon>",
                    pred, succ,
                    ctx->phi_copies[i].dest_vreg,
                    (int)ctx->phi_copies[i].src_op.kind,
                    ctx->phi_copies[i].src_op.kind == LR_VAL_VREG
                        ? ctx->phi_copies[i].src_op.vreg
                        : 0u);
        }
        tmp_vreg = stage_base + staged;
        ctx->next_vreg = tmp_vreg + 1u;
        tmp_off = alloc_slot(cc, tmp_vreg, dst_sz);
        if (dst_sz <= 8) {
            emit_load_operand(cc, src_op, A64_X9);
            emit_store_slot(cc, tmp_vreg, A64_X9);
        } else if (src_op->kind == LR_VAL_VREG) {
            emit_copy_vreg_value_bytes_to_base(cc, src_op->vreg, dst_sz,
                                               A64_FP, tmp_off);
        } else if (src_op->kind == LR_VAL_UNDEF ||
                   src_op->kind == LR_VAL_NULL) {
            emit_mem_zero_base(cc, A64_FP, tmp_off, dst_sz);
        } else {
            emit_load_operand(cc, src_op, A64_X9);
            emit_store(cc->buf, &cc->pos, cc->buflen, A64_X9,
                       A64_FP, tmp_off, 8);
            emit_mem_zero_base(cc, A64_FP, tmp_off + 8, dst_sz - 8);
        }
        staged++;
    }

    if (staged == 0)
        return;

    staged = 0;
    for (uint32_t i = 0; i < ctx->phi_copy_count; i++) {
        lr_operand_t staged_src;
        if (ctx->phi_copies[i].pred_block_id != pred ||
            ctx->phi_copies[i].succ_block_id != succ)
            continue;
        if (getenv("LIRIC_DBG_A64_PHI") != NULL) {
            fprintf(stderr,
                    "[a64 phi edge] func=%s pred=%u succ=%u phase=apply dest=%u staged=%u\n",
                    cc->func_name ? cc->func_name : "<anon>",
                    pred, succ,
                    ctx->phi_copies[i].dest_vreg,
                    stage_base + staged);
        }
        memset(&staged_src, 0, sizeof(staged_src));
        staged_src.kind = LR_VAL_VREG;
        staged_src.type = ctx->phi_copies[i].src_op.type;
        staged_src.vreg = stage_base + staged;
        emit_phi_copy_value(cc, ctx->phi_copies[i].dest_vreg, &staged_src);
        ctx->phi_copies[i].emitted = true;
        staged++;
    }
}

static int a64_flush_deferred_terminator(a64_direct_ctx_t *ctx) {
    a64_compile_ctx_t *cc;
    a64_deferred_term_t *dt;
    bool dbg_term;

    if (!ctx || !ctx->deferred.pending)
        return 0;

    cc = &ctx->cc;
    dt = &ctx->deferred;
    dbg_term = getenv("LIRIC_DBG_A64_TERM") != NULL;
    dt->pending = false;

    switch (dt->op) {
    case LR_OP_RET:
        if (cc->func_uses_fp_abi && is_fp_abi_type(ctx->ret_type)) {
            emit_load_fp_operand(cc, &dt->ops[0], A64_D0,
                                 fp_abi_size(ctx->ret_type));
        } else {
            emit_load_operand(cc, &dt->ops[0], A64_X9);
            emit_mov_reg(cc->buf, &cc->pos, cc->buflen, A64_X0, A64_X9,
                         true);
        }
        emit_epilogue_a64(cc);
        break;
    case LR_OP_RET_VOID:
        emit_epilogue_a64(cc);
        break;
    case LR_OP_BR: {
        if (dbg_term) {
            fprintf(stderr,
                    "[a64 term] func=%s op=br pred=%u succ=%u\n",
                    cc->func_name ? cc->func_name : "<anon>",
                    dt->block_id, dt->ops[0].block_id);
        }
        a64_direct_emit_phi_copies_for_edge(ctx, dt->block_id,
                                            dt->ops[0].block_id);
        if (a64_direct_ensure_fixup_cap(ctx) != 0) return -1;
        emit_jmp_a64(cc, dt->ops[0].block_id);
        cc->fixups[cc->num_fixups - 1].source = dt->block_id;
        break;
    }
    case LR_OP_CONDBR: {
        size_t true_path_pos;
        size_t jcc_insn_pos;
        int64_t jcc_imm;
        uint8_t cond;
        uint32_t true_id;
        uint32_t false_id;
        emit_load_operand(cc, &dt->ops[0], A64_X9);
        emit_u32(cc->buf, &cc->pos, cc->buflen,
                 enc_ands_reg(false, A64_X9, A64_X9));
        true_id = dt->ops[1].block_id;
        false_id = dt->ops[2].block_id;
        if (dbg_term) {
            fprintf(stderr,
                    "[a64 term] func=%s op=condbr pred=%u t=%u f=%u\n",
                    cc->func_name ? cc->func_name : "<anon>",
                    dt->block_id, true_id, false_id);
        }

        /* Emit edge-specific copies:
           cmp; b.ne true_path; false_copies; b false; true_path: true_copies; b true */
        cond = lr_cc_to_a64(LR_CC_NE);
        jcc_insn_pos = cc->pos;
        emit_u32(cc->buf, &cc->pos, cc->buflen, 0x54000000u | cond);

        a64_direct_emit_phi_copies_for_edge(ctx, dt->block_id, false_id);
        if (a64_direct_ensure_fixup_cap(ctx) != 0) return -1;
        emit_jmp_a64(cc, false_id);
        cc->fixups[cc->num_fixups - 1].source = dt->block_id;

        true_path_pos = cc->pos;
        jcc_imm = ((int64_t)true_path_pos - (int64_t)jcc_insn_pos) / 4;
        patch_u32(cc->buf, cc->buflen, jcc_insn_pos,
                  0x54000000u |
                  (((uint32_t)jcc_imm & 0x7FFFFu) << 5) |
                  cond);

        a64_direct_emit_phi_copies_for_edge(ctx, dt->block_id, true_id);
        if (a64_direct_ensure_fixup_cap(ctx) != 0) return -1;
        emit_jmp_a64(cc, true_id);
        cc->fixups[cc->num_fixups - 1].source = dt->block_id;
        break;
    }
    default:
        break;
    }
    return 0;
}

static int aarch64_compile_begin(void **compile_ctx,
                                 const lr_compile_func_meta_t *func_meta,
                                 lr_module_t *mod,
                                 uint8_t *buf, size_t buflen,
                                 lr_arena_t *arena) {
    static const uint8_t param_regs[] = {
        A64_X0, A64_X1, A64_X2, A64_X3, A64_X4, A64_X5, A64_X6, A64_X7
    };
    a64_direct_ctx_t *ctx = NULL;
    uint32_t initial_slots;
    uint32_t *param_vregs = NULL;
    uint32_t num_params;
    lr_type_t *ret_type;

    if (!compile_ctx || !func_meta || !mod || !arena)
        return -1;

    ctx = lr_arena_new(arena, a64_direct_ctx_t);
    if (!ctx)
        return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->mode = func_meta->mode;
    ctx->next_vreg = func_meta->next_vreg;
    ret_type = func_meta->ret_type ? func_meta->ret_type : mod->type_void;
    ctx->ret_type = ret_type;
    num_params = func_meta->num_params;

    if (func_meta->func && func_meta->func->param_vregs) {
        param_vregs = func_meta->func->param_vregs;
    } else if (num_params > 0) {
        param_vregs = lr_arena_array(arena, uint32_t, num_params);
        if (!param_vregs) return -1;
        for (uint32_t i = 0; i < num_params; i++) {
            param_vregs[i] = i + 1u;
            if (ctx->next_vreg <= i + 1u)
                ctx->next_vreg = i + 2u;
        }
    }

    initial_slots = ctx->next_vreg > 64 ? ctx->next_vreg : 64;
    a64_compile_ctx_t *cc = &ctx->cc;
    cc->buf = buf;
    cc->buflen = buflen;
    cc->pos = 0;
    cc->stack_size = 0;
    cc->stack_slots = lr_arena_array(arena, int32_t, initial_slots);
    cc->stack_slot_sizes = lr_arena_array(arena, uint32_t, initial_slots);
    cc->num_stack_slots = initial_slots;
    cc->static_alloca_offsets = NULL;
    cc->num_static_alloca_offsets = 0;
    cc->block_offsets = lr_arena_array_uninit(arena, size_t, 8);
    cc->block_entry_offsets = lr_arena_array_uninit(arena, size_t, 8);
    cc->num_block_offsets = 8;
    for (uint32_t i = 0; i < 8; i++) cc->block_offsets[i] = SIZE_MAX;
    for (uint32_t i = 0; i < 8; i++) cc->block_entry_offsets[i] = SIZE_MAX;
    cc->fixups = lr_arena_array_uninit(arena, a64_fixup_t, 16);
    cc->num_fixups = 0;
    cc->fixup_cap = 16;
    cc->arena = arena;
    cc->obj_ctx = mod ? mod->obj_ctx : NULL;
    cc->mod = mod;
    cc->sym_defined = NULL;
    cc->sym_funcs = NULL;
    cc->sym_count = 0;
    cc->x9_holds_vreg = UINT32_MAX;
    cc->x10_holds_vreg = UINT32_MAX;
    cc->func_uses_fp_abi = func_meta->func && func_meta->func->uses_llvm_abi;
    cc->func_is_vararg = func_meta->vararg;
    cc->vararg_stack_start_off = 16;
    cc->func_name = (func_meta->func && func_meta->func->name)
                        ? func_meta->func->name
                        : "<anon>";

    attach_obj_symbol_meta_cache(cc);

    if (cc->func_is_vararg) {
        uint32_t stack_used = 0;
        if (cc->func_uses_fp_abi) {
            lr_type_t **param_types_local = func_meta->param_types;
            uint32_t gp_used_local = 0;
            uint32_t fp_used_local = 0;
            for (uint32_t i = 0; i < num_params; i++) {
                const lr_type_t *pty = param_types_local ? param_types_local[i] : NULL;
                if (is_fp_abi_type(pty) && fp_used_local < 8) {
                    fp_used_local++;
                } else if (!is_fp_abi_type(pty) && gp_used_local < 8) {
                    gp_used_local++;
                } else {
                    stack_used++;
                }
            }
        } else if (num_params > 8) {
            stack_used = num_params - 8;
        }
        cc->vararg_stack_start_off = 16 + (int32_t)(stack_used * 8u);
    }

    ctx->prologue_patch_pos = emit_prologue_a64(cc);

    if (cc->func_uses_fp_abi) {
        static const uint8_t param_fp_regs[] = {
            A64_D0, A64_D1, A64_D2, A64_D3,
            A64_D4, A64_D5, A64_D6, A64_D7
        };
        lr_type_t **param_types = func_meta->param_types;
        uint32_t gp_used = 0;
        uint32_t fp_used = 0;
        uint32_t stack_used = 0;
        for (uint32_t i = 0; i < num_params; i++) {
            const lr_type_t *pty = param_types ? param_types[i] : NULL;
            if (is_fp_abi_type(pty) && fp_used < 8) {
                if (getenv("LIRIC_DBG_A64_PARAMS")) {
                    fprintf(stderr,
                            "[a64 param] func=%s idx=%u vreg=%u src=fpr%u ty=%d\n",
                            cc->func_name ? cc->func_name : "<anon>",
                            i, param_vregs[i], fp_used,
                            pty ? (int)pty->kind : -1);
                }
                emit_store_fp_slot(cc, param_vregs[i],
                                   param_fp_regs[fp_used],
                                   fp_abi_size(pty));
                fp_used++;
            } else if (!is_fp_abi_type(pty) && gp_used < 8) {
                if (getenv("LIRIC_DBG_A64_PARAMS")) {
                    fprintf(stderr,
                            "[a64 param] func=%s idx=%u vreg=%u src=gpr%u ty=%d\n",
                            cc->func_name ? cc->func_name : "<anon>",
                            i, param_vregs[i], gp_used,
                            pty ? (int)pty->kind : -1);
                }
                emit_store_slot(cc, param_vregs[i], param_regs[gp_used]);
                gp_used++;
            } else {
                int32_t caller_off = 16 + (int32_t)(stack_used * 8u);
                if (is_fp_abi_type(pty)) {
                    uint8_t fsize = fp_abi_size(pty);
                    emit_fp_load(cc->buf, &cc->pos, cc->buflen,
                                 FP_SCRATCH0, A64_FP, caller_off, fsize);
                    emit_store_fp_slot(cc, param_vregs[i], FP_SCRATCH0,
                                       fsize);
                } else {
                    emit_load(cc->buf, &cc->pos, cc->buflen, A64_X9,
                              A64_FP, caller_off, 8);
                    emit_store_slot(cc, param_vregs[i], A64_X9);
                }
                stack_used++;
            }
        }
    } else {
        for (uint32_t i = 0; i < num_params && i < 8; i++)
            emit_store_slot(cc, param_vregs[i], param_regs[i]);
        for (uint32_t i = 8; i < num_params; i++) {
            int32_t caller_off = 16 + (int32_t)(i - 8) * 8;
            emit_load(cc->buf, &cc->pos, cc->buflen, A64_X9, A64_FP,
                      caller_off, 8);
            emit_store_slot(cc, param_vregs[i], A64_X9);
        }
    }

    *compile_ctx = ctx;
    return 0;
}

static int aarch64_compile_set_block(void *compile_ctx, uint32_t block_id) {
    a64_direct_ctx_t *ctx = (a64_direct_ctx_t *)compile_ctx;
    if (!ctx)
        return -1;
    /* Flush deferred terminators on block transitions so branch fixups
       are emitted before we bind the next block entry. This also ensures
       empty blocks receive concrete offsets and don't leave unresolved
       placeholder branches (which encode as self-loops on AArch64). */
    if (ctx->deferred.pending &&
        (!ctx->has_current_block || ctx->deferred.block_id != block_id)) {
        if (a64_flush_deferred_terminator(ctx) != 0)
            return -1;
    }
    if (a64_direct_ensure_block_offsets(ctx, block_id) != 0)
        return -1;
    ctx->current_block_id = block_id;
    ctx->has_current_block = true;
    if (ctx->cc.block_offsets[block_id] == SIZE_MAX) {
        ctx->cc.block_offsets[block_id] = ctx->cc.pos;
        ctx->cc.block_entry_offsets[block_id] = ctx->cc.pos;
    }
    if (getenv("LIRIC_DBG_A64_BLOCKS") != NULL) {
        fprintf(stderr,
                "[a64 block] func=%s block=%u pos=%zu off=%zu entry=%zu\n",
                ctx->cc.func_name ? ctx->cc.func_name : "<anon>",
                block_id,
                ctx->cc.pos,
                ctx->cc.block_offsets[block_id],
                ctx->cc.block_entry_offsets[block_id]);
    }
    /* Entering a new block must invalidate cached register mappings before
       emitting non-PHI instructions, but keep offsets bound for empty blocks. */
    ctx->block_offset_pending = true;
    return 0;
}

static int aarch64_compile_emit(void *compile_ctx,
                                const lr_compile_inst_desc_t *desc) {
    a64_direct_ctx_t *ctx = (a64_direct_ctx_t *)compile_ctx;
    a64_compile_ctx_t *cc;
    lr_operand_t ops[16];
    lr_operand_t *ops_ptr = ops;
    lr_inst_t inst_header;

    if (!ctx || !desc || !ctx->has_current_block)
        return -1;
    if (desc->num_operands > 0 && !desc->operands)
        return -1;
    if (desc->num_indices > 0 && !desc->indices)
        return -1;

    /* Keep same-block allocas before the deferred terminator so entry-block
       stack setup is not split by an inserted branch. */
    if (desc->op != LR_OP_PHI) {
        if (ctx->deferred.pending &&
            (desc->op != LR_OP_ALLOCA ||
             ctx->deferred.block_id != ctx->current_block_id)) {
            if (a64_flush_deferred_terminator(ctx) != 0)
                return -1;
        }
        if (ctx->block_offset_pending) {
            invalidate_cached_gprs_a64(&ctx->cc);
        }
        ctx->block_offset_pending = false;
    }

    cc = &ctx->cc;
    a64_direct_note_vregs(ctx, desc);

    if (a64_direct_ensure_fixup_cap(ctx) != 0)
        return -1;

    uint32_t nops = desc->num_operands;
    {
        const char *watch_env = getenv("LIRIC_DBG_A64_WATCH_VREG");
        if (watch_env && watch_env[0] != '\0') {
            uint32_t watch = (uint32_t)strtoul(watch_env, NULL, 10);
            bool hit = (desc->dest == watch);
            if (!hit) {
                for (uint32_t i = 0; i < nops; i++) {
                    if (desc->operands[i].kind == LR_OP_KIND_VREG &&
                        desc->operands[i].vreg == watch) {
                        hit = true;
                        break;
                    }
                }
            }
            if (hit) {
                fprintf(stderr,
                        "[a64 watch] func=%s block=%u op=%d dest=%u nops=%u",
                        cc->func_name ? cc->func_name : "<anon>",
                        ctx->current_block_id,
                        (int)desc->op,
                        desc->dest,
                        nops);
                for (uint32_t i = 0; i < nops; i++) {
                    if (desc->operands[i].kind == LR_OP_KIND_VREG) {
                        fprintf(stderr, " op%u=v%u", i, desc->operands[i].vreg);
                    } else {
                        fprintf(stderr, " op%u=k%d", i, (int)desc->operands[i].kind);
                    }
                }
                fprintf(stderr, "\n");
            }
        }
    }

    if (nops > 16) {
        ops_ptr = lr_arena_array_uninit(cc->arena, lr_operand_t, nops);
        if (!ops_ptr)
            return -1;
        for (uint32_t i = 0; i < nops; i++)
            ops_ptr[i] = a64_operand_from_desc(&desc->operands[i]);
        memset(&inst_header, 0, sizeof(inst_header));
        inst_header.op = desc->op;
        inst_header.operands = ops_ptr;
        inst_header.num_operands = nops;
        inst_header.type = desc->type;
        inst_header.dest = desc->dest;
        inst_header.icmp_pred = (lr_icmp_pred_t)desc->icmp_pred;
        inst_header.fcmp_pred = (lr_fcmp_pred_t)desc->fcmp_pred;
        inst_header.call_external_abi = desc->call_external_abi;
        inst_header.call_vararg = desc->call_vararg;
        inst_header.call_fixed_args = desc->call_fixed_args;
        inst_header.indices = (uint32_t *)desc->indices;
        inst_header.num_indices = desc->num_indices;
    } else {
        for (uint32_t i = 0; i < nops; i++)
            ops_ptr[i] = a64_operand_from_desc(&desc->operands[i]);
        memset(&inst_header, 0, sizeof(inst_header));
        inst_header.op = desc->op;
    }

    switch (desc->op) {
    case LR_OP_RET: {
        ctx->deferred.pending = true;
        ctx->deferred.op = LR_OP_RET;
        ctx->deferred.ops[0] = ops[0];
        ctx->deferred.num_ops = 1;
        ctx->deferred.block_id = ctx->current_block_id;
        return 0;
    }
    case LR_OP_RET_VOID:
        ctx->deferred.pending = true;
        ctx->deferred.op = LR_OP_RET_VOID;
        ctx->deferred.num_ops = 0;
        ctx->deferred.block_id = ctx->current_block_id;
        return 0;
    case LR_OP_ADD: case LR_OP_SUB: case LR_OP_AND:
    case LR_OP_OR: case LR_OP_XOR: {
        emit_load_operand(cc, &ops[0], A64_X9);
        emit_load_operand(cc, &ops[1], A64_X10);
        bool is64 = lr_type_size(desc->type) > 4;
        switch (desc->op) {
        case LR_OP_ADD:
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_add_reg(is64, A64_X9, A64_X9, A64_X10));
            break;
        case LR_OP_SUB:
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_sub_reg(is64, A64_X9, A64_X9, A64_X10));
            break;
        case LR_OP_AND:
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_logic_reg(0x8A000000u, is64, A64_X9, A64_X9, A64_X10));
            break;
        case LR_OP_OR:
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_logic_reg(0xAA000000u, is64, A64_X9, A64_X9, A64_X10));
            break;
        case LR_OP_XOR:
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_logic_reg(0xCA000000u, is64, A64_X9, A64_X9, A64_X10));
            break;
        default: break;
        }
        emit_store_slot(cc, desc->dest, A64_X9);
        break;
    }
    case LR_OP_MUL: {
        emit_load_operand(cc, &ops[0], A64_X9);
        emit_load_operand(cc, &ops[1], A64_X10);
        bool is64 = lr_type_size(desc->type) > 4;
        emit_u32(cc->buf, &cc->pos, cc->buflen,
                 enc_mul(is64, A64_X9, A64_X9, A64_X10));
        emit_store_slot(cc, desc->dest, A64_X9);
        break;
    }
    case LR_OP_FADD: case LR_OP_FSUB:
    case LR_OP_FMUL: case LR_OP_FDIV: {
        uint8_t fsize = (desc->type &&
                         desc->type->kind == LR_TYPE_FLOAT) ? 4 : 8;
        emit_load_fp_operand(cc, &ops[0], FP_SCRATCH0, fsize);
        emit_load_fp_operand(cc, &ops[1], FP_SCRATCH1, fsize);
        switch (desc->op) {
        case LR_OP_FADD:
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_fadd(fsize, FP_SCRATCH0, FP_SCRATCH0, FP_SCRATCH1));
            break;
        case LR_OP_FSUB:
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_fsub(fsize, FP_SCRATCH0, FP_SCRATCH0, FP_SCRATCH1));
            break;
        case LR_OP_FMUL:
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_fmul(fsize, FP_SCRATCH0, FP_SCRATCH0, FP_SCRATCH1));
            break;
        case LR_OP_FDIV:
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_fdiv(fsize, FP_SCRATCH0, FP_SCRATCH0, FP_SCRATCH1));
            break;
        default: break;
        }
        emit_store_fp_slot(cc, desc->dest, FP_SCRATCH0, fsize);
        break;
    }
    case LR_OP_FNEG: {
        uint8_t fsize = (desc->type &&
                         desc->type->kind == LR_TYPE_FLOAT) ? 4 : 8;
        emit_load_fp_operand(cc, &ops[0], FP_SCRATCH0, fsize);
        emit_u32(cc->buf, &cc->pos, cc->buflen,
                 enc_fneg(fsize, FP_SCRATCH0, FP_SCRATCH0));
        emit_store_fp_slot(cc, desc->dest, FP_SCRATCH0, fsize);
        break;
    }
    case LR_OP_SDIV: case LR_OP_SREM: {
        emit_load_operand(cc, &ops[0], A64_X9);
        emit_load_operand(cc, &ops[1], A64_X10);
        bool is64 = lr_type_size(desc->type) > 4;
        emit_mov_reg(cc->buf, &cc->pos, cc->buflen, A64_X11, A64_X9, is64);
        emit_u32(cc->buf, &cc->pos, cc->buflen,
                 enc_sdiv(is64, A64_X9, A64_X9, A64_X10));
        emit_u32(cc->buf, &cc->pos, cc->buflen,
                 enc_msub(is64, A64_X11, A64_X9, A64_X10, A64_X11));
        invalidate_cached_reg_a64(cc, A64_X9);
        if (desc->op == LR_OP_SREM)
            emit_store_slot(cc, desc->dest, A64_X11);
        else
            emit_store_slot(cc, desc->dest, A64_X9);
        break;
    }
    case LR_OP_SHL: case LR_OP_LSHR: case LR_OP_ASHR: {
        emit_load_operand(cc, &ops[0], A64_X9);
        emit_load_operand(cc, &ops[1], A64_X10);
        bool is64 = lr_type_size(desc->type) > 4;
        switch (desc->op) {
        case LR_OP_SHL:
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_lslv(is64, A64_X9, A64_X9, A64_X10));
            break;
        case LR_OP_LSHR:
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_lsrv(is64, A64_X9, A64_X9, A64_X10));
            break;
        case LR_OP_ASHR:
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_asrv(is64, A64_X9, A64_X9, A64_X10));
            break;
        default: break;
        }
        emit_store_slot(cc, desc->dest, A64_X9);
        break;
    }
    case LR_OP_ICMP: {
        emit_load_operand(cc, &ops[0], A64_X9);
        emit_load_operand(cc, &ops[1], A64_X10);
        bool is64 = lr_type_size(ops[0].type) > 4;
        emit_u32(cc->buf, &cc->pos, cc->buflen,
                 enc_subs_reg(is64, A64_X9, A64_X10));
        uint8_t icc = lr_target_cc_from_icmp(
            (lr_icmp_pred_t)desc->icmp_pred);
        emit_setcc_a64(cc, icc, A64_X9);
        emit_store_slot(cc, desc->dest, A64_X9);
        break;
    }
    case LR_OP_SELECT: {
        emit_load_operand(cc, &ops[0], A64_X9);
        emit_u32(cc->buf, &cc->pos, cc->buflen,
                 enc_ands_reg(false, A64_X9, A64_X9));
        emit_load_operand(cc, &ops[2], A64_X9);  /* false value */
        emit_load_operand(cc, &ops[1], A64_X10); /* true value */
        uint8_t cond = lr_cc_to_a64(LR_CC_NE);
        emit_u32(cc->buf, &cc->pos, cc->buflen,
                 enc_csel(true, A64_X9, A64_X10, A64_X9, cond));
        emit_store_slot(cc, desc->dest, A64_X9);
        break;
    }
    case LR_OP_BR: {
        ctx->deferred.pending = true;
        ctx->deferred.op = LR_OP_BR;
        ctx->deferred.ops[0] = ops[0];
        ctx->deferred.num_ops = 1;
        ctx->deferred.block_id = ctx->current_block_id;
        return 0;
    }
    case LR_OP_CONDBR: {
        ctx->deferred.pending = true;
        ctx->deferred.op = LR_OP_CONDBR;
        ctx->deferred.ops[0] = ops[0];
        ctx->deferred.ops[1] = ops[1];
        ctx->deferred.ops[2] = ops[2];
        ctx->deferred.num_ops = 3;
        ctx->deferred.block_id = ctx->current_block_id;
        return 0;
    }
    case LR_OP_ALLOCA: {
        size_t elem_sz = lr_type_size(desc->type);
        size_t elem_align = lr_type_align(desc->type);
        if (elem_sz < 1) elem_sz = 1;
        if (elem_align < 8) elem_align = 8;

        bool use_static = (nops == 0) ||
            (ops[0].kind == LR_VAL_IMM_I64);

        if (use_static) {
            int64_t count = (nops > 0) ? ops[0].imm_i64 : 1;
            size_t total_sz;
            int32_t off = lr_target_lookup_static_alloca_offset(
                cc->static_alloca_offsets, cc->num_static_alloca_offsets,
                desc->dest);
            if (count < 1)
                count = 1;
            total_sz = elem_sz * (size_t)count;
            if (off == 0) {
                cc->stack_size = (uint32_t)align_up_size(cc->stack_size,
                                                         elem_align);
                cc->stack_size += (uint32_t)total_sz;
                off = -(int32_t)cc->stack_size;
                lr_target_set_static_alloca_offset(
                    cc->arena, &cc->static_alloca_offsets,
                    &cc->num_static_alloca_offsets, desc->dest, off);
            }
            emit_addr(cc->buf, &cc->pos, cc->buflen, A64_X9, A64_FP, off);
            emit_store_slot(cc, desc->dest, A64_X9);
        } else {
            emit_load_operand(cc, &ops[0], A64_X9);
            if (elem_sz != 1) {
                emit_move_imm_ctx(cc, A64_X10, (int64_t)elem_sz, true);
                emit_u32(cc->buf, &cc->pos, cc->buflen,
                         enc_mul(true, A64_X9, A64_X9, A64_X10));
            }
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_add_imm(true, A64_X9, A64_X9, 15));
            emit_move_imm_ctx(cc, A64_X10, ~15LL, true);
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_logic_reg(0x8A000000u, true, A64_X9, A64_X9, A64_X10));
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_add_imm(true, A64_X14, A64_SP, 0));
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_sub_reg(true, A64_X14, A64_X14, A64_X9));
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_add_imm(true, A64_SP, A64_X14, 0));
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_add_imm(true, A64_X9, A64_SP, 0));
            emit_store_slot(cc, desc->dest, A64_X9);
        }
        break;
    }
    case LR_OP_LOAD: {
        bool dbg_ls = getenv("LIRIC_DBG_A64_LOADSTORE") != NULL;
        {
            const char *watch_env = getenv("LIRIC_DBG_A64_WATCH_VREG");
            if (watch_env && watch_env[0] != '\0') {
                uint32_t watch = (uint32_t)strtoul(watch_env, NULL, 10);
                if (nops > 0 && ops[0].kind == LR_VAL_VREG &&
                    ops[0].vreg == watch) {
                    fprintf(stderr,
                            "[a64 load-op] func=%s block=%u src_vreg=%u src_kind=%d pos=%zu\n",
                            cc->func_name ? cc->func_name : "<anon>",
                            ctx->current_block_id,
                            ops[0].vreg,
                            (int)ops[0].kind,
                            cc->pos);
                }
            }
        }
        emit_load_operand(cc, &ops[0], A64_X9);
        size_t load_sz = lr_type_size(desc->type);
        if (load_sz == 0)
            load_sz = 8;
        if (dbg_ls) {
            fprintf(stderr,
                    "[a64 load] func=%s block=%u dest=%u src_kind=%d src_vreg=%u desc_ty=%d load_sz=%zu\n",
                    cc->func_name ? cc->func_name : "<anon>",
                    ctx->current_block_id,
                    desc->dest,
                    (int)ops[0].kind,
                    ops[0].kind == LR_VAL_VREG ? ops[0].vreg : 0u,
                    desc->type ? (int)desc->type->kind : -1,
                    load_sz);
        }
        if (load_sz > 8) {
            int32_t dst_off = alloc_slot(cc, desc->dest, load_sz);
            emit_mem_copy_base_to_base(cc, A64_FP, dst_off, A64_X9, 0, load_sz);
        } else {
            emit_load(cc->buf, &cc->pos, cc->buflen, A64_X9, A64_X9, 0,
                      (uint8_t)load_sz);
            emit_store_slot(cc, desc->dest, A64_X9);
        }
        break;
    }
    case LR_OP_STORE: {
        bool dbg_ls = getenv("LIRIC_DBG_A64_LOADSTORE") != NULL;
        {
            const char *watch_env = getenv("LIRIC_DBG_A64_WATCH_VREG");
            if (watch_env && watch_env[0] != '\0') {
                uint32_t watch = (uint32_t)strtoul(watch_env, NULL, 10);
                if (nops > 1 && ops[1].kind == LR_VAL_VREG &&
                    ops[1].vreg == watch) {
                    fprintf(stderr,
                            "[a64 store-op] func=%s block=%u ptr_vreg=%u ptr_kind=%d pos=%zu\n",
                            cc->func_name ? cc->func_name : "<anon>",
                            ctx->current_block_id,
                            ops[1].vreg,
                            (int)ops[1].kind,
                            cc->pos);
                }
            }
        }
        emit_load_operand(cc, &ops[1], A64_X10);
        size_t store_sz = lr_type_size(ops[0].type);
        if (store_sz == 0)
            store_sz = 8;
        if (dbg_ls) {
            fprintf(stderr,
                    "[a64 store] func=%s block=%u src_kind=%d src_vreg=%u src_ty=%d ptr_kind=%d ptr_vreg=%u ptr_ty=%d store_sz=%zu\n",
                    cc->func_name ? cc->func_name : "<anon>",
                    ctx->current_block_id,
                    (int)ops[0].kind,
                    ops[0].kind == LR_VAL_VREG ? ops[0].vreg : 0u,
                    ops[0].type ? (int)ops[0].type->kind : -1,
                    (int)ops[1].kind,
                    ops[1].kind == LR_VAL_VREG ? ops[1].vreg : 0u,
                    ops[1].type ? (int)ops[1].type->kind : -1,
                    store_sz);
        }
        if (store_sz > 8) {
            if (ops[0].kind == LR_VAL_IMM_I64 && ops[0].imm_i64 == 0) {
                emit_mem_zero_base(cc, A64_X10, 0, store_sz);
                break;
            }
            if (ops[0].kind == LR_VAL_VREG) {
                emit_copy_vreg_value_bytes_to_base(cc, ops[0].vreg, store_sz,
                                                   A64_X10, 0);
                break;
            }
            emit_mem_zero_base(cc, A64_X10, 0, store_sz);
            break;
        }
        emit_load_operand(cc, &ops[0], A64_X9);
        emit_store(cc->buf, &cc->pos, cc->buflen, A64_X9, A64_X10, 0,
                   (uint8_t)store_sz);
        break;
    }
    case LR_OP_GEP: {
        emit_load_operand(cc, &ops[0], A64_X9);
        const lr_type_t *cur_ty = desc->type;
        for (uint32_t idx = 1; idx < nops; idx++) {
            lr_gep_step_t step;
            if (!lr_gep_analyze_step(cur_ty, idx == 1, &ops[idx], &step))
                continue;
            cur_ty = step.next_type;
            if (step.is_const) {
                if (step.const_byte_offset == 0)
                    continue;
                emit_move_imm_ctx(cc, A64_X10, step.const_byte_offset, true);
                emit_u32(cc->buf, &cc->pos, cc->buflen,
                         enc_add_reg(true, A64_X9, A64_X9, A64_X10));
                continue;
            }
            emit_load_operand(cc, &ops[idx], A64_X10);
            emit_signext_index_reg(cc, A64_X10, step.runtime_signext_bytes);
            if (step.runtime_elem_size != 1) {
                emit_move_imm_ctx(cc, A64_X11, (int64_t)step.runtime_elem_size, true);
                emit_u32(cc->buf, &cc->pos, cc->buflen,
                         enc_mul(true, A64_X10, A64_X10, A64_X11));
            }
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_add_reg(true, A64_X9, A64_X9, A64_X10));
        }
        emit_store_slot(cc, desc->dest, A64_X9);
        break;
    }
    case LR_OP_SEXT: {
        emit_load_operand(cc, &ops[0], A64_X9);
        uint8_t src_bits = int_type_width_bits(ops[0].type);
        if (src_bits > 0 && src_bits < 64) {
            uint8_t shift = (uint8_t)(64 - src_bits);
            emit_move_imm_ctx(cc, A64_X10, (int64_t)shift, true);
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_lslv(true, A64_X9, A64_X9, A64_X10));
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_asrv(true, A64_X9, A64_X9, A64_X10));
        }
        emit_store_slot(cc, desc->dest, A64_X9);
        break;
    }
    case LR_OP_ZEXT: {
        emit_load_operand(cc, &ops[0], A64_X9);
        uint8_t src_bits = int_type_width_bits(ops[0].type);
        if (src_bits > 0 && src_bits < 64) {
            uint8_t shift = (uint8_t)(64 - src_bits);
            emit_move_imm_ctx(cc, A64_X10, (int64_t)shift, true);
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_lslv(true, A64_X9, A64_X9, A64_X10));
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_lsrv(true, A64_X9, A64_X9, A64_X10));
        }
        emit_store_slot(cc, desc->dest, A64_X9);
        break;
    }
    case LR_OP_TRUNC: {
        emit_load_operand(cc, &ops[0], A64_X9);
        uint8_t dst_bits = int_type_width_bits(desc->type);
        if (dst_bits > 0 && dst_bits < 64) {
            uint8_t shift = (uint8_t)(64 - dst_bits);
            emit_move_imm_ctx(cc, A64_X10, (int64_t)shift, true);
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_lslv(true, A64_X9, A64_X9, A64_X10));
            emit_u32(cc->buf, &cc->pos, cc->buflen,
                     enc_lsrv(true, A64_X9, A64_X9, A64_X10));
        }
        emit_store_slot(cc, desc->dest, A64_X9);
        break;
    }
    case LR_OP_BITCAST:
    case LR_OP_PTRTOINT:
    case LR_OP_INTTOPTR:
        emit_load_operand(cc, &ops[0], A64_X9);
        emit_store_slot(cc, desc->dest, A64_X9);
        break;
    case LR_OP_FCMP: {
        uint8_t fsize = (ops[0].type &&
                         ops[0].type->kind == LR_TYPE_FLOAT) ? 4 : 8;
        emit_load_fp_operand(cc, &ops[0], FP_SCRATCH0, fsize);
        emit_load_fp_operand(cc, &ops[1], FP_SCRATCH1, fsize);
        emit_u32(cc->buf, &cc->pos, cc->buflen,
                 enc_fcmp(fsize, FP_SCRATCH0, FP_SCRATCH1));
        uint8_t fcc = lr_target_cc_from_fcmp(
            (lr_fcmp_pred_t)desc->fcmp_pred);
        emit_setcc_a64(cc, fcc, A64_X9);
        emit_store_slot(cc, desc->dest, A64_X9);
        break;
    }
    case LR_OP_SITOFP: {
        uint8_t fsize = (desc->type &&
                         desc->type->kind == LR_TYPE_FLOAT) ? 4 : 8;
        emit_load_operand(cc, &ops[0], A64_X9);
        {
            size_t src_sz = lr_type_size(ops[0].type);
            if (src_sz <= 4) {
                emit_u32(cc->buf, &cc->pos, cc->buflen,
                         0x93407C00u | ((uint32_t)A64_X9 << 5) | A64_X9);
            }
        }
        emit_u32(cc->buf, &cc->pos, cc->buflen,
                 enc_scvtf(fsize, FP_SCRATCH0, A64_X9));
        emit_store_fp_slot(cc, desc->dest, FP_SCRATCH0, fsize);
        break;
    }
    case LR_OP_UITOFP: {
        uint8_t fsize = (desc->type &&
                         desc->type->kind == LR_TYPE_FLOAT) ? 4 : 8;
        emit_load_operand(cc, &ops[0], A64_X9);
        {
            uint8_t src_bits = int_type_width_bits(ops[0].type);
            if (src_bits > 0 && src_bits < 64) {
                uint8_t shift = (uint8_t)(64 - src_bits);
                emit_move_imm_ctx(cc, A64_X10, (int64_t)shift, true);
                emit_u32(cc->buf, &cc->pos, cc->buflen,
                         enc_lslv(true, A64_X9, A64_X9, A64_X10));
                emit_u32(cc->buf, &cc->pos, cc->buflen,
                         enc_lsrv(true, A64_X9, A64_X9, A64_X10));
            }
        }
        emit_u32(cc->buf, &cc->pos, cc->buflen,
                 enc_ucvtf(fsize, FP_SCRATCH0, A64_X9));
        emit_store_fp_slot(cc, desc->dest, FP_SCRATCH0, fsize);
        break;
    }
    case LR_OP_FPTOSI: {
        uint8_t fsize = (ops[0].type &&
                         ops[0].type->kind == LR_TYPE_FLOAT) ? 4 : 8;
        emit_load_fp_operand(cc, &ops[0], FP_SCRATCH0, fsize);
        emit_u32(cc->buf, &cc->pos, cc->buflen,
                 enc_fcvtzs(fsize, A64_X9, FP_SCRATCH0));
        emit_store_slot(cc, desc->dest, A64_X9);
        break;
    }
    case LR_OP_FPTOUI: {
        uint8_t fsize = (ops[0].type &&
                         ops[0].type->kind == LR_TYPE_FLOAT) ? 4 : 8;
        emit_load_fp_operand(cc, &ops[0], FP_SCRATCH0, fsize);
        emit_u32(cc->buf, &cc->pos, cc->buflen,
                 enc_fcvtzu(fsize, A64_X9, FP_SCRATCH0));
        emit_store_slot(cc, desc->dest, A64_X9);
        break;
    }
    case LR_OP_FPEXT: {
        emit_load_fp_operand(cc, &ops[0], FP_SCRATCH0, 4);
        emit_u32(cc->buf, &cc->pos, cc->buflen,
                 enc_fcvt_f32_to_f64(FP_SCRATCH0, FP_SCRATCH0));
        emit_store_fp_slot(cc, desc->dest, FP_SCRATCH0, 8);
        break;
    }
    case LR_OP_FPTRUNC: {
        emit_load_fp_operand(cc, &ops[0], FP_SCRATCH0, 8);
        emit_u32(cc->buf, &cc->pos, cc->buflen,
                 enc_fcvt_f64_to_f32(FP_SCRATCH0, FP_SCRATCH0));
        emit_store_fp_slot(cc, desc->dest, FP_SCRATCH0, 4);
        break;
    }
    case LR_OP_EXTRACTVALUE: {
        size_t field_off = 0;
        const lr_type_t *field_ty = NULL;
        size_t field_sz = 8;
        bool have_path = false;
        size_t agg_sz = 0;

        if (nops > 0 && ops[0].type)
            have_path = lr_aggregate_index_path(
                ops[0].type, desc->indices, desc->num_indices,
                &field_off, &field_ty);
        if (nops > 0 && ops[0].type)
            agg_sz = lr_type_size(ops[0].type);
        if (field_ty)
            field_sz = lr_type_size(field_ty);
        if (field_sz == 0)
            field_sz = 8;

        if (have_path && nops > 0 && ops[0].kind == LR_VAL_VREG) {
            bool src_indirect = vreg_uses_indirect_aggregate_storage(
                cc, ops[0].vreg, agg_sz);
            if (field_sz > 8) {
                int32_t dst_off = alloc_slot(cc, desc->dest, field_sz);
                if (src_indirect) {
                    int32_t src_off = alloc_slot(cc, ops[0].vreg, 8);
                    emit_load(cc->buf, &cc->pos, cc->buflen,
                              A64_X10, A64_FP, src_off, 8);
                    emit_mem_copy_base_to_base(cc, A64_FP, dst_off,
                                               A64_X10, (int32_t)field_off,
                                               field_sz);
                } else {
                    int32_t src_off = alloc_slot(cc, ops[0].vreg, 8) +
                                      (int32_t)field_off;
                    emit_mem_copy_base_to_base(cc, A64_FP, dst_off,
                                               A64_FP, src_off, field_sz);
                }
            } else {
                if (src_indirect) {
                    int32_t src_off = alloc_slot(cc, ops[0].vreg, 8);
                    emit_load(cc->buf, &cc->pos, cc->buflen,
                              A64_X10, A64_FP, src_off, 8);
                    emit_load(cc->buf, &cc->pos, cc->buflen, A64_X9,
                              A64_X10, (int32_t)field_off, (uint8_t)field_sz);
                } else {
                    emit_load_vreg_mem_sized(cc, ops[0].vreg,
                                             (int32_t)field_off, A64_X9,
                                             (uint8_t)field_sz);
                }
                emit_store_slot(cc, desc->dest, A64_X9);
            }
            break;
        }
        if (nops > 0 && (ops[0].kind == LR_VAL_UNDEF ||
                          ops[0].kind == LR_VAL_NULL)) {
            if (field_sz > 8) {
                int32_t dst_off = alloc_slot(cc, desc->dest, field_sz);
                emit_mem_zero_base(cc, A64_FP, dst_off, field_sz);
            } else {
                emit_move_imm_ctx(cc, A64_X9, 0, true);
                emit_store_slot(cc, desc->dest, A64_X9);
            }
            break;
        }
        emit_load_operand(cc, &ops[0], A64_X9);
        emit_store_slot(cc, desc->dest, A64_X9);
        break;
    }
    case LR_OP_INSERTVALUE: {
        size_t agg_sz = desc->type ? lr_type_size(desc->type) : 8;
        size_t field_off = 0;
        const lr_type_t *field_ty = NULL;
        int32_t dst_off;

        if (agg_sz < 8) agg_sz = 8;
        dst_off = alloc_slot(cc, desc->dest, agg_sz);

        if (nops > 0) {
            if (ops[0].kind == LR_VAL_VREG) {
                emit_copy_vreg_value_bytes_to_base(cc, ops[0].vreg, agg_sz,
                                                   A64_FP, dst_off);
            } else if (ops[0].kind == LR_VAL_UNDEF ||
                       ops[0].kind == LR_VAL_NULL) {
                emit_mem_zero_base(cc, A64_FP, dst_off, agg_sz);
            } else if (agg_sz <= 8) {
                emit_load_operand(cc, &ops[0], A64_X9);
                emit_store(cc->buf, &cc->pos, cc->buflen, A64_X9,
                           A64_FP, dst_off, (uint8_t)agg_sz);
            } else {
                emit_mem_zero_base(cc, A64_FP, dst_off, agg_sz);
            }
        }

        if (nops < 2 ||
            !lr_aggregate_index_path(desc->type, desc->indices,
                                     desc->num_indices, &field_off,
                                     &field_ty) ||
            !field_ty)
            break;

        {
            size_t field_sz = lr_type_size(field_ty);
            if (field_sz == 0) break;
            if (field_sz > 8) {
                if (ops[1].kind == LR_VAL_VREG) {
                    emit_copy_vreg_value_bytes_to_base(
                        cc, ops[1].vreg, field_sz, A64_FP,
                        dst_off + (int32_t)field_off);
                } else {
                    emit_mem_zero_base(cc, A64_FP,
                                       dst_off + (int32_t)field_off,
                                       field_sz);
                }
            } else {
                if (ops[1].kind == LR_VAL_UNDEF ||
                    ops[1].kind == LR_VAL_NULL)
                    emit_move_imm_ctx(cc, A64_X9, 0, true);
                else
                    emit_load_operand(cc, &ops[1], A64_X9);
                emit_store(cc->buf, &cc->pos, cc->buflen, A64_X9, A64_FP,
                           dst_off + (int32_t)field_off, (uint8_t)field_sz);
            }
        }
        break;
    }
    case LR_OP_CALL: {
        if (ops_ptr[0].kind == LR_VAL_GLOBAL && cc->mod) {
            const char *cname = lr_module_symbol_name(cc->mod, ops_ptr[0].global_id);
            bool cmp_is64 = true;
            a64_int_cmp_intrinsic_kind_t cmp_intrin =
                classify_llvm_int_cmp_intrinsic(cname, &cmp_is64);
            uint8_t objsize_bits = llvm_objectsize_bits(cname);
            if (cmp_intrin != A64_INT_CMP_INTRIN_NONE) {
                uint8_t cond = 0;
                if (nops >= 3) {
                    emit_load_operand(cc, &ops_ptr[1], A64_X9);
                    emit_load_operand(cc, &ops_ptr[2], A64_X10);
                    emit_u32(cc->buf, &cc->pos, cc->buflen,
                             enc_subs_reg(cmp_is64, A64_X9, A64_X10));
                    switch (cmp_intrin) {
                    case A64_INT_CMP_INTRIN_UMAX: cond = 2; break;  /* hs */
                    case A64_INT_CMP_INTRIN_UMIN: cond = 9; break;  /* ls */
                    case A64_INT_CMP_INTRIN_SMAX: cond = 10; break; /* ge */
                    case A64_INT_CMP_INTRIN_SMIN: cond = 13; break; /* le */
                    default: cond = 0; break;
                    }
                    emit_u32(cc->buf, &cc->pos, cc->buflen,
                             enc_csel(cmp_is64, A64_X9, A64_X9, A64_X10, cond));
                    if (desc->type && desc->type->kind != LR_TYPE_VOID)
                        emit_store_slot(cc, desc->dest, A64_X9);
                }
                invalidate_cached_gprs_a64(cc);
                break;
            }
            if (objsize_bits != 0) {
                if (desc->type && desc->type->kind != LR_TYPE_VOID) {
                    int64_t unknown = (objsize_bits == 32)
                                      ? (int64_t)(uint32_t)UINT32_MAX
                                      : (int64_t)UINT64_MAX;
                    emit_move_imm_ctx(cc, A64_X9, unknown, true);
                    emit_store_slot(cc, desc->dest, A64_X9);
                }
                invalidate_cached_gprs_a64(cc);
                break;
            }
            if (is_llvm_va_start_name(cname)) {
                if (getenv("LIRIC_VERBOSE_VA_LOWER")) {
                    fprintf(stderr,
                            "aarch64 va-lower: %s: va_start stack_off=%d vararg=%d\n",
                            cc->func_name ? cc->func_name : "<anon>",
                            cc->vararg_stack_start_off,
                            cc->func_is_vararg ? 1 : 0);
                }
                if (nops >= 2) {
                    emit_load_operand(cc, &ops_ptr[1], A64_X9);
                    if (cc->func_is_vararg) {
                        emit_addr(cc->buf, &cc->pos, cc->buflen, A64_X10, A64_FP,
                                  cc->vararg_stack_start_off);
                    } else {
                        emit_move_imm_ctx(cc, A64_X10, 0, true);
                    }
                    emit_store(cc->buf, &cc->pos, cc->buflen, A64_X10, A64_X9, 0, 8);
                }
                invalidate_cached_gprs_a64(cc);
                break;
            }
            if (is_llvm_va_end_name(cname)) {
                if (getenv("LIRIC_VERBOSE_VA_LOWER")) {
                    fprintf(stderr, "aarch64 va-lower: %s: va_end\n",
                            cc->func_name ? cc->func_name : "<anon>");
                }
                invalidate_cached_gprs_a64(cc);
                break;
            }
            if (is_llvm_va_copy_name(cname)) {
                if (getenv("LIRIC_VERBOSE_VA_LOWER")) {
                    fprintf(stderr, "aarch64 va-lower: %s: va_copy\n",
                            cc->func_name ? cc->func_name : "<anon>");
                }
                if (nops >= 3) {
                    emit_load_operand(cc, &ops_ptr[1], A64_X9);
                    emit_load_operand(cc, &ops_ptr[2], A64_X10);
                    emit_load(cc->buf, &cc->pos, cc->buflen, A64_X11, A64_X10, 0, 8);
                    emit_store(cc->buf, &cc->pos, cc->buflen, A64_X11, A64_X9, 0, 8);
                }
                invalidate_cached_gprs_a64(cc);
                break;
            }
        }

        static const uint8_t call_regs[] = {
            A64_X0, A64_X1, A64_X2, A64_X3,
            A64_X4, A64_X5, A64_X6, A64_X7
        };
        uint32_t nargs = nops - 1;
        uint32_t gp_used = 0, stack_args = 0;
        uint32_t stack_bytes;
        const char *call_sym_name = NULL;

        bool use_fp_abi;
        bool call_vararg = desc->call_vararg;
        uint32_t call_fixed_args = desc->call_fixed_args;
        bool darwin_stack_varargs = false;
        uint32_t fixed_args = 0;
        use_fp_abi = direct_call_uses_external_fp_abi(
            cc, &ops_ptr[0], desc->call_external_abi, desc->call_vararg,
            desc->call_fixed_args, &call_vararg, &call_fixed_args);
        if (ops_ptr[0].kind == LR_VAL_GLOBAL && cc->mod)
            call_sym_name = lr_module_symbol_name(cc->mod, ops_ptr[0].global_id);
        if (getenv("LIRIC_VERBOSE_CALL_ABI")) {
            fprintf(stderr,
                    "aarch64 call-abi: fn=%s callee=%s nargs=%u ext=%d vararg=%d/%d fixed=%u/%u fp_abi=%d\n",
                    cc->func_name ? cc->func_name : "<anon>",
                    call_sym_name ? call_sym_name : "<indirect>",
                    nargs,
                    desc->call_external_abi ? 1 : 0,
                    desc->call_vararg ? 1 : 0,
                    call_vararg ? 1 : 0,
                    desc->call_fixed_args,
                    call_fixed_args,
                    use_fp_abi ? 1 : 0);
        }

#if defined(__APPLE__)
        if (use_fp_abi && call_vararg) {
            darwin_stack_varargs = true;
            fixed_args = call_fixed_args;
            if (fixed_args > nargs)
                fixed_args = nargs;
        }
#endif

        if (use_fp_abi) {
            uint32_t fp_used_count = 0;
            for (uint32_t i = 0; i < nargs; i++) {
                const lr_type_t *at = ops_ptr[i + 1].type;
                bool is_variadic_stack_arg = darwin_stack_varargs &&
                                             i >= fixed_args;
                bool is_fp = at && (at->kind == LR_TYPE_FLOAT ||
                                    at->kind == LR_TYPE_DOUBLE);
                if (is_variadic_stack_arg) {
                    stack_args++;
                    continue;
                }
                if (is_fp) {
                    if (fp_used_count < 8) fp_used_count++;
                    else stack_args++;
                } else {
                    if (gp_used < 8) gp_used++;
                    else stack_args++;
                }
            }
        } else {
            stack_args = nargs > 8 ? nargs - 8 : 0;
        }

        stack_bytes = ((stack_args * 8 + 15) & ~15u);
        if (stack_bytes > 0)
            emit_sp_adjust(cc->buf, &cc->pos, cc->buflen, stack_bytes, true);

        if (use_fp_abi) {
            static const uint8_t call_fp_regs[] = {
                A64_D0, A64_D1, A64_D2, A64_D3,
                A64_D4, A64_D5, A64_D6, A64_D7
            };
            uint32_t si = 0;
            gp_used = 0;
            uint32_t fp_used_emit = 0;
            for (uint32_t i = 0; i < nargs; i++) {
                bool is_variadic_stack_arg = darwin_stack_varargs &&
                                             i >= fixed_args;
                bool is_fp = ops_ptr[i + 1].type &&
                             (ops_ptr[i + 1].type->kind == LR_TYPE_FLOAT ||
                              ops_ptr[i + 1].type->kind == LR_TYPE_DOUBLE);
                uint8_t fsz = (ops_ptr[i + 1].type &&
                               ops_ptr[i + 1].type->kind == LR_TYPE_FLOAT) ? 4 : 8;
                if (is_variadic_stack_arg) {
                    if (is_fp) {
                        emit_load_fp_operand(cc, &ops_ptr[i + 1], A64_D0, fsz);
                        emit_fp_store(cc->buf, &cc->pos, cc->buflen,
                                      A64_D0, A64_SP, (int32_t)(si * 8), fsz);
                    } else {
                        emit_load_operand(cc, &ops_ptr[i + 1], A64_X9);
                        emit_store(cc->buf, &cc->pos, cc->buflen,
                                   A64_X9, A64_SP, (int32_t)(si * 8), 8);
                    }
                    si++;
                } else if (is_fp && fp_used_emit < 8) {
                    emit_load_fp_operand(cc, &ops_ptr[i + 1],
                                         call_fp_regs[fp_used_emit], fsz);
                    fp_used_emit++;
                } else if (!is_fp && gp_used < 8) {
                    emit_load_operand(cc, &ops_ptr[i + 1], call_regs[gp_used]);
                    gp_used++;
                } else {
                    emit_load_operand(cc, &ops_ptr[i + 1], A64_X9);
                    emit_store(cc->buf, &cc->pos, cc->buflen,
                               A64_X9, A64_SP, (int32_t)(si * 8), 8);
                    si++;
                }
            }
        } else {
            uint32_t nstack = nargs > 8 ? nargs - 8 : 0;
            for (uint32_t i = 0; i < nstack; i++) {
                emit_load_operand(cc, &ops_ptr[8 + i + 1], A64_X9);
                emit_store(cc->buf, &cc->pos, cc->buflen,
                           A64_X9, A64_SP, (int32_t)(i * 8), 8);
            }
            for (uint32_t i = 0; i < nargs && i < 8; i++)
                emit_load_operand(cc, &ops_ptr[i + 1], call_regs[i]);
        }

        emit_load_operand(cc, &ops_ptr[0], A64_X16);
        emit_u32(cc->buf, &cc->pos, cc->buflen,
                 0xD63F0000u | ((uint32_t)A64_X16 << 5));

        if (stack_bytes > 0)
            emit_sp_adjust(cc->buf, &cc->pos, cc->buflen, stack_bytes, false);

        invalidate_cached_gprs_a64(cc);
        if (desc->type && desc->type->kind != LR_TYPE_VOID) {
            bool ret_fp = use_fp_abi && desc->type &&
                          (desc->type->kind == LR_TYPE_FLOAT ||
                           desc->type->kind == LR_TYPE_DOUBLE);
            if (ret_fp) {
                uint8_t rsz = (desc->type->kind == LR_TYPE_FLOAT) ? 4 : 8;
                emit_store_fp_slot(cc, desc->dest, A64_D0, rsz);
            } else {
                emit_store_slot(cc, desc->dest, A64_X0);
            }
        }
        break;
    }
    case LR_OP_PHI:
        {
            size_t phi_sz = desc->type ? lr_type_size(desc->type) : 8;
            if (phi_sz < 8)
                phi_sz = 8;
            (void)alloc_slot(cc, desc->dest, phi_sz);
        }
        break;
    case LR_OP_UNREACHABLE:
        break;
    default:
        break;
    }

    return 0;
}

static int aarch64_compile_end(void *compile_ctx, size_t *out_len) {
    a64_direct_ctx_t *ctx = (a64_direct_ctx_t *)compile_ctx;
    a64_compile_ctx_t *cc;
    bool dbg_fixups = getenv("LIRIC_DBG_A64_FIXUPS") != NULL;
    bool dbg_late_phi = getenv("LIRIC_DBG_A64_LATE_PHI") != NULL;
    uint32_t unresolved_fixups = 0;
    if (!ctx || !out_len)
        return -1;

    ctx->block_offset_pending = false;
    if (a64_flush_deferred_terminator(ctx) != 0)
        return -1;

    cc = &ctx->cc;

    /* Late phi copy stubs: for fixups whose source block has unemitted phi
       copies (registered after the terminator was flushed), emit stub code
       that applies the copies then jumps to the original target.  Patch the
       original branch instruction to redirect through the stub. */
    {
        uint32_t orig_num_fixups = cc->num_fixups;
        for (uint32_t fi = 0; fi < orig_num_fixups; fi++) {
            uint32_t source = cc->fixups[fi].source;
            uint32_t target = cc->fixups[fi].target;
            bool has_late = false;
            for (uint32_t pi = 0; pi < ctx->phi_copy_count; pi++) {
                if (ctx->phi_copies[pi].pred_block_id == source &&
                    ctx->phi_copies[pi].succ_block_id == target &&
                    !ctx->phi_copies[pi].emitted) {
                    has_late = true;
                    break;
                }
            }
            if (!has_late)
                continue;
            if (dbg_late_phi) {
                fprintf(stderr,
                        "[a64 late-phi] func=%s source=%u target=%u fixup_insn=%zu kind=%u\n",
                        cc->func_name ? cc->func_name : "<anon>",
                        source, target, cc->fixups[fi].insn_pos,
                        (unsigned)cc->fixups[fi].kind);
            }
            size_t stub_pos = cc->pos;
            /* Late stubs are reached from runtime control-flow edges, so any
               compile-time register cache assumptions are invalid here. */
            invalidate_cached_gprs_a64(cc);
            for (uint32_t pi = 0; pi < ctx->phi_copy_count; pi++) {
                if (ctx->phi_copies[pi].pred_block_id != source ||
                    ctx->phi_copies[pi].succ_block_id != target)
                    continue;
                if (dbg_late_phi) {
                    const lr_operand_t *sop = &ctx->phi_copies[pi].src_op;
                    long long imm = (long long)(sop->kind == LR_VAL_IMM_I64
                                                    ? sop->imm_i64
                                                    : 0LL);
                    fprintf(stderr,
                            "[a64 late-phi copy] dest=%u src_kind=%d src_vreg=%u imm=%lld\n",
                            ctx->phi_copies[pi].dest_vreg, (int)sop->kind,
                            sop->kind == LR_VAL_VREG ? sop->vreg : 0u, imm);
                }
                emit_phi_copy_value(cc, ctx->phi_copies[pi].dest_vreg,
                                    &ctx->phi_copies[pi].src_op);
            }
            invalidate_cached_gprs_a64(cc);
            if (a64_direct_ensure_fixup_cap(ctx) != 0)
                return -1;
            emit_jmp_a64(cc, target);
            cc->fixups[cc->num_fixups - 1].source = UINT32_MAX;
            /* Patch original branch to jump to stub instead */
            {
                int64_t stub_imm = ((int64_t)stub_pos -
                                    (int64_t)cc->fixups[fi].insn_pos) / 4;
                if (cc->fixups[fi].kind == 0) {
                    uint32_t insn = 0x14000000u
                                  | ((uint32_t)stub_imm & 0x03FFFFFFu);
                    patch_u32(cc->buf, cc->buflen,
                              cc->fixups[fi].insn_pos, insn);
                } else {
                    uint32_t insn = 0x54000000u
                                  | (((uint32_t)stub_imm & 0x7FFFFu) << 5)
                                  | (cc->fixups[fi].cond & 0xF);
                    patch_u32(cc->buf, cc->buflen,
                              cc->fixups[fi].insn_pos, insn);
                }
            }
            cc->fixups[fi].target = UINT32_MAX;
        }
    }

    for (uint32_t i = 0; i < cc->num_fixups; i++) {
        size_t target_off = SIZE_MAX;
        if (cc->fixups[i].target == UINT32_MAX) continue;
        if (cc->fixups[i].target_pos_hint != SIZE_MAX) {
            target_off = cc->fixups[i].target_pos_hint;
        } else if (cc->fixups[i].target < cc->num_block_offsets) {
            size_t entry = cc->block_entry_offsets[cc->fixups[i].target];
            size_t off = cc->block_offsets[cc->fixups[i].target];
            if (entry != SIZE_MAX && off != SIZE_MAX)
                target_off = entry < off ? entry : off;
            else if (entry != SIZE_MAX)
                target_off = entry;
            else if (off != SIZE_MAX)
                target_off = off;
        }
        if (target_off == SIZE_MAX) {
            unresolved_fixups++;
            if (dbg_fixups) {
                fprintf(stderr,
                        "a64 unresolved fixup func=%s src=%u tgt=%u insn=%zu blocks=%u\n",
                        cc->func_name ? cc->func_name : "<anon>",
                        cc->fixups[i].source,
                        cc->fixups[i].target,
                        cc->fixups[i].insn_pos,
                        cc->num_block_offsets);
            }
            continue;
        }
        if (getenv("LIRIC_DBG_A64_FIXUPS") != NULL) {
            fprintf(stderr,
                    "[a64 patch] func=%s idx=%u src=%u tgt=%u insn=%zu hint=%zu off=%zu kind=%u\n",
                    cc->func_name ? cc->func_name : "<anon>",
                    i,
                    cc->fixups[i].source,
                    cc->fixups[i].target,
                    cc->fixups[i].insn_pos,
                    cc->fixups[i].target_pos_hint,
                    target_off,
                    (unsigned)cc->fixups[i].kind);
        }

        int64_t target_pos = (int64_t)target_off;
        int64_t here = (int64_t)cc->fixups[i].insn_pos;
        int64_t imm = (target_pos - here) / 4;

        if (cc->fixups[i].kind == 0) {
            if (imm >= -(1LL << 25) && imm < (1LL << 25)) {
                uint32_t insn = 0x14000000u | ((uint32_t)imm & 0x03FFFFFFu);
                patch_u32(cc->buf, cc->buflen, cc->fixups[i].insn_pos, insn);
            }
        } else {
            if (imm >= -(1LL << 18) && imm < (1LL << 18)) {
                uint32_t insn = 0x54000000u
                              | (((uint32_t)imm & 0x7FFFFu) << 5)
                              | (cc->fixups[i].cond & 0xF);
                patch_u32(cc->buf, cc->buflen, cc->fixups[i].insn_pos, insn);
            }
        }
    }

    patch_prologue_stack_adjust(cc, ctx->prologue_patch_pos,
                                (cc->stack_size + 15u) & ~15u);

    if (unresolved_fixups != 0 && dbg_fixups) {
        fprintf(stderr, "a64 unresolved fixup count func=%s count=%u\n",
                cc->func_name ? cc->func_name : "<anon>", unresolved_fixups);
    }

    *out_len = cc->pos;
    if (cc->pos > cc->buflen)
        return -1;
    return 0;
}

static int aarch64_compile_add_phi_copy(void *compile_ctx,
                                        uint32_t pred_block_id,
                                        uint32_t succ_block_id,
                                        uint32_t dest_vreg,
                                        const lr_operand_desc_t *src_op) {
    a64_direct_ctx_t *ctx = (a64_direct_ctx_t *)compile_ctx;
    if (!ctx || !src_op)
        return -1;
    if (a64_direct_ensure_phi_copy_cap(ctx) != 0)
        return -1;

    {
        size_t copy_sz = src_op->type ? lr_type_size(src_op->type) : 8;
        if (copy_sz < 8)
            copy_sz = 8;
        (void)alloc_slot(&ctx->cc, dest_vreg, copy_sz);
    }

    a64_stream_phi_copy_t *entry = &ctx->phi_copies[ctx->phi_copy_count++];
    entry->pred_block_id = pred_block_id;
    entry->succ_block_id = succ_block_id;
    entry->dest_vreg = dest_vreg;
    entry->src_op = a64_operand_from_desc(src_op);
    entry->emitted = false;
    if (getenv("LIRIC_DBG_A64_PHI") != NULL) {
        fprintf(stderr,
                "[a64 phi add] func=%s pred=%u succ=%u dest=%u src_kind=%d src_vreg=%u\n",
                ctx->cc.func_name ? ctx->cc.func_name : "<anon>",
                pred_block_id, succ_block_id, dest_vreg,
                (int)entry->src_op.kind,
                entry->src_op.kind == LR_VAL_VREG ? entry->src_op.vreg : 0u);
    }
    return 0;
}

static const lr_target_t aarch64_target = {
    .name = "aarch64",
    .ptr_size = 8,
    .compile_begin = aarch64_compile_begin,
    .compile_emit = aarch64_compile_emit,
    .compile_set_block = aarch64_compile_set_block,
    .compile_end = aarch64_compile_end,
    .compile_add_phi_copy = aarch64_compile_add_phi_copy,
};

const lr_target_t *lr_target_aarch64(void) {
    return &aarch64_target;
}
