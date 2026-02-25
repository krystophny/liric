#include "target_x86_64.h"
#include "target_common.h"
#include "target_shared.h"
#include "objfile.h"
#include "jit.h"
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

typedef struct { size_t pos; uint32_t target; uint32_t source; } x86_fixup_t;

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
    uint32_t rax_holds_vreg;
    uint32_t rcx_holds_vreg;
    lr_inst_t *current_inst;
    bool func_uses_internal_sret;
    int32_t sret_ptr_off;
    bool func_is_vararg;
    int32_t vararg_rsa_off;
    uint32_t vararg_named_gp;
    lr_jit_t *jit;
    bool func_uses_external_sysv_fp;
} x86_compile_ctx_t;

static void invalidate_cached_reg(x86_compile_ctx_t *ctx, uint8_t reg) {
    if (!ctx) return;
    if (reg == X86_RAX) ctx->rax_holds_vreg = UINT32_MAX;
    if (reg == X86_RCX) ctx->rcx_holds_vreg = UINT32_MAX;
}

static void invalidate_cached_gprs(x86_compile_ctx_t *ctx) {
    if (!ctx) return;
    ctx->rax_holds_vreg = UINT32_MAX;
    ctx->rcx_holds_vreg = UINT32_MAX;
}

static bool cached_reg_holds_vreg(const x86_compile_ctx_t *ctx, uint8_t reg, uint32_t vreg) {
    if (!ctx) return false;
    if (reg == X86_RAX) return ctx->rax_holds_vreg == vreg;
    if (reg == X86_RCX) return ctx->rcx_holds_vreg == vreg;
    return false;
}

static void set_cached_reg_vreg(x86_compile_ctx_t *ctx, uint8_t reg, uint32_t vreg) {
    if (!ctx) return;
    if (reg == X86_RAX) ctx->rax_holds_vreg = vreg;
    if (reg == X86_RCX) ctx->rcx_holds_vreg = vreg;
}

static size_t align_up(size_t value, size_t align) {
    if (align <= 1)
        return value;
    return ((value + align - 1) / align) * align;
}

static int32_t alloc_temp_slot(x86_compile_ctx_t *ctx, size_t size, size_t align) {
    if (size < 8) size = 8;
    if (align < 8) align = 8;
    ctx->stack_size = (uint32_t)align_up(ctx->stack_size, align);
    ctx->stack_size += (uint32_t)size;
    return -(int32_t)ctx->stack_size;
}

static bool uses_internal_sret_abi(const lr_type_t *type) {
    size_t sz;
    if (!type)
        return false;
    if (type->kind != LR_TYPE_STRUCT && type->kind != LR_TYPE_ARRAY)
        return false;
    sz = lr_type_size(type);
    return sz > 8;
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
        if ((uint32_t)size <= ctx->stack_slot_sizes[vreg])
            return ctx->stack_slots[vreg];
        /* Existing slot too small — allocate a larger one (old becomes dead) */
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
    if (size == 1) {
        switch (opcode) {
        case 0x01: opcode = 0x00; break; /* add r/m8, r8 */
        case 0x09: opcode = 0x08; break; /* or r/m8, r8 */
        case 0x21: opcode = 0x20; break; /* and r/m8, r8 */
        case 0x29: opcode = 0x28; break; /* sub r/m8, r8 */
        case 0x31: opcode = 0x30; break; /* xor r/m8, r8 */
        case 0x39: opcode = 0x38; break; /* cmp r/m8, r8 */
        case 0x85: opcode = 0x84; break; /* test r/m8, r8 */
        case 0x89: opcode = 0x88; break; /* mov r/m8, r8 */
        default: break;
        }
    }
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
    /* Static allocas: emit LEA inline instead of loading from slot.
       This handles the case where the alloca instruction itself is in
       unreachable code (e.g., placed after a deferred branch) but later
       instructions reference the vreg. */
    int32_t alloca_off = lr_target_lookup_static_alloca_offset(
        ctx->static_alloca_offsets, ctx->num_static_alloca_offsets, vreg);
    if (alloca_off != 0) {
        encode_mem(ctx->buf, &ctx->pos, ctx->buflen, 0x8D,
                   reg, X86_RBP, alloca_off, 8);
        set_cached_reg_vreg(ctx, reg, vreg);
        return;
    }
    int32_t off = alloc_slot(ctx, vreg, 8, 8);
    encode_mem(ctx->buf, &ctx->pos, ctx->buflen, 0x8B, reg, X86_RBP, off, 8);
    set_cached_reg_vreg(ctx, reg, vreg);
}

/* Emit: mov [rbp + offset], reg (store reg to vreg stack slot) */
static void emit_store_slot(x86_compile_ctx_t *ctx, uint32_t vreg, uint8_t reg) {
    int32_t off = alloc_slot(ctx, vreg, 8, 8);
    encode_mem(ctx->buf, &ctx->pos, ctx->buflen, 0x89, reg, X86_RBP, off, 8);
    set_cached_reg_vreg(ctx, reg, vreg);
}

/* Emit an immediate into a GPR. For zero immediates we can use xor reg, reg
 * when flags are not live to reduce code size. */
static void emit_mov_imm(x86_compile_ctx_t *ctx, uint8_t dst, int64_t imm,
                         bool preserve_flags) {
    if (imm == 0 && !preserve_flags) {
        if (dst >= 8) {
            emit_byte(ctx->buf, &ctx->pos, ctx->buflen,
                      rex(false, true, false, true));
        }
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x31);
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, dst, dst));
    } else if (imm >= INT32_MIN && imm <= INT32_MAX) {
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, rex(true, false, false, dst >= 8));
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0xC7);
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, 0, dst));
        emit_u32(ctx->buf, &ctx->pos, ctx->buflen, (uint32_t)(int32_t)imm);
    } else {
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, rex(true, false, false, dst >= 8));
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen, (uint8_t)(0xB8 + (dst & 7)));
        emit_u64(ctx->buf, &ctx->pos, ctx->buflen, (uint64_t)imm);
    }
    invalidate_cached_reg(ctx, dst);
}

