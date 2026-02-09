#include "target_x86_64.h"
#include "target_common.h"
#include "target_shared.h"
#include "objfile.h"
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
 * Stack slots are allocated lazily while emitting instructions; the prologue
 * stack adjustment is patched after emission when final frame size is known.
 */

#define FP_SCRATCH0  X86_XMM0
#define FP_SCRATCH1  X86_XMM1

typedef struct { size_t pos; uint32_t target; } x86_fixup_t;

/* Backend-local compile context replacing the old MIR linked-list state */
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
    x86_fixup_t *fixups;
    uint32_t num_fixups;
    uint32_t fixup_cap;
    lr_arena_t *arena;
    lr_objfile_ctx_t *obj_ctx;
    lr_module_t *mod;
    uint8_t *sym_defined;
    lr_func_t **sym_funcs;
    uint32_t sym_count;
} x86_compile_ctx_t;

static size_t align_up(size_t value, size_t align) {
    if (align <= 1)
        return value;
    return ((value + align - 1) / align) * align;
}

/* Allocate a stack slot for a vreg, return rbp offset (negative). */
static int32_t alloc_slot(x86_compile_ctx_t *ctx, uint32_t vreg,
                          size_t size, size_t align) {
    while (vreg >= ctx->num_stack_slots) {
        uint32_t old = ctx->num_stack_slots;
        uint32_t new_cap = old == 0 ? 64 : old * 2;
        int32_t *ns = lr_arena_array_uninit(ctx->arena, int32_t, new_cap);
        uint32_t *ss = lr_arena_array_uninit(ctx->arena, uint32_t, new_cap);
        if (old > 0) memcpy(ns, ctx->stack_slots, old * sizeof(int32_t));
        if (old > 0) memcpy(ss, ctx->stack_slot_sizes, old * sizeof(uint32_t));
        for (uint32_t i = old; i < new_cap; i++) {
            ns[i] = 0;
            ss[i] = 0;
        }
        ctx->stack_slots = ns;
        ctx->stack_slot_sizes = ss;
        ctx->num_stack_slots = new_cap;
    }

    if (ctx->stack_slots[vreg] != 0) {
        return ctx->stack_slots[vreg];
    }

    if (size < 8) size = 8;
    if (align < 8) align = 8;
    ctx->stack_size = (uint32_t)align_up(ctx->stack_size, align);
    ctx->stack_size += (uint32_t)size;
    int32_t offset = -(int32_t)ctx->stack_size;
    ctx->stack_slots[vreg] = offset;
    ctx->stack_slot_sizes[vreg] = (uint32_t)size;
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

static void patch_u32(uint8_t *buf, size_t len, size_t pos, uint32_t v) {
    if (pos + 4 > len)
        return;
    buf[pos + 0] = (uint8_t)(v >> 0);
    buf[pos + 1] = (uint8_t)(v >> 8);
    buf[pos + 2] = (uint8_t)(v >> 16);
    buf[pos + 3] = (uint8_t)(v >> 24);
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
    int32_t off = alloc_slot(ctx, vreg, 8, 8);
    encode_mem(ctx->buf, &ctx->pos, ctx->buflen, 0x8B, reg, X86_RBP, off, 8);
}

/* Emit: mov [rbp + offset], reg (store reg to vreg stack slot) */
static void emit_store_slot(x86_compile_ctx_t *ctx, uint32_t vreg, uint8_t reg) {
    int32_t off = alloc_slot(ctx, vreg, 8, 8);
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

/* Emit: add/sub reg, imm32 (sign-extended) */
static void emit_add_imm(x86_compile_ctx_t *ctx, uint8_t dst, int64_t imm) {
    if (imm == 0)
        return;
    if (imm > INT32_MAX || imm < INT32_MIN) {
        emit_mov_imm(ctx, X86_R11, imm);
        encode_alu_rr(ctx->buf, &ctx->pos, ctx->buflen, 0x01, dst, X86_R11, 8);
        return;
    }
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, rex(true, false, false, dst >= 8));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x81);
    if (imm >= 0) {
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, 0, dst)); /* ADD */
        emit_u32(ctx->buf, &ctx->pos, ctx->buflen, (uint32_t)(int32_t)imm);
    } else {
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, 5, dst)); /* SUB */
        emit_u32(ctx->buf, &ctx->pos, ctx->buflen, (uint32_t)(int32_t)(-imm));
    }
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