/* Emit: add/sub reg, imm32 (sign-extended) */
static void emit_add_imm(x86_compile_ctx_t *ctx, uint8_t dst, int64_t imm) {
    if (imm == 0)
        return;
    if (imm > INT32_MAX || imm < INT32_MIN) {
        emit_mov_imm(ctx, X86_R11, imm, false);
        encode_alu_rr(ctx->buf, &ctx->pos, ctx->buflen, 0x01, dst, X86_R11, 8);
        invalidate_cached_reg(ctx, dst);
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
    invalidate_cached_reg(ctx, dst);
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

static bool fp_abi_two_lane_aggregate(const lr_type_t *type,
                                      uint8_t *lane_size_out,
                                      uint8_t *lane_count_out) {
    const lr_type_t *elem0 = NULL;
    const lr_type_t *elem1 = NULL;
    uint8_t lane_size = 0;
    uint8_t lane_count = 0;
    if (!type)
        return false;

    if (type->kind == LR_TYPE_STRUCT && type->struc.num_fields == 2) {
        elem0 = type->struc.fields[0];
        elem1 = type->struc.fields[1];
    } else if ((type->kind == LR_TYPE_ARRAY || type->kind == LR_TYPE_VECTOR) &&
               type->array.count == 2) {
        elem0 = type->array.elem;
        elem1 = type->array.elem;
    } else {
        return false;
    }

    if (!is_fp_abi_type(elem0) || !is_fp_abi_type(elem1))
        return false;
    if (elem0->kind != elem1->kind)
        return false;

    lane_size = fp_abi_size(elem0);
    if (lane_size == 8) {
        /* {double,double}: two SSE eightbyte lanes */
        lane_count = 2;
    } else if (lane_size == 4) {
        /* {float,float} / <2 x float>: packed in one 64-bit SSE lane */
        lane_size = 8;
        lane_count = 1;
    } else {
        return false;
    }

    if (lane_size_out)
        *lane_size_out = lane_size;
    if (lane_count_out)
        *lane_count_out = lane_count;
    return true;
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

static lr_func_t *find_module_function(lr_module_t *mod, const char *name) {
    if (!mod || !name) return NULL;
    for (lr_func_t *f = mod->first_func; f; f = f->next) {
        if (strcmp(f->name, name) == 0)
            return f;
    }
    return NULL;
}


static bool emit_copy_from_cached_scratch(x86_compile_ctx_t *ctx,
                                          uint32_t vreg, uint8_t dst_reg) {
    uint8_t src_reg;

    if (!ctx)
        return false;
    if (dst_reg == X86_RAX && cached_reg_holds_vreg(ctx, X86_RCX, vreg)) {
        src_reg = X86_RCX;
    } else if (dst_reg == X86_RCX &&
               cached_reg_holds_vreg(ctx, X86_RAX, vreg)) {
        src_reg = X86_RAX;
    } else {
        return false;
    }

    encode_alu_rr(ctx->buf, &ctx->pos, ctx->buflen, 0x89, dst_reg, src_reg, 8);
    set_cached_reg_vreg(ctx, dst_reg, vreg);
    return true;
}

/* Load an operand value into a GPR */
static void emit_load_operand(x86_compile_ctx_t *ctx,
                               const lr_operand_t *op, uint8_t reg) {
    bool preserve_flags = ctx && ctx->current_inst &&
                          ctx->current_inst->op == LR_OP_SELECT;
    if (op->kind == LR_VAL_IMM_I64) {
        emit_mov_imm(ctx, reg, op->imm_i64, preserve_flags);
    } else if (op->kind == LR_VAL_VREG) {
        if (cached_reg_holds_vreg(ctx, reg, op->vreg))
            return;
        if (emit_copy_from_cached_scratch(ctx, op->vreg, reg))
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
        emit_mov_imm(ctx, reg, imm_bits, preserve_flags);
    } else if (op->kind == LR_VAL_NULL || op->kind == LR_VAL_UNDEF) {
        emit_mov_imm(ctx, reg, 0, preserve_flags);
    } else if (op->kind == LR_VAL_GLOBAL && ctx->jit && !ctx->obj_ctx) {
        const char *sym_name = lr_module_symbol_name(ctx->mod,
                                                      op->global_id);
        void *addr = NULL;
        if (sym_name)
            addr = lr_jit_get_function(ctx->jit, sym_name);
        int64_t val = (int64_t)(uintptr_t)addr;
        if (op->global_offset != 0)
            val += op->global_offset;
        emit_mov_imm(ctx, reg, val, preserve_flags);
        invalidate_cached_reg(ctx, reg);
    } else if (op->kind == LR_VAL_GLOBAL && ctx->obj_ctx) {
        const char *sym_name = lr_module_symbol_name(ctx->mod,
                                                      op->global_id);
        if (!sym_name) {
            emit_mov_imm(ctx, reg, 0, preserve_flags);
            return;
        }
        uint32_t sym_idx = lr_obj_ensure_symbol(ctx->obj_ctx, sym_name,
                                                false, 0, 0);
        if (sym_idx == UINT32_MAX) {
            emit_mov_imm(ctx, reg, 0, preserve_flags);
            return;
        }

        if (ctx->jit) {
            /*
             * DIRECT/session JIT paths can map code/data beyond rel32 reach.
             * Emit absolute relocations there to avoid out-of-range failures.
             */
            emit_byte(ctx->buf, &ctx->pos, ctx->buflen,
                      rex(true, false, false, reg >= 8));
            emit_byte(ctx->buf, &ctx->pos, ctx->buflen,
                      (uint8_t)(0xB8 + (reg & 7)));
            size_t imm_off = ctx->pos;
            emit_u64(ctx->buf, &ctx->pos, ctx->buflen, 0);
            lr_obj_add_reloc(ctx->obj_ctx, (uint32_t)imm_off, sym_idx,
                             LR_RELOC_X86_64_64);
        } else {
            bool defined = false;
            if (op->global_id < ctx->sym_count)
                defined = ctx->sym_defined[op->global_id] != 0;
            else
                defined = is_symbol_defined_in_module(ctx->mod, sym_name);

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
        }
        if (op->global_offset != 0)
            emit_add_imm(ctx, reg, op->global_offset);
        invalidate_cached_reg(ctx, reg);
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

static void emit_load_fp_mem_base(x86_compile_ctx_t *ctx, uint8_t base,
                                   int32_t off, uint8_t fpreg, uint8_t fsize) {
    uint8_t prefix = (fsize == 8) ? 0xF2 : 0xF3;
    encode_sse_mem(ctx->buf, &ctx->pos, ctx->buflen, prefix, 0x10, 0,
                   fpreg, base, off);
}

static void emit_store_fp_mem_base(x86_compile_ctx_t *ctx, uint8_t base,
                                    int32_t off, uint8_t fpreg, uint8_t fsize) {
    uint8_t prefix = (fsize == 8) ? 0xF2 : 0xF3;
    encode_sse_mem(ctx->buf, &ctx->pos, ctx->buflen, prefix, 0x11, 0,
                   fpreg, base, off);
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

typedef struct x86_fpagg_src {
    bool zero;
    bool imm;
    lr_operand_t imm_op;
    bool by_ptr;
    int32_t rbp_off;
    uint8_t ptr_reg;
} x86_fpagg_src_t;

static bool x86_fp_aggregate_layout(const lr_type_t *ty,
                                    const lr_type_t **elem_ty_out,
                                    uint64_t *count_out,
                                    uint8_t *elem_size_out,
                                    size_t *total_size_out) {
    const lr_type_t *elem_ty;
    uint64_t count;
    size_t elem_sz;
    size_t total_sz;
    if (!ty)
        return false;
    if (ty->kind != LR_TYPE_ARRAY && ty->kind != LR_TYPE_VECTOR)
        return false;
    elem_ty = ty->array.elem;
    count = ty->array.count;
    if (!elem_ty || count == 0)
        return false;
    if (elem_ty->kind != LR_TYPE_FLOAT && elem_ty->kind != LR_TYPE_DOUBLE)
        return false;
    elem_sz = lr_type_size(elem_ty);
    total_sz = lr_type_size(ty);
    if ((elem_sz != 4 && elem_sz != 8) || total_sz == 0)
        return false;
    if (elem_ty_out) *elem_ty_out = elem_ty;
    if (count_out) *count_out = count;
    if (elem_size_out) *elem_size_out = (uint8_t)elem_sz;
    if (total_size_out) *total_size_out = total_sz;
    return true;
}

static void x86_fpagg_init_src(x86_compile_ctx_t *cc, const lr_operand_t *op,
                               size_t total_sz, uint8_t ptr_reg,
                               x86_fpagg_src_t *out) {
    memset(out, 0, sizeof(*out));
    out->ptr_reg = ptr_reg;
    if (!op || op->kind == LR_VAL_UNDEF || op->kind == LR_VAL_NULL) {
        out->zero = true;
        return;
    }
    if (op->kind == LR_VAL_IMM_F64 || op->kind == LR_VAL_IMM_I64) {
        out->imm = true;
        out->imm_op = *op;
        return;
    }
    if (op->kind == LR_VAL_VREG) {
        size_t src_sz = 0;
        int32_t src_off = alloc_slot(cc, op->vreg, 8, 8);
        if (op->vreg < cc->num_stack_slots)
            src_sz = cc->stack_slot_sizes[op->vreg];
        if (src_sz >= total_sz) {
            out->rbp_off = src_off;
            return;
        }
        if (src_sz == 8) {
            encode_mem(cc->buf, &cc->pos, cc->buflen, 0x8B,
                       ptr_reg, X86_RBP, src_off, 8);
            out->by_ptr = true;
            return;
        }
        out->zero = true;
        return;
    }
    /* Fallback: treat operand as pointer to aggregate bytes. */
    emit_load_operand(cc, op, ptr_reg);
    out->by_ptr = true;
}

static void x86_fpagg_load_elem(x86_compile_ctx_t *cc,
                                const x86_fpagg_src_t *src,
                                int32_t elem_off, uint8_t fpreg,
                                uint8_t fsize) {
    if (!src || src->zero) {
        encode_sse_rr(cc->buf, &cc->pos, cc->buflen, 0x66, 0x57, 0,
                      fpreg, fpreg);
        return;
    }
    if (src->imm) {
        emit_load_fp_operand(cc, &src->imm_op, fpreg, fsize);
        return;
    }
    if (src->by_ptr) {
        emit_load_fp_mem_base(cc, src->ptr_reg, elem_off, fpreg, fsize);
        return;
    }
    emit_load_fp_mem_base(cc, X86_RBP, src->rbp_off + elem_off, fpreg, fsize);
}

static const lr_type_t *call_arg_abi_type(const lr_func_t *callee_func,
                                          uint32_t arg_index,
                                          const lr_operand_t *arg_op) {
    if (callee_func && callee_func->param_types &&
        arg_index < callee_func->num_params) {
        return callee_func->param_types[arg_index];
    }
    return arg_op ? arg_op->type : NULL;
}

static void emit_load_external_fp_call_arg(x86_compile_ctx_t *cc,
                                           const lr_operand_t *op,
                                           const lr_type_t *abi_type,
                                           uint8_t fpreg) {
    uint8_t fsize = fp_abi_size(abi_type);
    if (!cc)
        return;
    if (!op) {
        encode_sse_rr(cc->buf, &cc->pos, cc->buflen, 0x66, 0x57, 0,
                      fpreg, fpreg);
        return;
    }
    if (op->kind == LR_VAL_UNDEF || op->kind == LR_VAL_NULL) {
        encode_sse_rr(cc->buf, &cc->pos, cc->buflen, 0x66, 0x57, 0,
                      fpreg, fpreg);
        return;
    }
    if (op->type && op->type->kind == LR_TYPE_PTR) {
        emit_load_operand(cc, op, X86_R10);
        emit_load_fp_mem_base(cc, X86_R10, 0, fpreg, fsize);
        return;
    }
    emit_load_fp_operand(cc, op, fpreg, fsize);
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

/* Inline encoding helpers for specific MIR-equivalent patterns */

static void emit_imul_rr(x86_compile_ctx_t *ctx, uint8_t dst, uint8_t src, uint8_t size) {
    bool need_rex = (size == 8) || (dst >= 8) || (src >= 8);
    if (need_rex)
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen,
                  rex(size == 8, dst >= 8, false, src >= 8));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x0F);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0xAF);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, dst, src));
    invalidate_cached_reg(ctx, dst);
}

static void emit_idiv_r(x86_compile_ctx_t *ctx, uint8_t src, uint8_t size) {
    bool need_rex = (size == 8) || (src >= 8);
    if (need_rex)
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen,
                  rex(size == 8, false, false, src >= 8));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0xF7);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, 7, src));
    invalidate_cached_gprs(ctx);
}

static void emit_shift(x86_compile_ctx_t *ctx, uint8_t ext, uint8_t dst, uint8_t size) {
    bool need_rex = (size == 8) || (dst >= 8);
    if (need_rex)
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen,
                  rex(size == 8, false, false, dst >= 8));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0xD3);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, ext, dst));
    invalidate_cached_reg(ctx, dst);
}

static void emit_setcc(x86_compile_ctx_t *ctx, uint8_t cc, uint8_t dst) {
    if (cc >= LR_CC_FP_OEQ) {
        emit_fp_setcc(ctx->buf, &ctx->pos, ctx->buflen, cc, dst);
        invalidate_cached_reg(ctx, X86_RCX);
    } else {
        uint8_t x86cc = lr_cc_to_x86(cc);
        emit_setcc_byte(ctx->buf, &ctx->pos, ctx->buflen, x86cc, dst);
    }
    invalidate_cached_reg(ctx, dst);
}

static void emit_movzx_rr(x86_compile_ctx_t *ctx, uint8_t dst, uint8_t src, uint8_t size) {
    bool need_rex = (dst >= 8) || (src >= 8);
    if (need_rex)
        emit_byte(ctx->buf, &ctx->pos, ctx->buflen,
                  rex(false, dst >= 8, false, src >= 8));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x0F);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, (size == 1) ? 0xB6 : 0xB7);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, dst, src));
    invalidate_cached_reg(ctx, dst);
}

static void emit_movsx_rr(x86_compile_ctx_t *ctx, uint8_t dst, uint8_t src, uint8_t size) {
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen,
              rex(true, dst >= 8, false, src >= 8));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x0F);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, (size == 1) ? 0xBE : 0xBF);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, dst, src));
    invalidate_cached_reg(ctx, dst);
}

static void emit_movsxd(x86_compile_ctx_t *ctx, uint8_t dst, uint8_t src);

static void emit_sign_extend_value(x86_compile_ctx_t *ctx, uint8_t reg,
                                   uint8_t bits) {
    if (!ctx || bits == 0 || bits >= 64)
        return;
    if (bits == 1) {
        emit_mov_imm(ctx, X86_R11, 1, false);
        encode_alu_rr(ctx->buf, &ctx->pos, ctx->buflen, 0x21, reg, X86_R11, 8);
        emit_mov_imm(ctx, X86_R11, 0, false);
        encode_alu_rr(ctx->buf, &ctx->pos, ctx->buflen, 0x29, X86_R11, reg, 8);
        encode_alu_rr(ctx->buf, &ctx->pos, ctx->buflen, 0x89, reg, X86_R11, 8);
        invalidate_cached_reg(ctx, reg);
        invalidate_cached_reg(ctx, X86_R11);
        return;
    }
    if (bits <= 8) {
        emit_movsx_rr(ctx, reg, reg, 1);
        return;
    }
    if (bits <= 16) {
        emit_movsx_rr(ctx, reg, reg, 2);
        return;
    }
    if (bits <= 32) {
        emit_movsxd(ctx, reg, reg);
        return;
    }
    {
        uint8_t sh = (uint8_t)(64 - bits);
        if (reg != X86_RCX) {
            emit_mov_imm(ctx, X86_RCX, (int64_t)sh, false);
            emit_shift(ctx, 4, reg, 8);
            emit_shift(ctx, 7, reg, 8);
            return;
        }
        encode_alu_rr(ctx->buf, &ctx->pos, ctx->buflen, 0x89,
                      X86_R11, X86_RCX, 8);
        emit_mov_imm(ctx, X86_RCX, (int64_t)sh, false);
        emit_shift(ctx, 4, X86_R11, 8);
        emit_shift(ctx, 7, X86_R11, 8);
        encode_alu_rr(ctx->buf, &ctx->pos, ctx->buflen, 0x89,
                      X86_RCX, X86_R11, 8);
        return;
    }
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
    invalidate_cached_reg(ctx, dst);
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
    emit_mov_imm(ctx, X86_RAX, 0, false);
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

static void emit_load_vreg_mem_sized(x86_compile_ctx_t *ctx, uint32_t src_vreg,
                                     int32_t add_off, uint8_t reg, uint8_t size) {
    int32_t src_off = alloc_slot(ctx, src_vreg, 8, 8) + add_off;
    emit_mem_load_sized(ctx, reg, X86_RBP, src_off, size);
}

static size_t vreg_slot_size(const x86_compile_ctx_t *ctx, uint32_t vreg) {
    if (!ctx || vreg >= ctx->num_stack_slots || ctx->stack_slot_sizes[vreg] == 0)
        return 8;
    return (size_t)ctx->stack_slot_sizes[vreg];
}

static bool vreg_uses_indirect_aggregate_storage(x86_compile_ctx_t *ctx,
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

static void emit_copy_vreg_value_bytes_to_base(x86_compile_ctx_t *ctx,
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
                                   X86_RBP, alloca_off, value_sz);
        return;
    }

    src_off = alloc_slot(ctx, src_vreg, 8, 8);
    src_sz = vreg_slot_size(ctx, src_vreg);
    if (src_sz >= value_sz) {
        emit_mem_copy_base_to_base(ctx, dst_base, dst_disp,
                                   X86_RBP, src_off, value_sz);
        return;
    }

    if (src_sz == 8 && value_sz > 8) {
        emit_mem_load_sized(ctx, X86_R10, X86_RBP, src_off, 8);
        emit_mem_copy_base_to_base(ctx, dst_base, dst_disp,
                                   X86_R10, 0, value_sz);
        return;
    }

    if (src_sz > 0) {
        emit_mem_copy_base_to_base(ctx, dst_base, dst_disp,
                                   X86_RBP, src_off, src_sz);
    }
    if (src_sz < value_sz) {
        emit_mem_zero_base(ctx, dst_base, dst_disp + (int32_t)src_sz,
                           value_sz - src_sz);
    }
}