static void attach_obj_symbol_meta_cache(x86_compile_ctx_t *ctx) {
    if (!ctx || !ctx->obj_ctx)
        return;
    ctx->sym_defined = ctx->obj_ctx->module_sym_defined;
    ctx->sym_funcs = ctx->obj_ctx->module_sym_funcs;
    ctx->sym_count = ctx->obj_ctx->module_sym_count;
}

static bool is_fp_abi_type(const lr_type_t *type) {
    return type &&
           (type->kind == LR_TYPE_FLOAT || type->kind == LR_TYPE_DOUBLE);
}

static uint8_t fp_abi_size(const lr_type_t *type) {
    return (type && type->kind == LR_TYPE_FLOAT) ? 4 : 8;
}

static lr_func_t *find_module_function(lr_module_t *mod, const char *name) {
    if (!mod || !name) return NULL;
    for (lr_func_t *f = mod->first_func; f; f = f->next) {
        if (strcmp(f->name, name) == 0)
            return f;
    }
    return NULL;
}

static bool call_uses_external_sysv_abi(x86_compile_ctx_t *ctx,
                                         const lr_inst_t *inst,
                                         lr_func_t **callee_func_out) {
    const lr_operand_t *callee = NULL;
    lr_func_t *callee_func = NULL;

    if (callee_func_out) *callee_func_out = NULL;
    if (!ctx || !ctx->mod || !inst || inst->num_operands == 0)
        return false;

    callee = &inst->operands[0];
    if (callee->kind != LR_VAL_GLOBAL)
        return false;

    if (callee->global_id < ctx->sym_count) {
        callee_func = ctx->sym_funcs[callee->global_id];
        if (callee_func_out) *callee_func_out = callee_func;
        if (callee_func)
            return callee_func->first_block == NULL;
        return ctx->sym_defined[callee->global_id] == 0;
    }

    const char *sym_name = lr_module_symbol_name(ctx->mod, callee->global_id);
    if (!sym_name)
        return false;
    callee_func = find_module_function(ctx->mod, sym_name);
    if (callee_func_out) *callee_func_out = callee_func;
    if (callee_func)
        return callee_func->first_block == NULL;
    return !is_symbol_defined_in_module(ctx->mod, sym_name);
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
    } else if (op->kind == LR_VAL_NULL || op->kind == LR_VAL_UNDEF) {
        emit_mov_imm(ctx, reg, 0);
    } else if (op->kind == LR_VAL_GLOBAL && ctx->obj_ctx) {
        const char *sym_name = lr_module_symbol_name(ctx->mod,
                                                      op->global_id);
        if (!sym_name) {
            emit_mov_imm(ctx, reg, 0);
            return;
        }
        bool defined = false;
        if (op->global_id < ctx->sym_count)
            defined = ctx->sym_defined[op->global_id] != 0;
        else
            defined = is_symbol_defined_in_module(ctx->mod, sym_name);
        uint32_t sym_idx = lr_obj_ensure_symbol(ctx->obj_ctx, sym_name,
                                                 false, 0, 0);
        if (defined) {
            /* LEA reg, [RIP + disp32] for defined symbols */
            emit_byte(ctx->buf, &ctx->pos, ctx->buflen,
                      rex(true, reg >= 8, false, false));
            emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x8D);
            emit_byte(ctx->buf, &ctx->pos, ctx->buflen,
                      modrm(0, reg, 5)); /* mod=00, rm=5 = RIP-relative */
            size_t disp_off = ctx->pos;
            emit_u32(ctx->buf, &ctx->pos, ctx->buflen, 0);
            lr_obj_add_reloc(ctx->obj_ctx, (uint32_t)disp_off, sym_idx,
                              LR_RELOC_X86_64_PC32);
        } else {
            /* MOV reg, [RIP + disp32] for GOT entry (external symbols) */
            emit_byte(ctx->buf, &ctx->pos, ctx->buflen,
                      rex(true, reg >= 8, false, false));
            emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x8B);
            emit_byte(ctx->buf, &ctx->pos, ctx->buflen,
                      modrm(0, reg, 5));
            size_t disp_off = ctx->pos;
            emit_u32(ctx->buf, &ctx->pos, ctx->buflen, 0);
            lr_obj_add_reloc(ctx->obj_ctx, (uint32_t)disp_off, sym_idx,
                              LR_RELOC_X86_64_GOTPCREL);
        }
        if (op->global_offset != 0)
            emit_add_imm(ctx, reg, op->global_offset);
    }
}

/* FP helpers: load/store FP values between stack slots and XMM regs.
 * Stack slots hold the raw bit representation; SSE2 FP load/store instructions
 * interpret the same bits as float/double. */

static void emit_load_fp_slot(x86_compile_ctx_t *ctx,
                               uint32_t vreg, uint8_t fpreg, uint8_t fsize) {
    int32_t off = alloc_slot(ctx, vreg, 8, 8);
    uint8_t prefix = (fsize == 8) ? 0xF2 : 0xF3;
    encode_sse_mem(ctx->buf, &ctx->pos, ctx->buflen, prefix, 0x10, 0,
                   fpreg, X86_RBP, off);
}

static void emit_store_fp_slot(x86_compile_ctx_t *ctx,
                                uint32_t vreg, uint8_t fpreg, uint8_t fsize) {
    int32_t off = alloc_slot(ctx, vreg, 8, 8);
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

/* Emit prologue and reserve a patch slot for `sub rsp, imm32`. */
static size_t emit_prologue(x86_compile_ctx_t *ctx) {
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x55); /* push rbp */
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, rex(true, false, false, false));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x89);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, X86_RSP, X86_RBP)); /* mov rbp, rsp */

    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, rex(true, false, false, false));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x81);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, 5, X86_RSP));
    {
        size_t imm_pos = ctx->pos;
        emit_u32(ctx->buf, &ctx->pos, ctx->buflen, 0);
        return imm_pos;
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
static void emit_phi_copies(x86_compile_ctx_t *ctx, lr_phi_copy_t *copies) {
    for (lr_phi_copy_t *pc = copies; pc; pc = pc->next) {
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

static void emit_movsx_rr(x86_compile_ctx_t *ctx, uint8_t dst, uint8_t src, uint8_t size) {
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen,
              rex(true, dst >= 8, false, src >= 8));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x0F);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, (size == 1) ? 0xBE : 0xBF);
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

static void emit_mem_load_sized(x86_compile_ctx_t *ctx, uint8_t dst,
                                uint8_t base, int32_t disp, uint8_t size) {
    if (size < 4)
        emit_movzx_mem(ctx, dst, base, disp, size);
    else
        encode_mem(ctx->buf, &ctx->pos, ctx->buflen, 0x8B, dst, base, disp, size);
}

static void emit_mem_store_sized(x86_compile_ctx_t *ctx, uint8_t src,
                                 uint8_t base, int32_t disp, uint8_t size) {
    uint8_t opcode = (size == 1) ? 0x88 : 0x89;
    encode_mem(ctx->buf, &ctx->pos, ctx->buflen, opcode, src, base, disp, size);
}

static void emit_mem_copy_base_to_base(x86_compile_ctx_t *ctx,
                                       uint8_t dst_base, int32_t dst_disp,
                                       uint8_t src_base, int32_t src_disp,
                                       size_t bytes) {
    const uint8_t scratch = X86_R11;
    size_t off = 0;
    while (bytes - off >= 8) {
        emit_mem_load_sized(ctx, scratch, src_base, src_disp + (int32_t)off, 8);
        emit_mem_store_sized(ctx, scratch, dst_base, dst_disp + (int32_t)off, 8);
        off += 8;
    }
    if (bytes - off >= 4) {
        emit_mem_load_sized(ctx, scratch, src_base, src_disp + (int32_t)off, 4);
        emit_mem_store_sized(ctx, scratch, dst_base, dst_disp + (int32_t)off, 4);
        off += 4;
    }
    if (bytes - off >= 2) {
        emit_mem_load_sized(ctx, scratch, src_base, src_disp + (int32_t)off, 2);
        emit_mem_store_sized(ctx, scratch, dst_base, dst_disp + (int32_t)off, 2);
        off += 2;
    }
    if (bytes - off == 1) {
        emit_mem_load_sized(ctx, scratch, src_base, src_disp + (int32_t)off, 1);
        emit_mem_store_sized(ctx, scratch, dst_base, dst_disp + (int32_t)off, 1);
    }
}

static void emit_mem_zero_base(x86_compile_ctx_t *ctx, uint8_t dst_base,
                               int32_t dst_disp, size_t bytes) {
    size_t off = 0;
    emit_mov_imm(ctx, X86_RAX, 0);
    while (bytes - off >= 8) {
        emit_mem_store_sized(ctx, X86_RAX, dst_base, dst_disp + (int32_t)off, 8);
        off += 8;
    }
    if (bytes - off >= 4) {
        emit_mem_store_sized(ctx, X86_RAX, dst_base, dst_disp + (int32_t)off, 4);
        off += 4;
    }
    if (bytes - off >= 2) {
        emit_mem_store_sized(ctx, X86_RAX, dst_base, dst_disp + (int32_t)off, 2);
        off += 2;
    }
    if (bytes - off == 1)
        emit_mem_store_sized(ctx, X86_RAX, dst_base, dst_disp + (int32_t)off, 1);
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

static void emit_load_vreg_mem_sized(x86_compile_ctx_t *ctx, uint32_t src_vreg,
                                     int32_t add_off, uint8_t reg, uint8_t size) {
    int32_t src_off = alloc_slot(ctx, src_vreg, 8, 8) + add_off;
    emit_mem_load_sized(ctx, reg, X86_RBP, src_off, size);
}

static void emit_jmp(x86_compile_ctx_t *ctx, uint32_t target_block) {
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0xE9);
    if (ctx->num_fixups < ctx->fixup_cap) {
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
    if (ctx->num_fixups < ctx->fixup_cap) {
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

static int32_t ensure_static_alloca_offset(x86_compile_ctx_t *ctx, const lr_inst_t *inst) {
    int32_t off = lr_target_lookup_static_alloca_offset(ctx->static_alloca_offsets,
                                                        ctx->num_static_alloca_offsets,
                                                        inst->dest);
    if (off != 0)
        return off;

    size_t elem_sz = lr_target_alloca_elem_size(inst, 8);
    size_t elem_align = lr_type_align(inst->type);
    if (elem_align < 8)
        elem_align = 8;
    ctx->stack_size = (uint32_t)align_up(ctx->stack_size, elem_align);
    ctx->stack_size += (uint32_t)elem_sz;
    {
        int32_t new_off = -(int32_t)ctx->stack_size;
        lr_target_set_static_alloca_offset(ctx->arena,
                                           &ctx->static_alloca_offsets,
                                           &ctx->num_static_alloca_offsets,
                                           inst->dest,
                                           new_off);
        return new_off;
    }
}

static int32_t ensure_static_alloca_offset_cb(void *ctx, const lr_inst_t *inst) {
    return ensure_static_alloca_offset((x86_compile_ctx_t *)ctx, inst);
}

static void reserve_phi_dest_slots(x86_compile_ctx_t *ctx, lr_phi_copy_t **phi_copies,
                                   uint32_t num_blocks) {
    for (uint32_t bi = 0; bi < num_blocks; bi++) {
        for (lr_phi_copy_t *pc = phi_copies[bi]; pc; pc = pc->next)
            alloc_slot(ctx, pc->dest_vreg, 8, 8);
    }
}

/*
 * x86_64_compile_func: single-pass ISel + encoding.
 * Replaces the old two-phase isel_func + encode_func approach.
 */
static int x86_64_compile_func(lr_func_t *func, lr_module_t *mod,
                                uint8_t *buf, size_t buflen, size_t *out_len,
                                lr_arena_t *arena) {
    uint32_t nb = func->num_blocks > 0 ? func->num_blocks : 1;
    uint32_t fc = nb * 2;
    x86_compile_ctx_t ctx = {
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
        .fixups = lr_arena_array_uninit(arena, x86_fixup_t, fc),
        .num_fixups = 0,
        .fixup_cap = fc,
        .arena = arena,
        .obj_ctx = mod ? (lr_objfile_ctx_t *)mod->obj_ctx : NULL,
        .mod = mod,
        .sym_defined = NULL,
        .sym_funcs = NULL,
        .sym_count = 0,
    };

    attach_obj_symbol_meta_cache(&ctx);

    /* Build PHI copy lists */
    lr_phi_copy_t **phi_copies = lr_build_phi_copies(ctx.arena, func);
    reserve_phi_dest_slots(&ctx, phi_copies, nb);
    lr_target_prescan_static_alloca_offsets(func, &ctx,
                                            ensure_static_alloca_offset_cb);

    /* Emit prologue and patch stack size once frame growth is complete. */
    size_t prologue_stack_patch_pos = emit_prologue(&ctx);

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

                uint8_t cc = lr_target_cc_from_icmp(inst->icmp_pred);

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
                size_t elem_sz = lr_target_alloca_elem_size(inst, 8);

                bool use_static = lr_target_alloca_uses_static_storage(inst);

                if (use_static) {
                    int32_t off = ensure_static_alloca_offset(&ctx, inst);
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
                size_t load_sz = lr_type_size(inst->type);
                if (load_sz > 8) {
                    size_t load_align = lr_type_align(inst->type);
                    int32_t dst_off = alloc_slot(&ctx, inst->dest, load_sz, load_align);
                    emit_mem_copy_base_to_base(&ctx, X86_RBP, dst_off, X86_RAX, 0, load_sz);
                } else {
                    uint8_t sz = (uint8_t)load_sz;
                    if (sz < 4) {
                        emit_movzx_mem(&ctx, X86_RAX, X86_RAX, 0, sz);
                    } else {
                        encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x8B, X86_RAX, X86_RAX, 0, sz);
                    }
                    emit_store_slot(&ctx, inst->dest, X86_RAX);
                }
                break;
            }
            case LR_OP_STORE: {
                emit_load_operand(&ctx, &inst->operands[1], X86_RCX);
                size_t store_sz = lr_type_size(inst->operands[0].type);
                if (store_sz == 0)
                    store_sz = 8;

                if (store_sz > 8) {
                    if (inst->operands[0].kind == LR_VAL_IMM_I64 &&
                        inst->operands[0].imm_i64 == 0) {
                        emit_mem_zero_base(&ctx, X86_RCX, 0, store_sz);
                        break;
                    }

                    if (inst->operands[0].kind == LR_VAL_VREG) {
                        uint32_t vreg = inst->operands[0].vreg;
                        size_t src_sz = 0;
                        int32_t src_off = alloc_slot(&ctx, vreg, 8, 8);
                        if (vreg < ctx.num_stack_slots)
                            src_sz = ctx.stack_slot_sizes[vreg];
                        if (src_sz > store_sz)
                            src_sz = store_sz;
                        if (src_sz > 0)
                            emit_mem_copy_base_to_base(&ctx, X86_RCX, 0, X86_RBP, src_off, src_sz);
                        if (src_sz < store_sz)
                            emit_mem_zero_base(&ctx, X86_RCX, (int32_t)src_sz, store_sz - src_sz);
                        break;
                    }

                    /* Conservative fallback for unsupported aggregate sources. */
                    emit_mem_zero_base(&ctx, X86_RCX, 0, store_sz);
                    break;
                }

                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                emit_mem_store_sized(&ctx, X86_RAX, X86_RCX, 0, (uint8_t)store_sz);
                break;
            }
            case LR_OP_GEP: {
                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
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
                        emit_mov_imm(&ctx, X86_RCX, step.const_byte_offset);
                        encode_alu_rr(ctx.buf, &ctx.pos, ctx.buflen, 0x01, X86_RAX, X86_RCX, 8);
                        continue;
                    }

                    emit_load_operand(&ctx, idx_op, X86_RCX);
                    if (step.runtime_signext_bytes == 1 || step.runtime_signext_bytes == 2) {
                        emit_movsx_rr(&ctx, X86_RCX, X86_RCX, step.runtime_signext_bytes);
                    } else if (step.runtime_signext_bytes == 4) {
                        emit_movsxd(&ctx, X86_RCX, X86_RCX);
                    }
                    if (step.runtime_elem_size != 1) {
                        emit_mov_imm(&ctx, X86_R10, (int64_t)step.runtime_elem_size);
                        emit_imul_rr(&ctx, X86_RCX, X86_R10, 8);
                    }
                    encode_alu_rr(ctx.buf, &ctx.pos, ctx.buflen, 0x01, X86_RAX, X86_RCX, 8);
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

                uint8_t cc = lr_target_cc_from_fcmp(inst->fcmp_pred);

                emit_setcc(&ctx, cc, X86_RAX);
                emit_movzx_rr(&ctx, X86_RAX, X86_RAX, 1);
                emit_store_slot(&ctx, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_SITOFP: {
                uint8_t fsize = (inst->type && inst->type->kind == LR_TYPE_FLOAT) ? 4 : 8;
                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                size_t src_sz = lr_type_size(inst->operands[0].type);
                if (src_sz == 1 || src_sz == 2)
                    emit_movsx_rr(&ctx, X86_RAX, X86_RAX, (uint8_t)src_sz);
                else if (src_sz == 4)
                    emit_movsxd(&ctx, X86_RAX, X86_RAX);
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
                        size_t dst_align = inst->type ? lr_type_align(inst->type) : 8;
                        if (dst_align < 8) {
                            dst_align = 8;
                        }
                        int32_t dst_off = alloc_slot(&ctx, inst->dest, field_sz, dst_align);
                        int32_t src_off = alloc_slot(&ctx, inst->operands[0].vreg, 8, 8) +
                                          (int32_t)field_off;
                        emit_mem_copy_base_to_base(&ctx, X86_RBP, dst_off,
                                                   X86_RBP, src_off, field_sz);
                    } else {
                        emit_load_vreg_mem_sized(&ctx, inst->operands[0].vreg,
                                                 (int32_t)field_off, X86_RAX,
                                                 (uint8_t)field_sz);
                        emit_store_slot(&ctx, inst->dest, X86_RAX);
                    }
                    break;
                }

                if (inst->num_operands > 0 &&
                    (inst->operands[0].kind == LR_VAL_UNDEF ||
                     inst->operands[0].kind == LR_VAL_NULL)) {
                    if (field_sz > 8) {
                        size_t dst_align = inst->type ? lr_type_align(inst->type) : 8;
                        if (dst_align < 8) {
                            dst_align = 8;
                        }
                        int32_t dst_off = alloc_slot(&ctx, inst->dest, field_sz, dst_align);
                        emit_mem_zero_base(&ctx, X86_RBP, dst_off, field_sz);
                    } else {
                        emit_mov_imm(&ctx, X86_RAX, 0);
                        emit_store_slot(&ctx, inst->dest, X86_RAX);
                    }
                    break;
                }

                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                emit_store_slot(&ctx, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_INSERTVALUE: {
                size_t agg_sz = inst->type ? lr_type_size(inst->type) : 8;
                size_t agg_align = inst->type ? lr_type_align(inst->type) : 8;
                size_t field_off = 0;
                const lr_type_t *field_ty = NULL;
                int32_t dst_off;

                if (agg_sz < 8) {
                    agg_sz = 8;
                }
                if (agg_align < 8) {
                    agg_align = 8;
                }
                dst_off = alloc_slot(&ctx, inst->dest, agg_sz, agg_align);

                if (inst->num_operands > 0) {
                    const lr_operand_t *agg = &inst->operands[0];
                    if (agg->kind == LR_VAL_VREG) {
                        size_t src_sz = 0;
                        int32_t src_off = alloc_slot(&ctx, agg->vreg, 8, 8);
                        if (agg->vreg < ctx.num_stack_slots) {
                            src_sz = ctx.stack_slot_sizes[agg->vreg];
                        }
                        if (src_sz > agg_sz) {
                            src_sz = agg_sz;
                        }
                        if (src_sz > 0) {
                            emit_mem_copy_base_to_base(&ctx, X86_RBP, dst_off,
                                                       X86_RBP, src_off, src_sz);
                        }
                        if (src_sz < agg_sz) {
                            emit_mem_zero_base(&ctx, X86_RBP,
                                               dst_off + (int32_t)src_sz,
                                               agg_sz - src_sz);
                        }
                    } else if (agg->kind == LR_VAL_UNDEF || agg->kind == LR_VAL_NULL) {
                        emit_mem_zero_base(&ctx, X86_RBP, dst_off, agg_sz);
                    } else if (agg_sz <= 8) {
                        emit_load_operand(&ctx, agg, X86_RAX);
                        emit_mem_store_sized(&ctx, X86_RAX, X86_RBP, dst_off,
                                             (uint8_t)agg_sz);
                    } else {
                        emit_mem_zero_base(&ctx, X86_RBP, dst_off, agg_sz);
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
                            int32_t src_off = alloc_slot(&ctx, val->vreg, 8, 8);
                            if (val->vreg < ctx.num_stack_slots) {
                                src_sz = ctx.stack_slot_sizes[val->vreg];
                            }
                            if (src_sz > field_sz) {
                                src_sz = field_sz;
                            }
                            if (src_sz > 0) {
                                emit_mem_copy_base_to_base(&ctx, X86_RBP,
                                                           dst_off + (int32_t)field_off,
                                                           X86_RBP, src_off, src_sz);
                            }
                            if (src_sz < field_sz) {
                                emit_mem_zero_base(&ctx, X86_RBP,
                                                   dst_off + (int32_t)field_off +
                                                   (int32_t)src_sz,
                                                   field_sz - src_sz);
                            }
                        } else {
                            emit_mem_zero_base(&ctx, X86_RBP,
                                               dst_off + (int32_t)field_off,
                                               field_sz);
                        }
                    } else {
                        if (val->kind == LR_VAL_UNDEF || val->kind == LR_VAL_NULL) {
                            emit_mov_imm(&ctx, X86_RAX, 0);
                        } else {
                            emit_load_operand(&ctx, val, X86_RAX);
                        }
                        emit_mem_store_sized(&ctx, X86_RAX, X86_RBP,
                                             dst_off + (int32_t)field_off,
                                             (uint8_t)field_sz);
                    }
                }
                break;
            }
            case LR_OP_CALL: {
                static const uint8_t call_regs[] = { X86_RDI, X86_RSI, X86_RDX, X86_RCX, X86_R8, X86_R9 };
                static const uint8_t call_fp_regs[] = {
                    X86_XMM0, X86_XMM1, X86_XMM2, X86_XMM3,
                    X86_XMM4, X86_XMM5, X86_XMM6, X86_XMM7
                };
                uint32_t nargs = inst->num_operands - 1;
                uint32_t gp_used = 0;
                uint32_t fp_used = 0;
                uint32_t stack_args = 0;
                uint32_t stack_bytes = 0;
                uint32_t fp_used_for_call = 0;
                lr_func_t *callee_func = NULL;
                bool use_external_sysv_fp = false;
                bool callee_vararg = false;

                if (inst->operands[0].kind == LR_VAL_GLOBAL) {
                    use_external_sysv_fp =
                        call_uses_external_sysv_abi(&ctx, inst, &callee_func);
                    callee_vararg = callee_func && callee_func->vararg;
                } else {
                    use_external_sysv_fp = inst->call_external_abi;
                    callee_vararg = inst->call_vararg;
                }

                if (use_external_sysv_fp) {
                    for (uint32_t i = 0; i < nargs; i++) {
                        const lr_type_t *arg_type = inst->operands[i + 1].type;
                        if (is_fp_abi_type(arg_type)) {
                            if (fp_used < 8) fp_used++;
                            else stack_args++;
                        } else {
                            if (gp_used < 6) gp_used++;
                            else stack_args++;
                        }
                    }
                } else {
                    stack_args = nargs > 6 ? nargs - 6 : 0;
                }

                stack_bytes = ((stack_args * 8 + 15) & ~15u);

                if (stack_bytes > 0)
                    emit_frame_alloc(&ctx, stack_bytes);

                if (use_external_sysv_fp) {
                    uint32_t stack_idx = 0;
                    gp_used = 0;
                    fp_used = 0;
                    for (uint32_t i = 0; i < nargs; i++) {
                        const lr_operand_t *arg = &inst->operands[i + 1];
                        if (is_fp_abi_type(arg->type) && fp_used < 8) {
                            emit_load_fp_operand(&ctx, arg, call_fp_regs[fp_used],
                                                 fp_abi_size(arg->type));
                            fp_used++;
                            continue;
                        }
                        if (!is_fp_abi_type(arg->type) && gp_used < 6) {
                            emit_load_operand(&ctx, arg, call_regs[gp_used]);
                            gp_used++;
                            continue;
                        }
                        emit_load_operand(&ctx, arg, X86_RAX);
                        encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x89, X86_RAX,
                                   X86_RSP, (int32_t)(stack_idx * 8), 8);
                        stack_idx++;
                    }
                    fp_used_for_call = fp_used;
                } else {
                    /* Legacy internal-call convention: first 6 args in GPRs, rest on stack. */
                    uint32_t nstack = nargs > 6 ? nargs - 6 : 0;
                    for (uint32_t i = 0; i < nstack; i++) {
                        uint32_t arg_idx = 6 + i;
                        emit_load_operand(&ctx, &inst->operands[arg_idx + 1], X86_RAX);
                        encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x89, X86_RAX,
                                   X86_RSP, (int32_t)(i * 8), 8);
                    }
                    for (uint32_t i = 0; i < nargs && i < 6; i++)
                        emit_load_operand(&ctx, &inst->operands[i + 1], call_regs[i]);
                }

                if (callee_vararg)
                    emit_mov_imm(&ctx, X86_RAX, (int64_t)fp_used_for_call);

                if (ctx.obj_ctx &&
                    inst->operands[0].kind == LR_VAL_GLOBAL) {
                    const char *sym_name = lr_module_symbol_name(
                        ctx.mod, inst->operands[0].global_id);
                    if (sym_name) {
                        uint32_t sym_idx = lr_obj_ensure_symbol(
                            ctx.obj_ctx, sym_name, false, 0, 0);
                        emit_byte(ctx.buf, &ctx.pos, ctx.buflen, 0xE8);
                        size_t disp_off = ctx.pos;
                        emit_u32(ctx.buf, &ctx.pos, ctx.buflen, 0);
                        lr_obj_add_reloc(ctx.obj_ctx, (uint32_t)disp_off,
                                          sym_idx, LR_RELOC_X86_64_PLT32);
                    } else {
                        /* Fallback: NOP (5 bytes to match call encoding) */
                        emit_byte(ctx.buf, &ctx.pos, ctx.buflen, 0x0F);
                        emit_byte(ctx.buf, &ctx.pos, ctx.buflen, 0x1F);
                        emit_byte(ctx.buf, &ctx.pos, ctx.buflen, 0x44);
                        emit_byte(ctx.buf, &ctx.pos, ctx.buflen, 0x00);
                        emit_byte(ctx.buf, &ctx.pos, ctx.buflen, 0x00);
                    }
                } else {
                    emit_load_operand(&ctx, &inst->operands[0], X86_R10);
                    emit_call_r10(&ctx);
                }

                if (stack_bytes > 0)
                    emit_frame_free(&ctx, stack_bytes);

                if (inst->type && inst->type->kind != LR_TYPE_VOID) {
                    if (use_external_sysv_fp && is_fp_abi_type(inst->type))
                        emit_store_fp_slot(&ctx, inst->dest, X86_XMM0,
                                           fp_abi_size(inst->type));
                    else
                        emit_store_slot(&ctx, inst->dest, X86_RAX);
                }
                break;
            }
            case LR_OP_PHI:
                /* Handled via phi_copies. */
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

    {
        uint32_t frame_stack_size = (ctx.stack_size + 15u) & ~15u;
        patch_u32(buf, buflen, prologue_stack_patch_pos, frame_stack_size);
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