static void emit_phi_copy_value(x86_compile_ctx_t *cc,
                                uint32_t dest_vreg,
                                const lr_operand_t *src_op) {
    size_t dst_sz = vreg_slot_size(cc, dest_vreg);
    int32_t dst_off;
    if (!cc || !src_op)
        return;
    if (dst_sz <= 8) {
        emit_load_operand(cc, src_op, X86_RAX);
        emit_store_slot(cc, dest_vreg, X86_RAX);
        return;
    }

    dst_off = alloc_slot(cc, dest_vreg, dst_sz, 8);
    if (src_op->kind == LR_VAL_VREG) {
        emit_copy_vreg_value_bytes_to_base(cc, src_op->vreg, dst_sz,
                                           X86_RBP, dst_off);
        return;
    }
    if (src_op->kind == LR_VAL_UNDEF || src_op->kind == LR_VAL_NULL) {
        emit_mem_zero_base(cc, X86_RBP, dst_off, dst_sz);
        return;
    }

    emit_load_operand(cc, src_op, X86_RAX);
    emit_mem_store_sized(cc, X86_RAX, X86_RBP, dst_off, 8);
    emit_mem_zero_base(cc, X86_RBP, dst_off + 8, dst_sz - 8);
}

static void emit_jmp_sourced(x86_compile_ctx_t *ctx, uint32_t target_block,
                             uint32_t source_block) {
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0xE9);
    if (ctx->num_fixups < ctx->fixup_cap) {
        ctx->fixups[ctx->num_fixups].pos = ctx->pos;
        ctx->fixups[ctx->num_fixups].target = target_block;
        ctx->fixups[ctx->num_fixups].source = source_block;
        ctx->num_fixups++;
    }
    emit_u32(ctx->buf, &ctx->pos, ctx->buflen, 0);
}

static void emit_jmp(x86_compile_ctx_t *ctx, uint32_t target_block) {
    emit_jmp_sourced(ctx, target_block, UINT32_MAX);
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
    invalidate_cached_reg(ctx, gpr);
}

static void emit_movsxd(x86_compile_ctx_t *ctx, uint8_t dst, uint8_t src) {
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, rex(true, dst >= 8, false, src >= 8));
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, 0x63);
    emit_byte(ctx->buf, &ctx->pos, ctx->buflen, modrm(3, dst, src));
    invalidate_cached_reg(ctx, dst);
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
    invalidate_cached_reg(ctx, dst);
}

/* ---- Streaming direct-emission ISel ------------------------------------ */

typedef struct x86_stream_phi_copy {
    uint32_t pred_block_id;
    uint32_t succ_block_id;
    uint32_t dest_vreg;
    lr_operand_t src_op;
    bool emitted;
} x86_stream_phi_copy_t;

/* Saved terminator for deferred emission (allows phi copies registered
   after the terminator to be included when the next block starts). */
typedef struct x86_deferred_term {
    bool pending;
    lr_opcode_t op;
    lr_type_t *type;
    uint32_t dest;
    lr_operand_t ops[4];
    uint32_t num_ops;
    uint32_t block_id;
} x86_deferred_term_t;

typedef struct x86_direct_ctx {
    x86_compile_ctx_t cc;
    size_t prologue_patch_pos;
    lr_compile_mode_t mode;
    uint32_t current_block_id;
    bool has_current_block;
    bool block_offset_pending;
    uint32_t next_vreg;
    lr_type_t *ret_type;
    x86_stream_phi_copy_t *phi_copies;
    uint32_t phi_copy_count;
    uint32_t phi_copy_cap;
    x86_deferred_term_t deferred;
} x86_direct_ctx_t;

static lr_operand_t operand_from_desc(const lr_operand_desc_t *desc) {
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

static void direct_note_vregs(x86_direct_ctx_t *ctx,
                              const lr_compile_inst_desc_t *desc) {
    if (desc->dest != 0 && desc->dest >= ctx->next_vreg)
        ctx->next_vreg = desc->dest + 1u;
    for (uint32_t i = 0; i < desc->num_operands; i++) {
        if (desc->operands[i].kind == LR_OP_KIND_VREG &&
            desc->operands[i].vreg >= ctx->next_vreg)
            ctx->next_vreg = desc->operands[i].vreg + 1u;
    }
}

static int direct_ensure_fixup_cap(x86_direct_ctx_t *ctx) {
    x86_compile_ctx_t *cc = &ctx->cc;
    if (cc->num_fixups < cc->fixup_cap)
        return 0;
    uint32_t new_cap = cc->fixup_cap == 0 ? 16u : cc->fixup_cap * 2u;
    x86_fixup_t *nf = lr_arena_array_uninit(cc->arena, x86_fixup_t, new_cap);
    if (!nf)
        return -1;
    if (cc->fixup_cap > 0)
        memcpy(nf, cc->fixups, sizeof(x86_fixup_t) * cc->fixup_cap);
    cc->fixups = nf;
    cc->fixup_cap = new_cap;
    return 0;
}

static int direct_ensure_block_offsets(x86_direct_ctx_t *ctx,
                                       uint32_t block_id) {
    x86_compile_ctx_t *cc = &ctx->cc;
    if (block_id < cc->num_block_offsets)
        return 0;
    uint32_t new_cap = cc->num_block_offsets == 0 ? 8u : cc->num_block_offsets;
    while (new_cap <= block_id)
        new_cap *= 2u;
    size_t *nb = lr_arena_array_uninit(cc->arena, size_t, new_cap);
    if (!nb)
        return -1;
    if (cc->num_block_offsets > 0)
        memcpy(nb, cc->block_offsets,
               sizeof(size_t) * cc->num_block_offsets);
    for (uint32_t i = cc->num_block_offsets; i < new_cap; i++)
        nb[i] = SIZE_MAX;
    cc->block_offsets = nb;
    cc->num_block_offsets = new_cap;
    return 0;
}

static int direct_ensure_phi_copy_cap(x86_direct_ctx_t *ctx) {
    if (ctx->phi_copy_count < ctx->phi_copy_cap)
        return 0;
    uint32_t new_cap = ctx->phi_copy_cap == 0 ? 8u : ctx->phi_copy_cap * 2u;
    x86_stream_phi_copy_t *np = lr_arena_array_uninit(
        ctx->cc.arena, x86_stream_phi_copy_t, new_cap);
    if (!np)
        return -1;
    if (ctx->phi_copy_cap > 0)
        memcpy(np, ctx->phi_copies,
               sizeof(x86_stream_phi_copy_t) * ctx->phi_copy_cap);
    ctx->phi_copies = np;
    ctx->phi_copy_cap = new_cap;
    return 0;
}

static void direct_emit_phi_copies_for_edge(x86_direct_ctx_t *ctx,
                                            uint32_t pred,
                                            uint32_t succ) {
    x86_compile_ctx_t *cc = &ctx->cc;
    uint32_t stage_base = ctx->next_vreg;
    uint32_t staged = 0;

    /* PHI inputs are parallel: stage sources first, then write destinations. */
    for (uint32_t i = 0; i < ctx->phi_copy_count; i++) {
        size_t dst_sz;
        int32_t tmp_off;
        uint32_t tmp_vreg;
        const lr_operand_t *src_op;

        if (ctx->phi_copies[i].pred_block_id != pred ||
            ctx->phi_copies[i].succ_block_id != succ)
            continue;

        src_op = &ctx->phi_copies[i].src_op;
        dst_sz = vreg_slot_size(cc, ctx->phi_copies[i].dest_vreg);
        if (dst_sz < 8)
            dst_sz = 8;

        tmp_vreg = stage_base + staged;
        ctx->next_vreg = tmp_vreg + 1u;
        tmp_off = alloc_slot(cc, tmp_vreg, dst_sz, 8);

        if (dst_sz <= 8) {
            emit_load_operand(cc, src_op, X86_RAX);
            emit_store_slot(cc, tmp_vreg, X86_RAX);
        } else if (src_op->kind == LR_VAL_VREG) {
            emit_copy_vreg_value_bytes_to_base(cc, src_op->vreg, dst_sz,
                                               X86_RBP, tmp_off);
        } else if (src_op->kind == LR_VAL_UNDEF ||
                   src_op->kind == LR_VAL_NULL) {
            emit_mem_zero_base(cc, X86_RBP, tmp_off, dst_sz);
        } else {
            emit_load_operand(cc, src_op, X86_RAX);
            emit_mem_store_sized(cc, X86_RAX, X86_RBP, tmp_off, 8);
            emit_mem_zero_base(cc, X86_RBP, tmp_off + 8, dst_sz - 8);
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
        memset(&staged_src, 0, sizeof(staged_src));
        staged_src.kind = LR_VAL_VREG;
        staged_src.type = ctx->phi_copies[i].src_op.type;
        staged_src.vreg = stage_base + staged;
        emit_phi_copy_value(cc, ctx->phi_copies[i].dest_vreg, &staged_src);
        ctx->phi_copies[i].emitted = true;
        staged++;
    }
}

/* Flush a deferred terminator (BR, CONDBR, RET, RET_VOID) that was
   saved during compile_emit. Phi copies are edge-specific; unconditional
   branches can emit them directly, while conditional branches use late
   edge stubs in compile_end(). */
static int flush_deferred_terminator(x86_direct_ctx_t *ctx) {
    x86_compile_ctx_t *cc;
    x86_deferred_term_t *dt;

    if (!ctx || !ctx->deferred.pending)
        return 0;

    cc = &ctx->cc;
    dt = &ctx->deferred;
    dt->pending = false;
    switch (dt->op) {
    case LR_OP_RET:
        if (cc->func_uses_internal_sret) {
            size_t ret_sz = lr_type_size(ctx->ret_type);
            emit_mem_load_sized(cc, X86_RDI, X86_RBP, cc->sret_ptr_off, 8);
            if (ret_sz == 0)
                ret_sz = 8;
            if (dt->ops[0].kind == LR_VAL_VREG) {
                uint32_t vreg = dt->ops[0].vreg;
                int32_t alloca_off = lr_target_lookup_static_alloca_offset(
                    cc->static_alloca_offsets,
                    cc->num_static_alloca_offsets, vreg);
                if (alloca_off != 0) {
                    emit_mem_copy_base_to_base(cc, X86_RDI, 0,
                                               X86_RBP, alloca_off,
                                               ret_sz);
                } else {
                    size_t src_sz = 0;
                    int32_t src_off = alloc_slot(cc, vreg, 8, 8);
                    if (vreg < cc->num_stack_slots)
                        src_sz = cc->stack_slot_sizes[vreg];
                    if (src_sz >= ret_sz) {
                        emit_mem_copy_base_to_base(cc, X86_RDI, 0,
                                                   X86_RBP, src_off,
                                                   ret_sz);
                    } else if (src_sz == 8) {
                        encode_mem(cc->buf, &cc->pos, cc->buflen, 0x8B,
                                   X86_RAX, X86_RBP, src_off, 8);
                        emit_mem_copy_base_to_base(cc, X86_RDI, 0,
                                                   X86_RAX, 0, ret_sz);
                    } else {
                        if (src_sz > 0)
                            emit_mem_copy_base_to_base(cc, X86_RDI, 0,
                                                       X86_RBP, src_off,
                                                       src_sz);
                        if (src_sz < ret_sz)
                            emit_mem_zero_base(cc, X86_RDI, (int32_t)src_sz,
                                               ret_sz - src_sz);
                    }
                }
            } else if (dt->ops[0].kind == LR_VAL_UNDEF ||
                       dt->ops[0].kind == LR_VAL_NULL) {
                emit_mem_zero_base(cc, X86_RDI, 0, ret_sz);
            } else if (ret_sz <= 8) {
                emit_load_operand(cc, &dt->ops[0], X86_RAX);
                emit_mem_store_sized(cc, X86_RAX, X86_RDI, 0,
                                     (uint8_t)ret_sz);
            } else {
                emit_mem_zero_base(cc, X86_RDI, 0, ret_sz);
            }
            encode_alu_rr(cc->buf, &cc->pos, cc->buflen, 0x89,
                          X86_RAX, X86_RDI, 8);
        } else if (cc->func_uses_external_sysv_fp &&
                   ctx->ret_type && is_fp_abi_type(ctx->ret_type)) {
            emit_load_fp_operand(cc, &dt->ops[0], X86_XMM0,
                                 fp_abi_size(ctx->ret_type));
        } else {
            uint8_t ret_lane_size = 0;
            uint8_t ret_lane_count = 0;
            bool ret_fp_agg = cc->func_uses_external_sysv_fp &&
                              fp_abi_two_lane_aggregate(ctx->ret_type,
                                                        &ret_lane_size,
                                                        &ret_lane_count);
            if (ret_fp_agg) {
                size_t ret_sz = lr_type_size(ctx->ret_type);
                x86_fpagg_src_t src;
                if (ret_sz < 8) ret_sz = 8;
                x86_fpagg_init_src(cc, &dt->ops[0], ret_sz, X86_R10, &src);
                x86_fpagg_load_elem(cc, &src, 0, X86_XMM0, ret_lane_size);
                if (ret_lane_count > 1)
                    x86_fpagg_load_elem(cc, &src, (int32_t)ret_lane_size,
                                        X86_XMM1, ret_lane_size);
            } else {
                emit_load_operand(cc, &dt->ops[0], X86_RAX);
            }
        }
        emit_epilogue(cc);
        break;
    case LR_OP_RET_VOID:
        emit_epilogue(cc);
        break;
    case LR_OP_BR: {
        direct_emit_phi_copies_for_edge(ctx, dt->block_id,
                                        dt->ops[0].block_id);
        if (direct_ensure_fixup_cap(ctx) != 0) return -1;
        uint32_t target_id = dt->ops[0].block_id;
        emit_jmp_sourced(cc, target_id, dt->block_id);
        break;
    }
    case LR_OP_CONDBR: {
        uint32_t true_id;
        uint32_t false_id;
        size_t jcc_disp_pos;
        size_t true_path_pos;
        int32_t rel32;
        uint8_t x86cc;
        emit_load_operand(cc, &dt->ops[0], X86_RAX);
        encode_alu_rr(cc->buf, &cc->pos, cc->buflen, 0x85,
                      X86_RAX, X86_RAX, 1);

        true_id = dt->ops[1].block_id;
        false_id = dt->ops[2].block_id;

        /* Emit edge-specific copies:
           test; jne true_path; false_copies; jmp false; true_path: true_copies; jmp true */
        x86cc = lr_cc_to_x86(LR_CC_NE);
        emit_byte(cc->buf, &cc->pos, cc->buflen, 0x0F);
        emit_byte(cc->buf, &cc->pos, cc->buflen, (uint8_t)(0x80 + x86cc));
        jcc_disp_pos = cc->pos;
        emit_u32(cc->buf, &cc->pos, cc->buflen, 0);

        direct_emit_phi_copies_for_edge(ctx, dt->block_id, false_id);
        if (direct_ensure_fixup_cap(ctx) != 0) return -1;
        emit_jmp_sourced(cc, false_id, dt->block_id);

        true_path_pos = cc->pos;
        rel32 = (int32_t)((int64_t)true_path_pos -
                          (int64_t)(jcc_disp_pos + 4));
        if (jcc_disp_pos + 4 <= cc->buflen) {
            cc->buf[jcc_disp_pos + 0] = (uint8_t)(rel32);
            cc->buf[jcc_disp_pos + 1] = (uint8_t)(rel32 >> 8);
            cc->buf[jcc_disp_pos + 2] = (uint8_t)(rel32 >> 16);
            cc->buf[jcc_disp_pos + 3] = (uint8_t)(rel32 >> 24);
        }

        direct_emit_phi_copies_for_edge(ctx, dt->block_id, true_id);
        if (direct_ensure_fixup_cap(ctx) != 0) return -1;
        emit_jmp_sourced(cc, true_id, dt->block_id);
        break;
    }
    default:
        break;
    }
    return 0;
}

static bool direct_call_uses_external_sysv_abi(x86_compile_ctx_t *cc,
                                               const lr_operand_t *callee_op,
                                               bool call_external_abi,
                                               bool call_vararg,
                                               lr_func_t **callee_func_out,
                                               bool *out_vararg) {
    lr_func_t *callee_func = NULL;
    if (callee_func_out) *callee_func_out = NULL;
    if (out_vararg) *out_vararg = call_vararg;

    if (!cc || !cc->mod || !callee_op)
        return false;

    if (callee_op->kind == LR_VAL_GLOBAL) {
        if (callee_op->global_id < cc->sym_count) {
            callee_func = cc->sym_funcs[callee_op->global_id];
            if (callee_func_out) *callee_func_out = callee_func;
            if (callee_func) {
                if (out_vararg)
                    *out_vararg = callee_func->vararg || call_vararg;
                return callee_func->first_block == NULL ||
                       callee_func->uses_llvm_abi;
            }
            return cc->sym_defined[callee_op->global_id] == 0;
        }
        const char *sym_name = lr_module_symbol_name(cc->mod,
                                                      callee_op->global_id);
        if (!sym_name) return false;
        callee_func = find_module_function(cc->mod, sym_name);
        if (callee_func_out) *callee_func_out = callee_func;
        if (callee_func) {
            if (out_vararg)
                *out_vararg = callee_func->vararg || call_vararg;
            return callee_func->first_block == NULL ||
                   callee_func->uses_llvm_abi;
        }
        return !is_symbol_defined_in_module(cc->mod, sym_name);
    }

    if (out_vararg) *out_vararg = call_vararg;
    return call_external_abi;
}

static bool function_uses_external_sysv_fp_abi(const lr_compile_func_meta_t *func_meta) {
    if (!func_meta || !func_meta->func)
        return false;
    return func_meta->func->uses_llvm_abi;
}

static int x86_64_compile_begin(void **compile_ctx,
                                const lr_compile_func_meta_t *func_meta,
                                lr_module_t *mod,
                                uint8_t *buf, size_t buflen,
                                lr_arena_t *arena) {
    static const uint8_t param_regs[] = {
        X86_RDI, X86_RSI, X86_RDX, X86_RCX, X86_R8, X86_R9
    };
    static const uint8_t param_fp_regs[] = {
        X86_XMM0, X86_XMM1, X86_XMM2, X86_XMM3,
        X86_XMM4, X86_XMM5, X86_XMM6, X86_XMM7
    };
    x86_direct_ctx_t *ctx = NULL;
    uint32_t initial_slots;
    uint32_t *param_vregs = NULL;
    uint32_t num_params;
    bool vararg;
    lr_type_t *ret_type;

    if (!compile_ctx || !func_meta || !mod || !arena)
        return -1;

    ctx = lr_arena_new(arena, x86_direct_ctx_t);
    if (!ctx)
        return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->mode = func_meta->mode;
    ctx->next_vreg = func_meta->next_vreg;
    ret_type = func_meta->ret_type ? func_meta->ret_type : mod->type_void;
    ctx->ret_type = ret_type;
    num_params = func_meta->num_params;
    vararg = func_meta->vararg;
    lr_type_t **param_types = func_meta->param_types;

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
    x86_compile_ctx_t *cc = &ctx->cc;
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
    cc->num_block_offsets = 8;
    for (uint32_t i = 0; i < 8; i++) cc->block_offsets[i] = SIZE_MAX;
    cc->fixups = lr_arena_array_uninit(arena, x86_fixup_t, 16);
    cc->num_fixups = 0;
    cc->fixup_cap = 16;
    cc->arena = arena;
    cc->obj_ctx = mod ? mod->obj_ctx : NULL;
    cc->mod = mod;
    cc->jit = func_meta ? func_meta->jit : NULL;
    cc->sym_defined = NULL;
    cc->sym_funcs = NULL;
    cc->sym_count = 0;
    cc->rax_holds_vreg = UINT32_MAX;
    cc->rcx_holds_vreg = UINT32_MAX;
    cc->current_inst = NULL;
    cc->func_uses_internal_sret = false;
    cc->sret_ptr_off = 0;
    cc->func_is_vararg = false;
    cc->vararg_rsa_off = 0;
    cc->vararg_named_gp = 0;
    cc->func_uses_external_sysv_fp = function_uses_external_sysv_fp_abi(func_meta);

    attach_obj_symbol_meta_cache(cc);

    ctx->prologue_patch_pos = emit_prologue(cc);

    cc->func_uses_internal_sret = uses_internal_sret_abi(ret_type) &&
                                  !fp_abi_two_lane_aggregate(ret_type, NULL,
                                                             NULL);
    if (cc->func_uses_internal_sret) {
        cc->sret_ptr_off = alloc_temp_slot(cc, 8, 8);
        emit_mem_store_sized(cc, X86_RDI, X86_RBP, cc->sret_ptr_off, 8);
    }

    {
        uint32_t gp_used = cc->func_uses_internal_sret ? 1u : 0u;
        uint32_t fp_used = 0u;
        uint32_t stack_used = 0u;

        for (uint32_t i = 0; i < num_params; i++) {
            const lr_type_t *pty = NULL;
            uint8_t agg_lane_size = 0;
            uint8_t agg_lane_count = 0;
            uint32_t agg_stack_units = 0;
            if (param_types)
                pty = param_types[i];
            if (fp_abi_two_lane_aggregate(pty, &agg_lane_size,
                                          &agg_lane_count))
                agg_stack_units = (uint32_t)(((uint32_t)agg_lane_size *
                                              (uint32_t)agg_lane_count + 7u) / 8u);

            if (cc->func_uses_external_sysv_fp) {
                if (is_fp_abi_type(pty) && fp_used < 8) {
                    emit_store_fp_slot(cc, param_vregs[i], param_fp_regs[fp_used],
                                       fp_abi_size(pty));
                    fp_used++;
                    continue;
                }

                if (agg_stack_units != 0 && fp_used + agg_lane_count <= 8) {
                    size_t dst_sz = lr_type_size(pty);
                    size_t dst_align = lr_type_align(pty);
                    int32_t dst_off;
                    if (dst_align < 8) dst_align = 8;
                    if (dst_sz < 8) dst_sz = 8;
                    dst_off = alloc_slot(cc, param_vregs[i], dst_sz, dst_align);
                    emit_store_fp_mem_base(cc, X86_RBP, dst_off,
                                           param_fp_regs[fp_used], agg_lane_size);
                    if (agg_lane_count > 1 &&
                        dst_sz >= (size_t)(2u * (size_t)agg_lane_size)) {
                        emit_store_fp_mem_base(cc, X86_RBP,
                                               dst_off + (int32_t)agg_lane_size,
                                               param_fp_regs[fp_used + 1],
                                               agg_lane_size);
                    }
                    fp_used += agg_lane_count;
                    continue;
                }

                if (!is_fp_abi_type(pty) && agg_stack_units == 0 && gp_used < 6) {
                    emit_store_slot(cc, param_vregs[i], param_regs[gp_used]);
                    gp_used++;
                    continue;
                }

                {
                    int32_t caller_off = 16 + (int32_t)(stack_used * 8u);
                    if (is_fp_abi_type(pty)) {
                        uint8_t fsize = fp_abi_size(pty);
                        emit_load_fp_mem_base(cc, X86_RBP, caller_off,
                                              FP_SCRATCH0, fsize);
                        emit_store_fp_slot(cc, param_vregs[i], FP_SCRATCH0, fsize);
                        stack_used++;
                    } else if (agg_stack_units != 0) {
                        size_t dst_sz = lr_type_size(pty);
                        size_t dst_align = lr_type_align(pty);
                        int32_t dst_off;
                        if (dst_align < 8) dst_align = 8;
                        if (dst_sz < 8) dst_sz = 8;
                        dst_off = alloc_slot(cc, param_vregs[i], dst_sz, dst_align);
                        emit_load_fp_mem_base(cc, X86_RBP, caller_off,
                                              FP_SCRATCH0, agg_lane_size);
                        emit_store_fp_mem_base(cc, X86_RBP, dst_off,
                                               FP_SCRATCH0, agg_lane_size);
                        if (agg_stack_units > 1 &&
                            dst_sz >= (size_t)(2u * (size_t)agg_lane_size)) {
                            emit_load_fp_mem_base(cc, X86_RBP,
                                                  caller_off + (int32_t)agg_lane_size,
                                                  FP_SCRATCH0, agg_lane_size);
                            emit_store_fp_mem_base(cc, X86_RBP,
                                                   dst_off + (int32_t)agg_lane_size,
                                                   FP_SCRATCH0, agg_lane_size);
                        }
                        stack_used += agg_stack_units;
                    } else {
                        encode_mem(cc->buf, &cc->pos, cc->buflen, 0x8B,
                                   X86_RAX, X86_RBP, caller_off, 8);
                        emit_store_slot(cc, param_vregs[i], X86_RAX);
                        stack_used++;
                    }
                }
                continue;
            }

            if (gp_used < 6) {
                emit_store_slot(cc, param_vregs[i], param_regs[gp_used]);
                gp_used++;
            } else {
                int32_t caller_off = 16 + (int32_t)((gp_used - 6u) * 8u);
                encode_mem(cc->buf, &cc->pos, cc->buflen, 0x8B,
                           X86_RAX, X86_RBP, caller_off, 8);
                emit_store_slot(cc, param_vregs[i], X86_RAX);
                gp_used++;
            }
        }
    }

    cc->func_is_vararg = vararg;
    if (vararg) {
        uint32_t named_gp = num_params;
        if (named_gp > 6) named_gp = 6;
        cc->vararg_named_gp = named_gp;
        cc->vararg_rsa_off = alloc_temp_slot(cc, 48, 8);
        for (uint32_t i = 0; i < 6; i++)
            emit_mem_store_sized(cc, param_regs[i], X86_RBP,
                                 cc->vararg_rsa_off + (int32_t)(i * 8), 8);
    }

    *compile_ctx = ctx;
    return 0;
}

static int x86_64_compile_set_block(void *compile_ctx, uint32_t block_id) {
    x86_direct_ctx_t *ctx = (x86_direct_ctx_t *)compile_ctx;
    if (!ctx)
        return -1;
    /* Flush deferred terminators on block transitions so branch fixups
       are emitted before binding the next block entry. This also guarantees
       empty blocks are assigned offsets instead of leaving placeholder
       branch displacements unresolved. */
    if (ctx->deferred.pending &&
        (!ctx->has_current_block || ctx->deferred.block_id != block_id)) {
        if (flush_deferred_terminator(ctx) != 0)
            return -1;
    }
    if (direct_ensure_block_offsets(ctx, block_id) != 0)
        return -1;
    ctx->current_block_id = block_id;
    ctx->has_current_block = true;
    if (ctx->cc.block_offsets[block_id] == SIZE_MAX)
        ctx->cc.block_offsets[block_id] = ctx->cc.pos;
    /* Entering a new block must invalidate cached register mappings before
       emitting non-PHI instructions, but keep offsets bound for empty blocks. */
    ctx->block_offset_pending = true;
    return 0;
}

static int x86_64_compile_emit(void *compile_ctx,
                               const lr_compile_inst_desc_t *desc) {
    x86_direct_ctx_t *ctx = (x86_direct_ctx_t *)compile_ctx;
    x86_compile_ctx_t *cc;
    lr_operand_t ops_stack[16];
    lr_operand_t *ops = ops_stack;
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
            if (flush_deferred_terminator(ctx) != 0)
                return -1;
        }
        if (ctx->block_offset_pending) {
            ctx->cc.block_offsets[ctx->current_block_id] = ctx->cc.pos;
            invalidate_cached_gprs(&ctx->cc);
        }
        ctx->block_offset_pending = false;
    }

    cc = &ctx->cc;
    direct_note_vregs(ctx, desc);

    if (direct_ensure_fixup_cap(ctx) != 0)
        return -1;

    uint32_t nops = desc->num_operands;
    if (nops > 16) {
        ops = lr_arena_array_uninit(cc->arena, lr_operand_t, nops);
        if (!ops)
            return -1;
    }
    for (uint32_t i = 0; i < nops; i++)
        ops[i] = operand_from_desc(&desc->operands[i]);

    memset(&inst_header, 0, sizeof(inst_header));
    inst_header.op = desc->op;
    inst_header.operands = ops;
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

    cc->current_inst = &inst_header;

    switch (desc->op) {
    case LR_OP_RET: {
        ctx->deferred.pending = true;
        ctx->deferred.op = LR_OP_RET;
        ctx->deferred.ops[0] = ops[0];
        ctx->deferred.num_ops = 1;
        ctx->deferred.block_id = ctx->current_block_id;
        cc->current_inst = NULL;
        return 0;
    }
    case LR_OP_RET_VOID:
        ctx->deferred.pending = true;
        ctx->deferred.op = LR_OP_RET_VOID;
        ctx->deferred.num_ops = 0;
        ctx->deferred.block_id = ctx->current_block_id;
        cc->current_inst = NULL;
        return 0;
    case LR_OP_ADD: case LR_OP_SUB: case LR_OP_AND:
    case LR_OP_OR: case LR_OP_XOR: {
        emit_load_operand(cc, &ops[0], X86_RAX);
        emit_load_operand(cc, &ops[1], X86_RCX);
        uint8_t opcode;
        switch (desc->op) {
        case LR_OP_ADD: opcode = 0x01; break;
        case LR_OP_SUB: opcode = 0x29; break;
        case LR_OP_AND: opcode = 0x21; break;
        case LR_OP_OR:  opcode = 0x09; break;
        case LR_OP_XOR: opcode = 0x31; break;
        default: opcode = 0x01; break;
        }
        encode_alu_rr(cc->buf, &cc->pos, cc->buflen, opcode,
                      X86_RAX, X86_RCX,
                      (uint8_t)lr_type_size(desc->type));
        emit_store_slot(cc, desc->dest, X86_RAX);
        break;
    }
    case LR_OP_MUL: {
        emit_load_operand(cc, &ops[0], X86_RAX);
        emit_load_operand(cc, &ops[1], X86_RCX);
        emit_imul_rr(cc, X86_RAX, X86_RCX,
                     (uint8_t)lr_type_size(desc->type));
        emit_store_slot(cc, desc->dest, X86_RAX);
        break;
    }
    case LR_OP_FADD: case LR_OP_FSUB:
    case LR_OP_FMUL: case LR_OP_FDIV: {
        const lr_type_t *elem_ty = NULL;
        uint64_t elem_count = 0;
        uint8_t elem_sz = 0;
        size_t total_sz = 0;
        if (x86_fp_aggregate_layout(desc->type, &elem_ty, &elem_count,
                                    &elem_sz, &total_sz)) {
            size_t dst_align = lr_type_align(desc->type);
            int32_t dst_off;
            x86_fpagg_src_t src0, src1;
            uint8_t op1;
            if (dst_align < 8) dst_align = 8;
            dst_off = alloc_slot(cc, desc->dest, total_sz, dst_align);
            x86_fpagg_init_src(cc, &ops[0], total_sz, X86_R10, &src0);
            x86_fpagg_init_src(cc, &ops[1], total_sz, X86_R11, &src1);
            switch (desc->op) {
            case LR_OP_FADD: op1 = 0x58; break;
            case LR_OP_FSUB: op1 = 0x5C; break;
            case LR_OP_FMUL: op1 = 0x59; break;
            case LR_OP_FDIV: op1 = 0x5E; break;
            default: op1 = 0x58; break;
            }
            for (uint64_t i = 0; i < elem_count; i++) {
                int32_t off = (int32_t)(i * (uint64_t)elem_sz);
                x86_fpagg_load_elem(cc, &src0, off, FP_SCRATCH0, elem_sz);
                x86_fpagg_load_elem(cc, &src1, off, FP_SCRATCH1, elem_sz);
                emit_sse_arith(cc, op1, FP_SCRATCH0, FP_SCRATCH1, elem_sz);
                emit_store_fp_mem_base(cc, X86_RBP, dst_off + off,
                                       FP_SCRATCH0, elem_sz);
            }
            break;
        }
        uint8_t fsize = (desc->type &&
                         desc->type->kind == LR_TYPE_FLOAT) ? 4 : 8;
        emit_load_fp_operand(cc, &ops[0], FP_SCRATCH0, fsize);
        emit_load_fp_operand(cc, &ops[1], FP_SCRATCH1, fsize);
        uint8_t op1;
        switch (desc->op) {
        case LR_OP_FADD: op1 = 0x58; break;
        case LR_OP_FSUB: op1 = 0x5C; break;
        case LR_OP_FMUL: op1 = 0x59; break;
        case LR_OP_FDIV: op1 = 0x5E; break;
        default: op1 = 0x58; break;
        }
        emit_sse_arith(cc, op1, FP_SCRATCH0, FP_SCRATCH1, fsize);
        emit_store_fp_slot(cc, desc->dest, FP_SCRATCH0, fsize);
        break;
    }
    case LR_OP_FNEG: {
        const lr_type_t *elem_ty = NULL;
        uint64_t elem_count = 0;
        uint8_t elem_sz = 0;
        size_t total_sz = 0;
        if (x86_fp_aggregate_layout(desc->type, &elem_ty, &elem_count,
                                    &elem_sz, &total_sz)) {
            size_t dst_align = lr_type_align(desc->type);
            int32_t dst_off;
            x86_fpagg_src_t src;
            uint8_t prefix = (elem_sz == 8) ? 0xF2 : 0xF3;
            if (dst_align < 8) dst_align = 8;
            dst_off = alloc_slot(cc, desc->dest, total_sz, dst_align);
            x86_fpagg_init_src(cc, &ops[0], total_sz, X86_R10, &src);
            for (uint64_t i = 0; i < elem_count; i++) {
                int32_t off = (int32_t)(i * (uint64_t)elem_sz);
                x86_fpagg_load_elem(cc, &src, off, FP_SCRATCH1, elem_sz);
                encode_sse_rr(cc->buf, &cc->pos, cc->buflen, 0x66, 0x57, 0,
                              FP_SCRATCH0, FP_SCRATCH0);
                encode_sse_rr(cc->buf, &cc->pos, cc->buflen, prefix, 0x5C, 0,
                              FP_SCRATCH0, FP_SCRATCH1);
                emit_store_fp_mem_base(cc, X86_RBP, dst_off + off,
                                       FP_SCRATCH0, elem_sz);
            }
            break;
        }
        uint8_t fsize = (desc->type &&
                         desc->type->kind == LR_TYPE_FLOAT) ? 4 : 8;
        emit_load_fp_operand(cc, &ops[0], FP_SCRATCH1, fsize);
        encode_sse_rr(cc->buf, &cc->pos, cc->buflen, 0x66, 0x57, 0,
                      FP_SCRATCH0, FP_SCRATCH0);
        uint8_t prefix = (fsize == 8) ? 0xF2 : 0xF3;
        encode_sse_rr(cc->buf, &cc->pos, cc->buflen, prefix, 0x5C, 0,
                      FP_SCRATCH0, FP_SCRATCH1);
        emit_store_fp_slot(cc, desc->dest, FP_SCRATCH0, fsize);
        break;
    }
    case LR_OP_SDIV: case LR_OP_SREM: {
        emit_load_operand(cc, &ops[0], X86_RAX);
        emit_load_operand(cc, &ops[1], X86_RCX);
        {
            uint8_t bits = int_type_width_bits(desc->type);
            emit_sign_extend_value(cc, X86_RAX, bits);
            emit_sign_extend_value(cc, X86_RCX, bits);
            emit_byte(cc->buf, &cc->pos, cc->buflen,
                      rex(true, false, false, false));
            emit_byte(cc->buf, &cc->pos, cc->buflen, 0x99);
            emit_idiv_r(cc, X86_RCX, 8);
            if (bits < 64) {
                uint8_t narrow_res = (desc->op == LR_OP_SREM) ?
                                     X86_RDX : X86_RAX;
                emit_sign_extend_value(cc, narrow_res, bits);
            }
        }
        {
            uint8_t res_reg = (desc->op == LR_OP_SREM) ?
                              X86_RDX : X86_RAX;
            emit_store_slot(cc, desc->dest, res_reg);
        }
        break;
    }
    case LR_OP_SHL: case LR_OP_LSHR: case LR_OP_ASHR: {
        emit_load_operand(cc, &ops[0], X86_RAX);
        emit_load_operand(cc, &ops[1], X86_RCX);
        uint8_t ext;
        switch (desc->op) {
        case LR_OP_SHL:  ext = 4; break;
        case LR_OP_LSHR: ext = 5; break;
        case LR_OP_ASHR: ext = 7; break;
        default: ext = 4; break;
        }
        emit_shift(cc, ext, X86_RAX,
                   (uint8_t)lr_type_size(desc->type));
        emit_store_slot(cc, desc->dest, X86_RAX);
        break;
    }
    case LR_OP_ICMP: {
        emit_load_operand(cc, &ops[0], X86_RAX);
        emit_load_operand(cc, &ops[1], X86_RCX);
        encode_alu_rr(cc->buf, &cc->pos, cc->buflen, 0x39,
                      X86_RAX, X86_RCX,
                      (uint8_t)lr_type_size(ops[0].type));
        uint8_t icc = lr_target_cc_from_icmp(
            (lr_icmp_pred_t)desc->icmp_pred);
        emit_setcc(cc, icc, X86_RAX);
        emit_movzx_rr(cc, X86_RAX, X86_RAX, 1);
        emit_store_slot(cc, desc->dest, X86_RAX);
        break;
    }
    case LR_OP_SELECT: {
        emit_load_operand(cc, &ops[0], X86_RAX);
        encode_alu_rr(cc->buf, &cc->pos, cc->buflen, 0x85,
                      X86_RAX, X86_RAX, 1);
        emit_load_operand(cc, &ops[2], X86_RAX);
        emit_load_operand(cc, &ops[1], X86_RCX);
        emit_cmovcc(cc, LR_CC_NE, X86_RAX, X86_RCX, 8);
        emit_store_slot(cc, desc->dest, X86_RAX);
        break;
    }
    case LR_OP_BR: {
        ctx->deferred.pending = true;
        ctx->deferred.op = LR_OP_BR;
        ctx->deferred.ops[0] = ops[0];
        ctx->deferred.num_ops = 1;
        ctx->deferred.block_id = ctx->current_block_id;
        cc->current_inst = NULL;
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
        cc->current_inst = NULL;
        return 0;
    }
    case LR_OP_ALLOCA: {
        size_t elem_sz = lr_type_size(desc->type);
        if (elem_sz < 1) elem_sz = 1;
        /* Treat all constant-count allocas as static: LLVM semantics
           guarantee alloca is entry-block regardless of IR position,
           so we use the fixed frame for any compile-time-known size. */
        bool use_static = (nops == 0) ||
            (ops[0].kind == LR_VAL_IMM_I64);
        if (use_static) {
            int64_t count = (nops > 0) ? ops[0].imm_i64 : 1;
            if (count < 1) count = 1;
            size_t total_sz = elem_sz * (size_t)count;
            int32_t off = lr_target_lookup_static_alloca_offset(
                cc->static_alloca_offsets, cc->num_static_alloca_offsets,
                desc->dest);
            if (off == 0) {
                size_t elem_align = lr_type_align(desc->type);
                if (elem_align == 0) elem_align = 1;
                cc->stack_size = (uint32_t)align_up(cc->stack_size,
                                                     elem_align);
                cc->stack_size += (uint32_t)total_sz;
                off = -(int32_t)cc->stack_size;
                lr_target_set_static_alloca_offset(
                    cc->arena, &cc->static_alloca_offsets,
                    &cc->num_static_alloca_offsets, desc->dest, off);
            }
            encode_mem(cc->buf, &cc->pos, cc->buflen, 0x8D,
                       X86_RAX, X86_RBP, off, 8);
            emit_store_slot(cc, desc->dest, X86_RAX);
        } else {
            emit_load_operand(cc, &ops[0], X86_RAX);
            if (elem_sz != 1) {
                emit_mov_imm(cc, X86_RCX, (int64_t)elem_sz, false);
                emit_imul_rr(cc, X86_RAX, X86_RCX, 8);
            }
            emit_mov_imm(cc, X86_RCX, 15, false);
            encode_alu_rr(cc->buf, &cc->pos, cc->buflen, 0x01,
                          X86_RAX, X86_RCX, 8);
            emit_mov_imm(cc, X86_RCX, ~15LL, false);
            encode_alu_rr(cc->buf, &cc->pos, cc->buflen, 0x21,
                          X86_RAX, X86_RCX, 8);
            encode_alu_rr(cc->buf, &cc->pos, cc->buflen, 0x29,
                          X86_RSP, X86_RAX, 8);
            encode_alu_rr(cc->buf, &cc->pos, cc->buflen, 0x89,
                          X86_RAX, X86_RSP, 8);
            emit_store_slot(cc, desc->dest, X86_RAX);
        }
        break;
    }
    case LR_OP_LOAD: {
        emit_load_operand(cc, &ops[0], X86_RAX);
        size_t load_sz = lr_type_size(desc->type);
        if (load_sz == 0) load_sz = 8;
        if (load_sz > 8) {
            size_t load_align = lr_type_align(desc->type);
            int32_t dst_off = alloc_slot(cc, desc->dest, load_sz,
                                         load_align);
            emit_mem_copy_base_to_base(cc, X86_RBP, dst_off,
                                       X86_RAX, 0, load_sz);
        } else {
            uint8_t sz = (uint8_t)load_sz;
            if (sz < 4)
                emit_movzx_mem(cc, X86_RAX, X86_RAX, 0, sz);
            else
                encode_mem(cc->buf, &cc->pos, cc->buflen, 0x8B,
                           X86_RAX, X86_RAX, 0, sz);
            emit_store_slot(cc, desc->dest, X86_RAX);
        }
        break;
    }
    case LR_OP_STORE: {
        emit_load_operand(cc, &ops[1], X86_RCX);
        size_t store_sz = lr_type_size(ops[0].type);
        if (store_sz == 0) store_sz = 8;
        if (store_sz > 8) {
            if (ops[0].kind == LR_VAL_IMM_I64 && ops[0].imm_i64 == 0) {
                emit_mem_zero_base(cc, X86_RCX, 0, store_sz);
                break;
            }
            if (ops[0].kind == LR_VAL_VREG) {
                uint32_t vreg = ops[0].vreg;
                int32_t alloca_off = lr_target_lookup_static_alloca_offset(
                    cc->static_alloca_offsets,
                    cc->num_static_alloca_offsets, vreg);
                if (alloca_off != 0) {
                    /* Source is a static alloca — data lives at the
                       alloca offset, not in the vreg pointer slot. */
                    emit_mem_copy_base_to_base(cc, X86_RCX, 0,
                                               X86_RBP, alloca_off,
                                               store_sz);
                    break;
                }
                size_t src_sz = 0;
                int32_t src_off = alloc_slot(cc, vreg, 8, 8);
                if (vreg < cc->num_stack_slots)
                    src_sz = cc->stack_slot_sizes[vreg];
                if (src_sz >= store_sz) {
                    emit_mem_copy_base_to_base(cc, X86_RCX, 0,
                                               X86_RBP, src_off,
                                               store_sz);
                    break;
                }
                /* Slot holds a pointer to the data — dereference it */
                if (src_sz == 8) {
                    encode_mem(cc->buf, &cc->pos, cc->buflen, 0x8B,
                               X86_RAX, X86_RBP, src_off, 8);
                    emit_mem_copy_base_to_base(cc, X86_RCX, 0,
                                               X86_RAX, 0, store_sz);
                    break;
                }
                if (src_sz > 0)
                    emit_mem_copy_base_to_base(cc, X86_RCX, 0,
                                               X86_RBP, src_off, src_sz);
                if (src_sz < store_sz)
                    emit_mem_zero_base(cc, X86_RCX, (int32_t)src_sz,
                                       store_sz - src_sz);
                break;
            }
            emit_mem_zero_base(cc, X86_RCX, 0, store_sz);
            break;
        }
        emit_load_operand(cc, &ops[0], X86_RAX);
        emit_mem_store_sized(cc, X86_RAX, X86_RCX, 0,
                             (uint8_t)store_sz);
        break;
    }
    case LR_OP_GEP: {
        emit_load_operand(cc, &ops[0], X86_RAX);
        const lr_type_t *cur_ty = desc->type;
        for (uint32_t idx = 1; idx < nops; idx++) {
            lr_gep_step_t step;
            if (!lr_gep_analyze_step(cur_ty, idx == 1, &ops[idx], &step))
                continue;
            cur_ty = step.next_type;
            if (step.is_const) {
                if (step.const_byte_offset == 0) continue;
                emit_mov_imm(cc, X86_RCX, step.const_byte_offset,
                             false);
                encode_alu_rr(cc->buf, &cc->pos, cc->buflen, 0x01,
                              X86_RAX, X86_RCX, 8);
                continue;
            }
            emit_load_operand(cc, &ops[idx], X86_RCX);
            if (step.runtime_signext_bytes == 1 ||
                step.runtime_signext_bytes == 2)
                emit_movsx_rr(cc, X86_RCX, X86_RCX,
                              step.runtime_signext_bytes);
            else if (step.runtime_signext_bytes == 4)
                emit_movsxd(cc, X86_RCX, X86_RCX);
            if (step.runtime_elem_size != 1) {
                emit_mov_imm(cc, X86_R10,
                             (int64_t)step.runtime_elem_size, false);
                emit_imul_rr(cc, X86_RCX, X86_R10, 8);
            }
            encode_alu_rr(cc->buf, &cc->pos, cc->buflen, 0x01,
                          X86_RAX, X86_RCX, 8);
        }
        emit_store_slot(cc, desc->dest, X86_RAX);
        break;
    }
    case LR_OP_SEXT: {
        emit_load_operand(cc, &ops[0], X86_RAX);
        uint8_t src_bits = int_type_width_bits(ops[0].type);
        if (src_bits > 0 && src_bits < 64) {
            uint8_t shift = (uint8_t)(64 - src_bits);
            emit_mov_imm(cc, X86_RCX, (int64_t)shift, false);
            emit_shift(cc, 4, X86_RAX, 8);
            emit_shift(cc, 7, X86_RAX, 8);
        }
        emit_store_slot(cc, desc->dest, X86_RAX);
        break;
    }
    case LR_OP_ZEXT: {
        emit_load_operand(cc, &ops[0], X86_RAX);
        uint8_t src_bits = int_type_width_bits(ops[0].type);
        if (src_bits > 0 && src_bits < 64) {
            uint8_t shift = (uint8_t)(64 - src_bits);
            emit_mov_imm(cc, X86_RCX, (int64_t)shift, false);
            emit_shift(cc, 4, X86_RAX, 8);
            emit_shift(cc, 5, X86_RAX, 8);
        }
        emit_store_slot(cc, desc->dest, X86_RAX);
        break;
    }
    case LR_OP_TRUNC: {
        emit_load_operand(cc, &ops[0], X86_RAX);
        uint8_t dst_bits = int_type_width_bits(desc->type);
        if (dst_bits > 0 && dst_bits < 64) {
            uint8_t shift = (uint8_t)(64 - dst_bits);
            emit_mov_imm(cc, X86_RCX, (int64_t)shift, false);
            emit_shift(cc, 4, X86_RAX, 8);
            emit_shift(cc, 5, X86_RAX, 8);
        }
        emit_store_slot(cc, desc->dest, X86_RAX);
        break;
    }
    case LR_OP_BITCAST:
    case LR_OP_PTRTOINT:
    case LR_OP_INTTOPTR:
        emit_load_operand(cc, &ops[0], X86_RAX);
        emit_store_slot(cc, desc->dest, X86_RAX);
        break;
    case LR_OP_FCMP: {
        uint8_t fsize = (ops[0].type &&
                         ops[0].type->kind == LR_TYPE_FLOAT) ? 4 : 8;
        emit_load_fp_operand(cc, &ops[0], FP_SCRATCH0, fsize);
        emit_load_fp_operand(cc, &ops[1], FP_SCRATCH1, fsize);
        emit_fcmp(cc, FP_SCRATCH0, FP_SCRATCH1, fsize);
        uint8_t fcc = lr_target_cc_from_fcmp(
            (lr_fcmp_pred_t)desc->fcmp_pred);
        emit_setcc(cc, fcc, X86_RAX);
        emit_movzx_rr(cc, X86_RAX, X86_RAX, 1);
        emit_store_slot(cc, desc->dest, X86_RAX);
        break;
    }
    case LR_OP_SITOFP: {
        uint8_t fsize = (desc->type &&
                         desc->type->kind == LR_TYPE_FLOAT) ? 4 : 8;
        emit_load_operand(cc, &ops[0], X86_RAX);
        {
            size_t src_sz = lr_type_size(ops[0].type);
            if (src_sz == 1 || src_sz == 2)
                emit_movsx_rr(cc, X86_RAX, X86_RAX, (uint8_t)src_sz);
            else if (src_sz == 4)
                emit_movsxd(cc, X86_RAX, X86_RAX);
        }
        emit_cvtsi2fp(cc, FP_SCRATCH0, X86_RAX, fsize);
        emit_store_fp_slot(cc, desc->dest, FP_SCRATCH0, fsize);
        break;
    }
    case LR_OP_UITOFP: {
        uint8_t fsize = (desc->type &&
                         desc->type->kind == LR_TYPE_FLOAT) ? 4 : 8;
        emit_load_operand(cc, &ops[0], X86_RAX);
        {
            size_t src_sz = lr_type_size(ops[0].type);
            if (src_sz <= 4) {
                cc->buf[cc->pos++] = 0x89;
                cc->buf[cc->pos++] = 0xC0;
            }
        }
        emit_cvtsi2fp(cc, FP_SCRATCH0, X86_RAX, fsize);
        emit_store_fp_slot(cc, desc->dest, FP_SCRATCH0, fsize);
        break;
    }
    case LR_OP_FPTOSI: {
        uint8_t fsize = (ops[0].type &&
                         ops[0].type->kind == LR_TYPE_FLOAT) ? 4 : 8;
        emit_load_fp_operand(cc, &ops[0], FP_SCRATCH0, fsize);
        emit_cvtfp2si(cc, X86_RAX, FP_SCRATCH0, fsize);
        emit_store_slot(cc, desc->dest, X86_RAX);
        break;
    }
    case LR_OP_FPTOUI: {
        uint8_t fsize = (ops[0].type &&
                         ops[0].type->kind == LR_TYPE_FLOAT) ? 4 : 8;
        emit_load_fp_operand(cc, &ops[0], FP_SCRATCH0, fsize);
        emit_cvtfp2si(cc, X86_RAX, FP_SCRATCH0, fsize);
        emit_store_slot(cc, desc->dest, X86_RAX);
        break;
    }
    case LR_OP_FPEXT: {
        emit_load_fp_operand(cc, &ops[0], FP_SCRATCH0, 4);
        encode_sse_rr(cc->buf, &cc->pos, cc->buflen, 0xF3, 0x5A, 0,
                      FP_SCRATCH0, FP_SCRATCH0);
        emit_store_fp_slot(cc, desc->dest, FP_SCRATCH0, 8);
        break;
    }
    case LR_OP_FPTRUNC: {
        emit_load_fp_operand(cc, &ops[0], FP_SCRATCH0, 8);
        encode_sse_rr(cc->buf, &cc->pos, cc->buflen, 0xF2, 0x5A, 0,
                      FP_SCRATCH0, FP_SCRATCH0);
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
                size_t dst_align = desc->type ? lr_type_align(desc->type) : 8;
                if (dst_align < 8) dst_align = 8;
                int32_t dst_off = alloc_slot(cc, desc->dest, field_sz,
                                             dst_align);
                if (src_indirect) {
                    int32_t src_off = alloc_slot(cc, ops[0].vreg, 8, 8);
                    emit_mem_load_sized(cc, X86_R10, X86_RBP, src_off, 8);
                    emit_mem_copy_base_to_base(cc, X86_RBP, dst_off,
                                               X86_R10, (int32_t)field_off,
                                               field_sz);
                } else {
                    int32_t src_off = alloc_slot(cc, ops[0].vreg, 8, 8) +
                                      (int32_t)field_off;
                    emit_mem_copy_base_to_base(cc, X86_RBP, dst_off,
                                               X86_RBP, src_off, field_sz);
                }
            } else {
                if (src_indirect) {
                    int32_t src_off = alloc_slot(cc, ops[0].vreg, 8, 8);
                    emit_mem_load_sized(cc, X86_R10, X86_RBP, src_off, 8);
                    emit_mem_load_sized(cc, X86_RAX, X86_R10,
                                        (int32_t)field_off,
                                        (uint8_t)field_sz);
                } else {
                    emit_load_vreg_mem_sized(cc, ops[0].vreg,
                                             (int32_t)field_off, X86_RAX,
                                             (uint8_t)field_sz);
                }
                emit_store_slot(cc, desc->dest, X86_RAX);
            }
            break;
        }
        if (nops > 0 && (ops[0].kind == LR_VAL_UNDEF ||
                          ops[0].kind == LR_VAL_NULL)) {
            if (field_sz > 8) {
                size_t dst_align = desc->type ? lr_type_align(desc->type) : 8;
                if (dst_align < 8) dst_align = 8;
                int32_t dst_off = alloc_slot(cc, desc->dest, field_sz,
                                             dst_align);
                emit_mem_zero_base(cc, X86_RBP, dst_off, field_sz);
            } else {
                emit_mov_imm(cc, X86_RAX, 0, false);
                emit_store_slot(cc, desc->dest, X86_RAX);
            }
            break;
        }
        emit_load_operand(cc, &ops[0], X86_RAX);
        emit_store_slot(cc, desc->dest, X86_RAX);
        break;
    }
    case LR_OP_INSERTVALUE: {
        size_t agg_sz = desc->type ? lr_type_size(desc->type) : 8;
        size_t agg_align = desc->type ? lr_type_align(desc->type) : 8;
        size_t field_off = 0;
        const lr_type_t *field_ty = NULL;
        int32_t dst_off;
        if (agg_sz < 8) agg_sz = 8;
        if (agg_align < 8) agg_align = 8;
        dst_off = alloc_slot(cc, desc->dest, agg_sz, agg_align);

        if (nops > 0) {
            if (ops[0].kind == LR_VAL_VREG) {
                emit_copy_vreg_value_bytes_to_base(cc, ops[0].vreg, agg_sz,
                                                   X86_RBP, dst_off);
            } else if (ops[0].kind == LR_VAL_UNDEF ||
                       ops[0].kind == LR_VAL_NULL) {
                emit_mem_zero_base(cc, X86_RBP, dst_off, agg_sz);
            } else if (agg_sz <= 8) {
                emit_load_operand(cc, &ops[0], X86_RAX);
                emit_mem_store_sized(cc, X86_RAX, X86_RBP, dst_off,
                                     (uint8_t)agg_sz);
            } else {
                emit_mem_zero_base(cc, X86_RBP, dst_off, agg_sz);
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
                        cc, ops[1].vreg, field_sz, X86_RBP,
                        dst_off + (int32_t)field_off);
                } else {
                    emit_mem_zero_base(cc, X86_RBP,
                                       dst_off + (int32_t)field_off,
                                       field_sz);
                }
            } else {
                if (ops[1].kind == LR_VAL_UNDEF ||
                    ops[1].kind == LR_VAL_NULL)
                    emit_mov_imm(cc, X86_RAX, 0, false);
                else
                    emit_load_operand(cc, &ops[1], X86_RAX);
                emit_mem_store_sized(cc, X86_RAX, X86_RBP,
                                     dst_off + (int32_t)field_off,
                                     (uint8_t)field_sz);
            }
        }
        break;
    }
    case LR_OP_CALL: {
        if (ops[0].kind == LR_VAL_GLOBAL && cc->mod) {
            const char *cname = lr_module_symbol_name(
                cc->mod, ops[0].global_id);
            if (cname && strcmp(cname, "llvm.va_start.p0") == 0) {
                if (cc->func_is_vararg && nops >= 2) {
                    emit_load_operand(cc, &ops[1], X86_RAX);
                    uint32_t gp_off = cc->vararg_named_gp * 8;
                    emit_mov_imm(cc, X86_RCX, (int64_t)gp_off, false);
                    encode_mem(cc->buf, &cc->pos, cc->buflen, 0x89,
                               X86_RCX, X86_RAX, 0, 4);
                    emit_mov_imm(cc, X86_RCX, 48, false);
                    encode_mem(cc->buf, &cc->pos, cc->buflen, 0x89,
                               X86_RCX, X86_RAX, 4, 4);
                    int32_t overflow_off = 16 + (int32_t)(
                        cc->vararg_named_gp > 6 ?
                        cc->vararg_named_gp - 6 : 0) * 8;
                    encode_mem(cc->buf, &cc->pos, cc->buflen, 0x8D,
                               X86_RCX, X86_RBP, overflow_off, 8);
                    encode_mem(cc->buf, &cc->pos, cc->buflen, 0x89,
                               X86_RCX, X86_RAX, 8, 8);
                    encode_mem(cc->buf, &cc->pos, cc->buflen, 0x8D,
                               X86_RCX, X86_RBP, cc->vararg_rsa_off, 8);
                    encode_mem(cc->buf, &cc->pos, cc->buflen, 0x89,
                               X86_RCX, X86_RAX, 16, 8);
                    invalidate_cached_gprs(cc);
                }
                break;
            }
            if (cname && strcmp(cname, "llvm.va_end.p0") == 0)
                break;
            if (cname && strcmp(cname, "llvm.va_copy.p0") == 0) {
                if (nops >= 3) {
                    emit_load_operand(cc, &ops[1], X86_RAX);
                    emit_load_operand(cc, &ops[2], X86_RCX);
                    for (int32_t off = 0; off < 24; off += 8) {
                        encode_mem(cc->buf, &cc->pos, cc->buflen, 0x8B,
                                   X86_R11, X86_RCX, off, 8);
                        encode_mem(cc->buf, &cc->pos, cc->buflen, 0x89,
                                   X86_R11, X86_RAX, off, 8);
                    }
                    invalidate_cached_gprs(cc);
                }
                break;
            }
        }
        static const uint8_t call_regs[] = {
            X86_RDI, X86_RSI, X86_RDX, X86_RCX, X86_R8, X86_R9
        };
        static const uint8_t call_fp_regs[] = {
            X86_XMM0, X86_XMM1, X86_XMM2, X86_XMM3,
            X86_XMM4, X86_XMM5, X86_XMM6, X86_XMM7
        };
        uint32_t nargs = nops - 1;
        uint32_t gp_used = 0;
        uint32_t fp_used = 0;
        uint32_t stack_args = 0;
        uint32_t stack_bytes = 0;
        uint32_t fp_used_for_call = 0;
        lr_func_t *callee_func = NULL;
        bool use_external_sysv_fp = false;
        bool callee_vararg = false;
        bool internal_sret = false;
        uint32_t internal_gp_start = 0;
        uint32_t internal_gp_cap = 6;

        use_external_sysv_fp = direct_call_uses_external_sysv_abi(
            cc, &ops[0], desc->call_external_abi, desc->call_vararg,
            &callee_func, &callee_vararg);
        internal_sret = !use_external_sysv_fp &&
                        uses_internal_sret_abi(desc->type);
        if (internal_sret) {
            internal_gp_start = 1;
            internal_gp_cap = 5;
        }

        if (use_external_sysv_fp) {
            for (uint32_t i = 0; i < nargs; i++) {
                const lr_type_t *arg_type = call_arg_abi_type(
                    callee_func, i, &ops[i + 1]);
                uint8_t agg_lane_size = 0;
                uint8_t agg_lane_count = 0;
                if (is_fp_abi_type(arg_type)) {
                    if (fp_used < 8) fp_used++;
                    else stack_args++;
                } else if (fp_abi_two_lane_aggregate(arg_type,
                                                      &agg_lane_size,
                                                      &agg_lane_count)) {
                    uint32_t agg_stack_units = (uint32_t)(
                        ((uint32_t)agg_lane_size * (uint32_t)agg_lane_count + 7u) / 8u);
                    if (fp_used + agg_lane_count <= 8)
                        fp_used += agg_lane_count;
                    else stack_args += agg_stack_units;
                } else {
                    if (gp_used < 6) gp_used++;
                    else stack_args++;
                }
            }
        } else {
            stack_args = nargs > internal_gp_cap ?
                nargs - internal_gp_cap : 0;
        }

        stack_bytes = ((stack_args * 8 + 15) & ~15u);
        if (stack_bytes > 0)
            emit_frame_alloc(cc, stack_bytes);

        if (use_external_sysv_fp) {
            uint32_t stack_idx = 0;
            gp_used = 0;
            fp_used = 0;
            for (uint32_t i = 0; i < nargs; i++) {
                const lr_type_t *arg_type = call_arg_abi_type(
                    callee_func, i, &ops[i + 1]);
                uint8_t agg_lane_size = 0;
                uint8_t agg_lane_count = 0;
                bool is_fp_agg = fp_abi_two_lane_aggregate(
                    arg_type, &agg_lane_size, &agg_lane_count);
                if (is_fp_abi_type(arg_type) && fp_used < 8) {
                    emit_load_external_fp_call_arg(cc, &ops[i + 1], arg_type,
                                                   call_fp_regs[fp_used]);
                    fp_used++;
                    continue;
                }
                if (is_fp_agg && fp_used + agg_lane_count <= 8) {
                    size_t agg_sz = (size_t)agg_lane_size *
                                    (size_t)agg_lane_count;
                    x86_fpagg_src_t src;
                    x86_fpagg_init_src(cc, &ops[i + 1], agg_sz, X86_R10, &src);
                    x86_fpagg_load_elem(cc, &src, 0, call_fp_regs[fp_used],
                                        agg_lane_size);
                    if (agg_lane_count > 1) {
                        x86_fpagg_load_elem(
                            cc, &src, (int32_t)agg_lane_size,
                            call_fp_regs[fp_used + 1], agg_lane_size);
                    }
                    fp_used += agg_lane_count;
                    continue;
                }
                if (!is_fp_abi_type(arg_type) && !is_fp_agg &&
                    gp_used < 6) {
                    emit_load_operand(cc, &ops[i + 1],
                                      call_regs[gp_used]);
                    gp_used++;
                    continue;
                }
                if (is_fp_abi_type(arg_type)) {
                    emit_load_external_fp_call_arg(cc, &ops[i + 1], arg_type,
                                                   FP_SCRATCH0);
                    emit_store_fp_mem_base(cc, X86_RSP,
                                           (int32_t)(stack_idx * 8),
                                           FP_SCRATCH0,
                                           fp_abi_size(arg_type));
                    stack_idx++;
                    continue;
                }
                if (is_fp_agg) {
                    uint32_t agg_stack_units = (uint32_t)(
                        ((uint32_t)agg_lane_size * (uint32_t)agg_lane_count + 7u) / 8u);
                    size_t agg_sz = (size_t)agg_lane_size *
                                    (size_t)agg_lane_count;
                    x86_fpagg_src_t src;
                    x86_fpagg_init_src(cc, &ops[i + 1], agg_sz, X86_R10, &src);
                    for (uint32_t lane = 0; lane < agg_stack_units; lane++) {
                        int32_t off = (int32_t)(lane * (uint32_t)agg_lane_size);
                        x86_fpagg_load_elem(cc, &src, off, FP_SCRATCH0,
                                            agg_lane_size);
                        emit_store_fp_mem_base(
                            cc, X86_RSP,
                            (int32_t)((stack_idx + lane) * 8u),
                            FP_SCRATCH0, agg_lane_size);
                    }
                    stack_idx += agg_stack_units;
                    continue;
                }
                emit_load_operand(cc, &ops[i + 1], X86_RAX);
                encode_mem(cc->buf, &cc->pos, cc->buflen, 0x89,
                           X86_RAX, X86_RSP,
                           (int32_t)(stack_idx * 8), 8);
                stack_idx++;
            }
            fp_used_for_call = fp_used;
        } else {
            uint32_t nstack = nargs > internal_gp_cap ?
                nargs - internal_gp_cap : 0;
            if (internal_sret) {
                size_t dst_sz = lr_type_size(desc->type);
                size_t dst_align = lr_type_align(desc->type);
                int32_t doff;
                if (dst_align < 8) dst_align = 8;
                if (dst_sz < 8) dst_sz = 8;
                doff = alloc_slot(cc, desc->dest, dst_sz, dst_align);
                encode_mem(cc->buf, &cc->pos, cc->buflen, 0x8D,
                           X86_RDI, X86_RBP, doff, 8);
            }
            for (uint32_t i = 0; i < nstack; i++) {
                uint32_t arg_idx = internal_gp_cap + i;
                emit_load_operand(cc, &ops[arg_idx + 1], X86_RAX);
                encode_mem(cc->buf, &cc->pos, cc->buflen, 0x89,
                           X86_RAX, X86_RSP, (int32_t)(i * 8), 8);
            }
            for (uint32_t i = 0; i < nargs && i < internal_gp_cap; i++)
                emit_load_operand(cc, &ops[i + 1],
                                  call_regs[internal_gp_start + i]);
        }

        if (callee_vararg)
            emit_mov_imm(cc, X86_RAX, (int64_t)fp_used_for_call,
                         false);

        emit_load_operand(cc, &ops[0], X86_R10);
        emit_call_r10(cc);

        if (stack_bytes > 0)
            emit_frame_free(cc, stack_bytes);

        invalidate_cached_gprs(cc);
        if (desc->type && desc->type->kind != LR_TYPE_VOID) {
            uint8_t ret_lane_size = 0;
            uint8_t ret_lane_count = 0;
            bool ret_fp_agg = fp_abi_two_lane_aggregate(
                desc->type, &ret_lane_size, &ret_lane_count);
            if (internal_sret) {
                /* Already materialized through hidden sret pointer. */
            } else if (use_external_sysv_fp && ret_fp_agg) {
                size_t dst_sz = lr_type_size(desc->type);
                size_t dst_align = lr_type_align(desc->type);
                int32_t doff;
                if (dst_align < 8) dst_align = 8;
                if (dst_sz < 8) dst_sz = 8;
                doff = alloc_slot(cc, desc->dest, dst_sz, dst_align);
                emit_store_fp_mem_base(cc, X86_RBP, doff, X86_XMM0,
                                       ret_lane_size);
                if (ret_lane_count > 1 &&
                    dst_sz >= (size_t)(2 * ret_lane_size))
                    emit_store_fp_mem_base(
                        cc, X86_RBP,
                        doff + (int32_t)ret_lane_size,
                        X86_XMM1, ret_lane_size);
            } else if (use_external_sysv_fp && is_fp_abi_type(desc->type)) {
                emit_store_fp_slot(cc, desc->dest, X86_XMM0,
                                   fp_abi_size(desc->type));
            } else {
                emit_store_slot(cc, desc->dest, X86_RAX);
            }
        }
        break;
    }
    case LR_OP_PHI: {
        size_t phi_sz = desc->type ? lr_type_size(desc->type) : 8;
        size_t phi_al = desc->type ? lr_type_align(desc->type) : 8;
        if (phi_sz < 8) phi_sz = 8;
        if (phi_al < 8) phi_al = 8;
        (void)alloc_slot(cc, desc->dest, phi_sz, phi_al);
        break;
    }
    case LR_OP_UNREACHABLE:
        break;
    default:
        break;
    }

    cc->current_inst = NULL;
    return 0;
}

static int x86_64_compile_end(void *compile_ctx, size_t *out_len) {
    x86_direct_ctx_t *ctx = (x86_direct_ctx_t *)compile_ctx;
    x86_compile_ctx_t *cc;
    if (!ctx || !out_len)
        return -1;

#if !defined(__x86_64__) && !defined(_M_X64)
    if (ctx->mode == LR_COMPILE_COPY_PATCH)
        return -1;
#endif

    if (ctx->block_offset_pending) {
        ctx->cc.block_offsets[ctx->current_block_id] = ctx->cc.pos;
    }
    ctx->block_offset_pending = false;
    if (flush_deferred_terminator(ctx) != 0)
        return -1;

    cc = &ctx->cc;

    {
        uint32_t orig_num_fixups = cc->num_fixups;
        for (uint32_t fi = 0; fi < orig_num_fixups; fi++) {
            uint32_t source = cc->fixups[fi].source;
            uint32_t target = cc->fixups[fi].target;
            bool has_late = false;
            if (source == UINT32_MAX)
                continue;
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
            size_t stub_pos = cc->pos;
            for (uint32_t pi = 0; pi < ctx->phi_copy_count; pi++) {
                if (ctx->phi_copies[pi].pred_block_id != source ||
                    ctx->phi_copies[pi].succ_block_id != target)
                    continue;
                emit_phi_copy_value(cc, ctx->phi_copies[pi].dest_vreg,
                                    &ctx->phi_copies[pi].src_op);
            }
            if (direct_ensure_fixup_cap(ctx) != 0)
                return -1;
            emit_jmp(cc, target);
            cc->fixups[fi].target = UINT32_MAX;
            if (cc->fixups[fi].pos + 4 <= cc->buflen) {
                int32_t rel = (int32_t)((int64_t)stub_pos -
                                        (int64_t)(cc->fixups[fi].pos + 4));
                cc->buf[cc->fixups[fi].pos + 0] = (uint8_t)(rel);
                cc->buf[cc->fixups[fi].pos + 1] = (uint8_t)(rel >> 8);
                cc->buf[cc->fixups[fi].pos + 2] = (uint8_t)(rel >> 16);
                cc->buf[cc->fixups[fi].pos + 3] = (uint8_t)(rel >> 24);
            }
        }
    }

    for (uint32_t i = 0; i < cc->num_fixups; i++) {
        size_t fix_pos = cc->fixups[i].pos;
        uint32_t target = cc->fixups[i].target;
        if (target == UINT32_MAX)
            continue;
        if (target < cc->num_block_offsets &&
            cc->block_offsets[target] != SIZE_MAX &&
            fix_pos + 4 <= cc->buflen) {
            int32_t rel = (int32_t)((int64_t)cc->block_offsets[target] -
                                    (int64_t)(fix_pos + 4));
            cc->buf[fix_pos + 0] = (uint8_t)(rel);
            cc->buf[fix_pos + 1] = (uint8_t)(rel >> 8);
            cc->buf[fix_pos + 2] = (uint8_t)(rel >> 16);
            cc->buf[fix_pos + 3] = (uint8_t)(rel >> 24);
        }
    }

    {
        uint32_t frame_stack_size = (cc->stack_size + 15u) & ~15u;
        patch_u32(cc->buf, cc->buflen, ctx->prologue_patch_pos,
                  frame_stack_size);
    }

    *out_len = cc->pos;
    if (cc->pos > cc->buflen)
        return -1;
    return 0;
}

static int x86_64_compile_add_phi_copy(void *compile_ctx,
                                       uint32_t pred_block_id,
                                       uint32_t succ_block_id,
                                       uint32_t dest_vreg,
                                       const lr_operand_desc_t *src_op) {
    x86_direct_ctx_t *ctx = (x86_direct_ctx_t *)compile_ctx;
    if (!ctx || !src_op)
        return -1;
    if (direct_ensure_phi_copy_cap(ctx) != 0)
        return -1;

    (void)alloc_slot(&ctx->cc, dest_vreg, 8, 8);

    x86_stream_phi_copy_t *entry = &ctx->phi_copies[ctx->phi_copy_count++];
    entry->pred_block_id = pred_block_id;
    entry->succ_block_id = succ_block_id;
    entry->dest_vreg = dest_vreg;
    entry->src_op = operand_from_desc(src_op);
    entry->emitted = false;
    return 0;
}

static const lr_target_t x86_64_target = {
    .name = "x86_64",
    .ptr_size = 8,
    .compile_begin = x86_64_compile_begin,
    .compile_emit = x86_64_compile_emit,
    .compile_set_block = x86_64_compile_set_block,
    .compile_end = x86_64_compile_end,
    .compile_add_phi_copy = x86_64_compile_add_phi_copy,
};

const lr_target_t *lr_target_x86_64(void) {
    return &x86_64_target;
}
