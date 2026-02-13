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
    uint32_t rax_holds_vreg;
    uint32_t rcx_holds_vreg;
    uint32_t *vreg_use_counts;
    uint32_t num_vreg_use_counts;
    lr_block_t *current_block;
    lr_inst_t *current_inst;
    uint32_t current_inst_index;
    bool func_uses_internal_sret;
    int32_t sret_ptr_off;
    bool func_is_vararg;
    int32_t vararg_rsa_off;
    uint32_t vararg_named_gp;
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

static bool inst_produces_elidable_rax_value(const lr_inst_t *inst) {
    if (!inst)
        return false;
    switch (inst->op) {
    case LR_OP_ADD: case LR_OP_SUB: case LR_OP_AND:
    case LR_OP_OR: case LR_OP_XOR:
    case LR_OP_MUL:
    case LR_OP_SDIV:
    case LR_OP_SHL: case LR_OP_LSHR: case LR_OP_ASHR:
    case LR_OP_ICMP:
    case LR_OP_SELECT:
    case LR_OP_ALLOCA:
    case LR_OP_LOAD:
    case LR_OP_GEP:
    case LR_OP_SEXT:
    case LR_OP_ZEXT: case LR_OP_TRUNC: case LR_OP_BITCAST:
    case LR_OP_PTRTOINT: case LR_OP_INTTOPTR:
    case LR_OP_FCMP:
    case LR_OP_FPTOSI: case LR_OP_FPTOUI:
    case LR_OP_EXTRACTVALUE:
        return true;
    default:
        return false;
    }
}

static bool inst_consumes_operand0_in_rax(const lr_inst_t *inst) {
    if (!inst || inst->num_operands == 0)
        return false;
    switch (inst->op) {
    case LR_OP_ADD: case LR_OP_SUB: case LR_OP_AND:
    case LR_OP_OR: case LR_OP_XOR:
    case LR_OP_MUL:
    case LR_OP_SDIV: case LR_OP_SREM:
    case LR_OP_SHL: case LR_OP_LSHR: case LR_OP_ASHR:
    case LR_OP_ICMP:
    case LR_OP_SELECT:
    case LR_OP_ALLOCA:
    case LR_OP_LOAD:
    case LR_OP_GEP:
    case LR_OP_SEXT:
    case LR_OP_ZEXT: case LR_OP_TRUNC: case LR_OP_BITCAST:
    case LR_OP_PTRTOINT: case LR_OP_INTTOPTR:
    case LR_OP_SITOFP: case LR_OP_UITOFP:
        return true;
    default:
        return false;
    }
}

static bool should_elide_store_slot(const x86_compile_ctx_t *ctx,
                                    uint32_t vreg, uint8_t reg) {
    if (!ctx || reg != X86_RAX || !ctx->current_block || !ctx->current_inst)
        return false;
    if (!inst_produces_elidable_rax_value(ctx->current_inst))
        return false;
    if (ctx->current_inst->dest != vreg)
        return false;
    if (!ctx->vreg_use_counts || vreg >= ctx->num_vreg_use_counts)
        return false;
    if (ctx->vreg_use_counts[vreg] != 1)
        return false;
    if (ctx->current_inst_index + 1 >= ctx->current_block->num_insts)
        return false;

    lr_inst_t *next = ctx->current_block->inst_array[ctx->current_inst_index + 1];
    if (!inst_consumes_operand0_in_rax(next))
        return false;
    if (next->operands[0].kind != LR_VAL_VREG || next->operands[0].vreg != vreg)
        return false;
    return true;
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
    int32_t off = alloc_slot(ctx, vreg, 8, 8);
    encode_mem(ctx->buf, &ctx->pos, ctx->buflen, 0x8B, reg, X86_RBP, off, 8);
    set_cached_reg_vreg(ctx, reg, vreg);
}

/* Emit: mov [rbp + offset], reg (store reg to vreg stack slot) */
static void emit_store_slot(x86_compile_ctx_t *ctx, uint32_t vreg, uint8_t reg) {
    if (should_elide_store_slot(ctx, vreg, reg)) {
        set_cached_reg_vreg(ctx, reg, vreg);
        return;
    }
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
    } else if (type->kind == LR_TYPE_ARRAY && type->array.count == 2) {
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
    } else if (op->kind == LR_VAL_GLOBAL && ctx->obj_ctx) {
        const char *sym_name = lr_module_symbol_name(ctx->mod,
                                                      op->global_id);
        if (!sym_name) {
            emit_mov_imm(ctx, reg, 0, preserve_flags);
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
static void emit_phi_copies(x86_compile_ctx_t *ctx, const lr_block_phi_copies_t *copies) {
    for (uint32_t i = 0; i < copies->count; i++) {
        emit_load_operand(ctx, &copies->copies[i].src_op, X86_RAX);
        emit_store_slot(ctx, copies->copies[i].dest_vreg, X86_RAX);
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

static int32_t ensure_static_alloca_offset(x86_compile_ctx_t *ctx, const lr_inst_t *inst) {
    int32_t off = lr_target_lookup_static_alloca_offset(ctx->static_alloca_offsets,
                                                        ctx->num_static_alloca_offsets,
                                                        inst->dest);
    if (off != 0)
        return off;

    size_t elem_sz = lr_target_alloca_elem_size(inst, 1);
    size_t elem_align = lr_type_align(inst->type);
    if (elem_align == 0)
        elem_align = 1;
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

static void reserve_phi_dest_slot_cb(void *ctx, uint32_t dest_vreg) {
    (void)alloc_slot((x86_compile_ctx_t *)ctx, dest_vreg, 8, 8);
}

/*
 * x86_64_compile_func: single-pass ISel + encoding.
 * Non-streaming path used by object-file emission and copy-and-patch fallback.
 */
int x86_64_compile_func(lr_func_t *func, lr_module_t *mod,
                                uint8_t *buf, size_t buflen, size_t *out_len,
                                lr_arena_t *arena) {
    lr_arena_t *layout_arena = (mod && mod->arena) ? mod->arena : arena;
    lr_target_func_analysis_t analysis;
    uint32_t initial_stack_slots = func->next_vreg > 64 ? func->next_vreg : 64;

    uint32_t nb = func->num_blocks > 0 ? func->num_blocks : 1;
    uint32_t fc = nb * 2;
    x86_compile_ctx_t ctx = {
        .buf = buf,
        .buflen = buflen,
        .pos = 0,
        .stack_size = 0,
        .stack_slots = lr_arena_array(arena, int32_t, initial_stack_slots),
        .stack_slot_sizes = lr_arena_array(arena, uint32_t, initial_stack_slots),
        .num_stack_slots = initial_stack_slots,
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
        .rax_holds_vreg = UINT32_MAX,
        .rcx_holds_vreg = UINT32_MAX,
        .vreg_use_counts = NULL,
        .num_vreg_use_counts = 0,
        .current_block = NULL,
        .current_inst = NULL,
        .current_inst_index = 0,
        .func_uses_internal_sret = false,
        .sret_ptr_off = 0,
        .func_is_vararg = false,
        .vararg_rsa_off = 0,
        .vararg_named_gp = 0,
    };

    attach_obj_symbol_meta_cache(&ctx);

    /* Build PHI copy arrays keyed by predecessor block id. */
    lr_block_phi_copies_t *phi_copies = lr_build_phi_copies(ctx.arena, func);
    if (!phi_copies)
        return -1;
    if (lr_target_analyze_function(func, layout_arena, phi_copies,
                                   &ctx, ensure_static_alloca_offset_cb,
                                   &ctx, reserve_phi_dest_slot_cb,
                                   &analysis) != 0)
        return -1;
    ctx.vreg_use_counts = analysis.vreg_use_counts;
    ctx.num_vreg_use_counts = analysis.num_vregs;

    /* Emit prologue and patch stack size once frame growth is complete. */
    size_t prologue_stack_patch_pos = emit_prologue(&ctx);

    ctx.func_uses_internal_sret = uses_internal_sret_abi(func->ret_type);
    if (ctx.func_uses_internal_sret) {
        ctx.sret_ptr_off = alloc_temp_slot(&ctx, 8, 8);
        emit_mem_store_sized(&ctx, X86_RDI, X86_RBP, ctx.sret_ptr_off, 8);
    }

    /* Store parameters: first 6 from registers, rest from caller frame */
    static const uint8_t param_regs[] = { X86_RDI, X86_RSI, X86_RDX, X86_RCX, X86_R8, X86_R9 };
    uint32_t param_gp_start = ctx.func_uses_internal_sret ? 1u : 0u;
    uint32_t param_gp_cap = 6u - param_gp_start;
    for (uint32_t i = 0; i < func->num_params && i < param_gp_cap; i++)
        emit_store_slot(&ctx, func->param_vregs[i], param_regs[param_gp_start + i]);
    for (uint32_t i = param_gp_cap; i < func->num_params; i++) {
        int32_t caller_off = 16 + (int32_t)(i - param_gp_cap) * 8;
        encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x8B, X86_RAX, X86_RBP, caller_off, 8);
        emit_store_slot(&ctx, func->param_vregs[i], X86_RAX);
    }

    ctx.func_is_vararg = func->vararg;
    if (func->vararg) {
        uint32_t named_gp = func->num_params;
        if (named_gp > 6) named_gp = 6;
        ctx.vararg_named_gp = named_gp;
        ctx.vararg_rsa_off = alloc_temp_slot(&ctx, 48, 8);
        for (uint32_t i = 0; i < 6; i++)
            emit_mem_store_sized(&ctx, param_regs[i], X86_RBP,
                                 ctx.vararg_rsa_off + (int32_t)(i * 8), 8);
    }

    /* Walk IR blocks and instructions, emitting code directly */
    for (uint32_t bi = 0; bi < func->num_blocks; bi++) {
        lr_block_t *b = func->block_array[bi];
        ctx.block_offsets[bi] = ctx.pos;
        invalidate_cached_gprs(&ctx);

        for (uint32_t ii = 0; ii < b->num_insts; ii++) {
            lr_inst_t *inst = b->inst_array[ii];
            ctx.current_block = b;
            ctx.current_inst = inst;
            ctx.current_inst_index = ii;
            switch (inst->op) {
            case LR_OP_RET: {
                emit_phi_copies(&ctx, &phi_copies[bi]);
                if (ctx.func_uses_internal_sret) {
                    size_t ret_sz = lr_type_size(func->ret_type);
                    const lr_operand_t *retv = &inst->operands[0];
                    emit_mem_load_sized(&ctx, X86_RDI, X86_RBP, ctx.sret_ptr_off, 8);
                    if (ret_sz == 0)
                        ret_sz = 8;
                    if (retv->kind == LR_VAL_VREG) {
                        uint32_t vreg = retv->vreg;
                        size_t src_sz = 0;
                        int32_t src_off = alloc_slot(&ctx, vreg, 8, 8);
                        if (vreg < ctx.num_stack_slots)
                            src_sz = ctx.stack_slot_sizes[vreg];
                        if (src_sz > ret_sz)
                            src_sz = ret_sz;
                        if (src_sz > 0)
                            emit_mem_copy_base_to_base(&ctx, X86_RDI, 0, X86_RBP, src_off, src_sz);
                        if (src_sz < ret_sz)
                            emit_mem_zero_base(&ctx, X86_RDI, (int32_t)src_sz, ret_sz - src_sz);
                    } else if (retv->kind == LR_VAL_UNDEF || retv->kind == LR_VAL_NULL) {
                        emit_mem_zero_base(&ctx, X86_RDI, 0, ret_sz);
                    } else if (ret_sz <= 8) {
                        emit_load_operand(&ctx, retv, X86_RAX);
                        emit_mem_store_sized(&ctx, X86_RAX, X86_RDI, 0, (uint8_t)ret_sz);
                    } else {
                        emit_mem_zero_base(&ctx, X86_RDI, 0, ret_sz);
                    }
                    encode_alu_rr(ctx.buf, &ctx.pos, ctx.buflen, 0x89,
                                  X86_RAX, X86_RDI, 8);
                } else {
                    emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                }
                emit_epilogue(&ctx);
                break;
            }
            case LR_OP_RET_VOID: {
                emit_phi_copies(&ctx, &phi_copies[bi]);
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
                {
                    uint8_t bits = int_type_width_bits(inst->type);
                    emit_sign_extend_value(&ctx, X86_RAX, bits);
                    emit_sign_extend_value(&ctx, X86_RCX, bits);
                    emit_byte(ctx.buf, &ctx.pos, ctx.buflen,
                              rex(true, false, false, false));
                    emit_byte(ctx.buf, &ctx.pos, ctx.buflen, 0x99); /* cqo */
                    emit_idiv_r(&ctx, X86_RCX, 8);
                    if (bits < 64) {
                        uint8_t narrow_res = (inst->op == LR_OP_SREM) ?
                                             X86_RDX : X86_RAX;
                        emit_sign_extend_value(&ctx, narrow_res, bits);
                    }
                }
                uint8_t res_reg = (inst->op == LR_OP_SREM) ?
                                  X86_RDX : X86_RAX;
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
                emit_phi_copies(&ctx, &phi_copies[bi]);
                uint32_t target_id = inst->operands[0].block_id;
                emit_jmp(&ctx, target_id);
                break;
            }
            case LR_OP_CONDBR: {
                emit_phi_copies(&ctx, &phi_copies[bi]);
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
                size_t elem_sz = lr_target_alloca_elem_size(inst, 1);

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
                        emit_mov_imm(&ctx, X86_RCX, (int64_t)elem_sz, false);
                        emit_imul_rr(&ctx, X86_RAX, X86_RCX, 8);
                    }
                    /* Align to 16: rax = (rax + 15) & ~15 */
                    emit_mov_imm(&ctx, X86_RCX, 15, false);
                    encode_alu_rr(ctx.buf, &ctx.pos, ctx.buflen, 0x01, X86_RAX, X86_RCX, 8);
                    emit_mov_imm(&ctx, X86_RCX, ~15LL, false);
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
                if (load_sz == 0)
                    load_sz = 8;
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
                        emit_mov_imm(&ctx, X86_RCX, step.const_byte_offset, false);
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
                        emit_mov_imm(&ctx, X86_R10, (int64_t)step.runtime_elem_size, false);
                        emit_imul_rr(&ctx, X86_RCX, X86_R10, 8);
                    }
                    encode_alu_rr(ctx.buf, &ctx.pos, ctx.buflen, 0x01, X86_RAX, X86_RCX, 8);
                }
                emit_store_slot(&ctx, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_SEXT: {
                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                uint8_t src_bits = int_type_width_bits(inst->operands[0].type);
                if (src_bits > 0 && src_bits < 64) {
                    uint8_t shift = (uint8_t)(64 - src_bits);
                    emit_mov_imm(&ctx, X86_RCX, (int64_t)shift, false);
                    emit_shift(&ctx, 4, X86_RAX, 8); /* shl rax, cl */
                    emit_shift(&ctx, 7, X86_RAX, 8); /* sar rax, cl */
                }
                emit_store_slot(&ctx, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_ZEXT: {
                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                uint8_t src_bits = int_type_width_bits(inst->operands[0].type);
                if (src_bits > 0 && src_bits < 64) {
                    uint8_t shift = (uint8_t)(64 - src_bits);
                    emit_mov_imm(&ctx, X86_RCX, (int64_t)shift, false);
                    emit_shift(&ctx, 4, X86_RAX, 8); /* shl rax, cl */
                    emit_shift(&ctx, 5, X86_RAX, 8); /* shr rax, cl */
                }
                emit_store_slot(&ctx, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_TRUNC: {
                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                uint8_t dst_bits = int_type_width_bits(inst->type);
                if (dst_bits > 0 && dst_bits < 64) {
                    uint8_t shift = (uint8_t)(64 - dst_bits);
                    emit_mov_imm(&ctx, X86_RCX, (int64_t)shift, false);
                    emit_shift(&ctx, 4, X86_RAX, 8); /* shl rax, cl */
                    emit_shift(&ctx, 5, X86_RAX, 8); /* shr rax, cl */
                }
                emit_store_slot(&ctx, inst->dest, X86_RAX);
                break;
            }
            case LR_OP_BITCAST:
            case LR_OP_PTRTOINT:
            case LR_OP_INTTOPTR: {
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
            case LR_OP_UITOFP: {
                uint8_t fsize = (inst->type && inst->type->kind == LR_TYPE_FLOAT) ? 4 : 8;
                emit_load_operand(&ctx, &inst->operands[0], X86_RAX);
                size_t src_sz = lr_type_size(inst->operands[0].type);
                if (src_sz <= 4) {
                    /* zero-extend to 64-bit: mov eax, eax */
                    ctx.buf[ctx.pos++] = 0x89; ctx.buf[ctx.pos++] = 0xC0;
                }
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
            case LR_OP_FPTOUI: {
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
                        emit_mov_imm(&ctx, X86_RAX, 0, false);
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
                            emit_mov_imm(&ctx, X86_RAX, 0, false);
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
                if (inst->operands[0].kind == LR_VAL_GLOBAL && ctx.mod) {
                    const char *cname = lr_module_symbol_name(
                        ctx.mod, inst->operands[0].global_id);
                    if (cname && strcmp(cname, "llvm.va_start.p0") == 0) {
                        if (ctx.func_is_vararg && inst->num_operands >= 2) {
                            emit_load_operand(&ctx, &inst->operands[1], X86_RAX);
                            uint32_t gp_off = ctx.vararg_named_gp * 8;
                            emit_mov_imm(&ctx, X86_RCX, (int64_t)gp_off, false);
                            encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x89, X86_RCX, X86_RAX, 0, 4);
                            emit_mov_imm(&ctx, X86_RCX, 48, false);
                            encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x89, X86_RCX, X86_RAX, 4, 4);
                            int32_t overflow_off = 16 + (int32_t)(ctx.vararg_named_gp > 6 ? ctx.vararg_named_gp - 6 : 0) * 8;
                            encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x8D, X86_RCX, X86_RBP, overflow_off, 8);
                            encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x89, X86_RCX, X86_RAX, 8, 8);
                            encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x8D, X86_RCX, X86_RBP, ctx.vararg_rsa_off, 8);
                            encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x89, X86_RCX, X86_RAX, 16, 8);
                            invalidate_cached_gprs(&ctx);
                        }
                        break;
                    }
                    if (cname && strcmp(cname, "llvm.va_end.p0") == 0)
                        break;
                    if (cname && strcmp(cname, "llvm.va_copy.p0") == 0) {
                        if (inst->num_operands >= 3) {
                            emit_load_operand(&ctx, &inst->operands[1], X86_RAX);
                            emit_load_operand(&ctx, &inst->operands[2], X86_RCX);
                            for (int32_t off = 0; off < 24; off += 8) {
                                encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x8B, X86_R11, X86_RCX, off, 8);
                                encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x89, X86_R11, X86_RAX, off, 8);
                            }
                            invalidate_cached_gprs(&ctx);
                        }
                        break;
                    }
                }
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
                bool internal_sret = false;
                uint32_t internal_gp_start = 0;
                uint32_t internal_gp_cap = 6;

                if (inst->operands[0].kind == LR_VAL_GLOBAL) {
                    use_external_sysv_fp =
                        call_uses_external_sysv_abi(&ctx, inst, &callee_func);
                    callee_vararg = (callee_func && callee_func->vararg) ||
                                    inst->call_vararg;
                } else {
                    use_external_sysv_fp = inst->call_external_abi;
                    callee_vararg = inst->call_vararg;
                }

                internal_sret = !use_external_sysv_fp &&
                                uses_internal_sret_abi(inst->type);
                if (internal_sret) {
                    internal_gp_start = 1;
                    internal_gp_cap = 5;
                }

                if (use_external_sysv_fp) {
                    for (uint32_t i = 0; i < nargs; i++) {
                        const lr_type_t *arg_type = inst->operands[i + 1].type;
                        uint8_t agg_lane_size = 0;
                        uint8_t agg_lane_count = 0;
                        if (is_fp_abi_type(arg_type)) {
                            if (fp_used < 8) fp_used++;
                            else stack_args++;
                        } else if (fp_abi_two_lane_aggregate(arg_type,
                                                               &agg_lane_size,
                                                               &agg_lane_count)) {
                            if (fp_used + agg_lane_count <= 8)
                                fp_used += agg_lane_count;
                            else stack_args++;
                        } else {
                            if (gp_used < 6) gp_used++;
                            else stack_args++;
                        }
                    }
                } else {
                    stack_args = nargs > internal_gp_cap ? nargs - internal_gp_cap : 0;
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
                        uint8_t agg_lane_size = 0;
                        uint8_t agg_lane_count = 0;
                        if (is_fp_abi_type(arg->type) && fp_used < 8) {
                            emit_load_fp_operand(&ctx, arg, call_fp_regs[fp_used],
                                                 fp_abi_size(arg->type));
                            fp_used++;
                            continue;
                        }
                        if (fp_abi_two_lane_aggregate(arg->type, &agg_lane_size,
                                                      &agg_lane_count) &&
                            arg->kind == LR_VAL_VREG &&
                            fp_used + agg_lane_count <= 8) {
                            int32_t src_off = alloc_slot(&ctx, arg->vreg, 8, 8);
                            emit_load_fp_mem_base(&ctx, X86_RBP, src_off,
                                                  call_fp_regs[fp_used],
                                                  agg_lane_size);
                            if (agg_lane_count > 1) {
                                emit_load_fp_mem_base(&ctx, X86_RBP,
                                                      src_off + (int32_t)agg_lane_size,
                                                      call_fp_regs[fp_used + 1],
                                                      agg_lane_size);
                            }
                            fp_used += agg_lane_count;
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
                    uint32_t nstack = nargs > internal_gp_cap ? nargs - internal_gp_cap : 0;
                    if (internal_sret) {
                        size_t dst_sz = lr_type_size(inst->type);
                        size_t dst_align = lr_type_align(inst->type);
                        int32_t dst_off;
                        if (dst_align < 8)
                            dst_align = 8;
                        if (dst_sz < 8)
                            dst_sz = 8;
                        dst_off = alloc_slot(&ctx, inst->dest, dst_sz, dst_align);
                        encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x8D,
                                   X86_RDI, X86_RBP, dst_off, 8);
                    }
                    for (uint32_t i = 0; i < nstack; i++) {
                        uint32_t arg_idx = internal_gp_cap + i;
                        emit_load_operand(&ctx, &inst->operands[arg_idx + 1], X86_RAX);
                        encode_mem(ctx.buf, &ctx.pos, ctx.buflen, 0x89, X86_RAX,
                                   X86_RSP, (int32_t)(i * 8), 8);
                    }
                    for (uint32_t i = 0; i < nargs && i < internal_gp_cap; i++)
                        emit_load_operand(&ctx, &inst->operands[i + 1],
                                          call_regs[internal_gp_start + i]);
                }

                if (callee_vararg)
                    emit_mov_imm(&ctx, X86_RAX, (int64_t)fp_used_for_call, false);

                bool emit_reloc_call = ctx.obj_ctx &&
                                       inst->operands[0].kind == LR_VAL_GLOBAL;
                if (emit_reloc_call && ctx.obj_ctx->preserve_symbol_names) {
                    const lr_operand_t *callee = &inst->operands[0];
                    const char *sym_name = lr_module_symbol_name(ctx.mod, callee->global_id);
                    bool defined = false;
                    if (sym_name) {
                        if (callee->global_id < ctx.sym_count)
                            defined = ctx.sym_defined[callee->global_id] != 0;
                        else
                            defined = is_symbol_defined_in_module(ctx.mod, sym_name);
                    }
                    emit_reloc_call = defined;
                }

                if (emit_reloc_call) {
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

                invalidate_cached_gprs(&ctx);
                if (inst->type && inst->type->kind != LR_TYPE_VOID) {
                    uint8_t ret_lane_size = 0;
                    uint8_t ret_lane_count = 0;
                    bool ret_fp_agg = use_external_sysv_fp &&
                        fp_abi_two_lane_aggregate(inst->type, &ret_lane_size,
                                                  &ret_lane_count);
                    if (internal_sret) {
                        /* Return value already materialized through hidden sret pointer. */
                    } else if (ret_fp_agg) {
                        size_t dst_sz = lr_type_size(inst->type);
                        size_t dst_align = lr_type_align(inst->type);
                        int32_t dst_off;
                        if (dst_align < 8)
                            dst_align = 8;
                        if (dst_sz < 8)
                            dst_sz = 8;
                        dst_off = alloc_slot(&ctx, inst->dest, dst_sz, dst_align);
                        emit_store_fp_mem_base(&ctx, X86_RBP, dst_off, X86_XMM0,
                                               ret_lane_size);
                        if (ret_lane_count > 1 && dst_sz >= (size_t)(2 * ret_lane_size)) {
                            emit_store_fp_mem_base(&ctx, X86_RBP,
                                                   dst_off + (int32_t)ret_lane_size,
                                                   X86_XMM1, ret_lane_size);
                        }
                    } else if (use_external_sysv_fp && is_fp_abi_type(inst->type))
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
        if (target < func->num_blocks && fix_pos + 4 <= ctx.buflen) {
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

/* Copy-and-patch backend (target_x86_64_cp.c) */
extern int x86_64_compile_func_cp(lr_func_t *func, lr_module_t *mod,
                                   uint8_t *buf, size_t buflen, size_t *out_len,
                                   lr_arena_t *arena);

/* ---- Streaming direct-emission ISel ------------------------------------ */

/*
 * True streaming backend: converts lr_compile_inst_desc_t operands to
 * stack-local lr_operand_t arrays and runs the same ISel switch as
 * x86_64_compile_func, emitting machine code immediately without
 * materializing persistent IR.
 */

typedef struct x86_stream_phi_copy {
    uint32_t pred_block_id;
    uint32_t dest_vreg;
    lr_operand_t src_op;
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
        nb[i] = 0;
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

static void direct_emit_phi_copies(x86_direct_ctx_t *ctx, uint32_t pred) {
    x86_compile_ctx_t *cc = &ctx->cc;
    for (uint32_t i = 0; i < ctx->phi_copy_count; i++) {
        if (ctx->phi_copies[i].pred_block_id != pred)
            continue;
        emit_load_operand(cc, &ctx->phi_copies[i].src_op, X86_RAX);
        emit_store_slot(cc, ctx->phi_copies[i].dest_vreg, X86_RAX);
    }
}

/* Flush a deferred terminator (BR, CONDBR, RET, RET_VOID) that was
   saved during compile_emit. Phi copies are emitted before the branch
   so that copies registered after the terminator's compile_emit call
   (e.g., from a PHI in a successor block) are included. */
static int flush_deferred_terminator(x86_direct_ctx_t *ctx) {
    x86_compile_ctx_t *cc;
    x86_deferred_term_t *dt;

    if (!ctx || !ctx->deferred.pending)
        return 0;

    cc = &ctx->cc;
    dt = &ctx->deferred;
    dt->pending = false;

    direct_emit_phi_copies(ctx, dt->block_id);

    switch (dt->op) {
    case LR_OP_RET:
        if (cc->func_uses_internal_sret) {
            size_t ret_sz = lr_type_size(ctx->ret_type);
            emit_mem_load_sized(cc, X86_RDI, X86_RBP, cc->sret_ptr_off, 8);
            if (ret_sz == 0)
                ret_sz = 8;
            if (dt->ops[0].kind == LR_VAL_VREG) {
                uint32_t vreg = dt->ops[0].vreg;
                size_t src_sz = 0;
                int32_t src_off = alloc_slot(cc, vreg, 8, 8);
                if (vreg < cc->num_stack_slots)
                    src_sz = cc->stack_slot_sizes[vreg];
                if (src_sz > ret_sz)
                    src_sz = ret_sz;
                if (src_sz > 0)
                    emit_mem_copy_base_to_base(cc, X86_RDI, 0,
                                               X86_RBP, src_off, src_sz);
                if (src_sz < ret_sz)
                    emit_mem_zero_base(cc, X86_RDI, (int32_t)src_sz,
                                       ret_sz - src_sz);
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
        } else {
            emit_load_operand(cc, &dt->ops[0], X86_RAX);
        }
        emit_epilogue(cc);
        break;
    case LR_OP_RET_VOID:
        emit_epilogue(cc);
        break;
    case LR_OP_BR: {
        if (direct_ensure_fixup_cap(ctx) != 0) return -1;
        uint32_t target_id = dt->ops[0].block_id;
        emit_jmp(cc, target_id);
        break;
    }
    case LR_OP_CONDBR: {
        emit_load_operand(cc, &dt->ops[0], X86_RAX);
        encode_alu_rr(cc->buf, &cc->pos, cc->buflen, 0x85,
                      X86_RAX, X86_RAX, 1);
        if (direct_ensure_fixup_cap(ctx) != 0) return -1;
        uint32_t true_id = dt->ops[1].block_id;
        uint32_t false_id = dt->ops[2].block_id;
        emit_jcc(cc, LR_CC_NE, true_id);
        if (direct_ensure_fixup_cap(ctx) != 0) return -1;
        emit_jmp(cc, false_id);
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
                return callee_func->first_block == NULL;
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
            return callee_func->first_block == NULL;
        }
        return !is_symbol_defined_in_module(cc->mod, sym_name);
    }

    if (out_vararg) *out_vararg = call_vararg;
    return call_external_abi;
}

static int x86_64_compile_begin(void **compile_ctx,
                                const lr_compile_func_meta_t *func_meta,
                                lr_module_t *mod,
                                uint8_t *buf, size_t buflen,
                                lr_arena_t *arena) {
    static const uint8_t param_regs[] = {
        X86_RDI, X86_RSI, X86_RDX, X86_RCX, X86_R8, X86_R9
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
    for (uint32_t i = 0; i < 8; i++) cc->block_offsets[i] = 0;
    cc->fixups = lr_arena_array_uninit(arena, x86_fixup_t, 16);
    cc->num_fixups = 0;
    cc->fixup_cap = 16;
    cc->arena = arena;
    cc->obj_ctx = NULL;
    cc->mod = mod;
    cc->sym_defined = NULL;
    cc->sym_funcs = NULL;
    cc->sym_count = 0;
    cc->rax_holds_vreg = UINT32_MAX;
    cc->rcx_holds_vreg = UINT32_MAX;
    cc->vreg_use_counts = NULL;
    cc->num_vreg_use_counts = 0;
    cc->current_block = NULL;
    cc->current_inst = NULL;
    cc->current_inst_index = 0;
    cc->func_uses_internal_sret = false;
    cc->sret_ptr_off = 0;
    cc->func_is_vararg = false;
    cc->vararg_rsa_off = 0;
    cc->vararg_named_gp = 0;

    attach_obj_symbol_meta_cache(cc);

    ctx->prologue_patch_pos = emit_prologue(cc);

    cc->func_uses_internal_sret = uses_internal_sret_abi(ret_type);
    if (cc->func_uses_internal_sret) {
        cc->sret_ptr_off = alloc_temp_slot(cc, 8, 8);
        emit_mem_store_sized(cc, X86_RDI, X86_RBP, cc->sret_ptr_off, 8);
    }

    {
        uint32_t gp_start = cc->func_uses_internal_sret ? 1u : 0u;
        uint32_t gp_cap = 6u - gp_start;
        for (uint32_t i = 0; i < num_params && i < gp_cap; i++)
            emit_store_slot(cc, param_vregs[i], param_regs[gp_start + i]);
        for (uint32_t i = gp_cap; i < num_params; i++) {
            int32_t caller_off = 16 + (int32_t)(i - gp_cap) * 8;
            encode_mem(cc->buf, &cc->pos, cc->buflen, 0x8B,
                       X86_RAX, X86_RBP, caller_off, 8);
            emit_store_slot(cc, param_vregs[i], X86_RAX);
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
    if (direct_ensure_block_offsets(ctx, block_id) != 0)
        return -1;
    /* Defer both the previous block's terminator and this block's offset
       recording. PHI instructions in the new block will register phi copies
       for predecessor blocks; the deferred terminator and block offset are
       materialized when the first non-PHI instruction is emitted. */
    ctx->current_block_id = block_id;
    ctx->has_current_block = true;
    ctx->block_offset_pending = true;
    return 0;
}

static int x86_64_compile_emit(void *compile_ctx,
                               const lr_compile_inst_desc_t *desc) {
    x86_direct_ctx_t *ctx = (x86_direct_ctx_t *)compile_ctx;
    x86_compile_ctx_t *cc;
    lr_operand_t ops[16];
    lr_inst_t inst_header;

    if (!ctx || !desc || !ctx->has_current_block)
        return -1;
    if (desc->num_operands > 0 && !desc->operands)
        return -1;
    if (desc->num_indices > 0 && !desc->indices)
        return -1;

    /* PHI instructions only register phi copies and allocate slots; they
       do not emit code and must not flush the deferred terminator. For
       all other instructions: flush the deferred terminator from the
       previous block (now that PHI copies are registered), then record
       this block's offset so branch fixups target the right position. */
    if (desc->op != LR_OP_PHI) {
        if (ctx->deferred.pending) {
            if (flush_deferred_terminator(ctx) != 0)
                return -1;
        }
        if (ctx->block_offset_pending) {
            ctx->cc.block_offsets[ctx->current_block_id] = ctx->cc.pos;
            invalidate_cached_gprs(&ctx->cc);
            ctx->block_offset_pending = false;
        }
    }

    cc = &ctx->cc;
    direct_note_vregs(ctx, desc);

    if (direct_ensure_fixup_cap(ctx) != 0)
        return -1;

    uint32_t nops = desc->num_operands;
    if (nops > 16) {
        lr_operand_t *heap_ops = lr_arena_array_uninit(
            cc->arena, lr_operand_t, nops);
        if (!heap_ops)
            return -1;
        for (uint32_t i = 0; i < nops; i++)
            heap_ops[i] = operand_from_desc(&desc->operands[i]);
        memset(&inst_header, 0, sizeof(inst_header));
        inst_header.op = desc->op;
        inst_header.operands = heap_ops;
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
        /* Copy the first 16 into the local array so the switch can
           reference ops[] uniformly for most operands. */
        memcpy(ops, heap_ops, 16 * sizeof(lr_operand_t));
    } else {
        for (uint32_t i = 0; i < nops; i++)
            ops[i] = operand_from_desc(&desc->operands[i]);
        memset(&inst_header, 0, sizeof(inst_header));
        inst_header.op = desc->op;
    }

    cc->current_inst = &inst_header;
    cc->current_block = NULL;

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
        bool use_static = (nops == 0) ||
            (ops[0].kind == LR_VAL_IMM_I64 && ops[0].imm_i64 == 1);
        if (use_static) {
            int32_t off = lr_target_lookup_static_alloca_offset(
                cc->static_alloca_offsets, cc->num_static_alloca_offsets,
                desc->dest);
            if (off == 0) {
                size_t elem_align = lr_type_align(desc->type);
                if (elem_align == 0) elem_align = 1;
                cc->stack_size = (uint32_t)align_up(cc->stack_size,
                                                     elem_align);
                cc->stack_size += (uint32_t)elem_sz;
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
                size_t src_sz = 0;
                int32_t src_off = alloc_slot(cc, vreg, 8, 8);
                if (vreg < cc->num_stack_slots)
                    src_sz = cc->stack_slot_sizes[vreg];
                if (src_sz > store_sz) src_sz = store_sz;
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

        if (nops > 0 && ops[0].type)
            have_path = lr_aggregate_index_path(
                ops[0].type, desc->indices, desc->num_indices,
                &field_off, &field_ty);
        if (field_ty)
            field_sz = lr_type_size(field_ty);
        if (field_sz == 0)
            field_sz = 8;

        if (have_path && nops > 0 && ops[0].kind == LR_VAL_VREG) {
            if (field_sz > 8) {
                size_t dst_align = desc->type ? lr_type_align(desc->type) : 8;
                if (dst_align < 8) dst_align = 8;
                int32_t dst_off = alloc_slot(cc, desc->dest, field_sz,
                                             dst_align);
                int32_t src_off = alloc_slot(cc, ops[0].vreg, 8, 8) +
                                  (int32_t)field_off;
                emit_mem_copy_base_to_base(cc, X86_RBP, dst_off,
                                           X86_RBP, src_off, field_sz);
            } else {
                emit_load_vreg_mem_sized(cc, ops[0].vreg,
                                         (int32_t)field_off, X86_RAX,
                                         (uint8_t)field_sz);
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
                size_t src_sz = 0;
                int32_t src_off = alloc_slot(cc, ops[0].vreg, 8, 8);
                if (ops[0].vreg < cc->num_stack_slots)
                    src_sz = cc->stack_slot_sizes[ops[0].vreg];
                if (src_sz > agg_sz) src_sz = agg_sz;
                if (src_sz > 0)
                    emit_mem_copy_base_to_base(cc, X86_RBP, dst_off,
                                               X86_RBP, src_off, src_sz);
                if (src_sz < agg_sz)
                    emit_mem_zero_base(cc, X86_RBP,
                                       dst_off + (int32_t)src_sz,
                                       agg_sz - src_sz);
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
                    size_t src_sz = 0;
                    int32_t src_off = alloc_slot(cc, ops[1].vreg, 8, 8);
                    if (ops[1].vreg < cc->num_stack_slots)
                        src_sz = cc->stack_slot_sizes[ops[1].vreg];
                    if (src_sz > field_sz) src_sz = field_sz;
                    if (src_sz > 0)
                        emit_mem_copy_base_to_base(
                            cc, X86_RBP,
                            dst_off + (int32_t)field_off,
                            X86_RBP, src_off, src_sz);
                    if (src_sz < field_sz)
                        emit_mem_zero_base(
                            cc, X86_RBP,
                            dst_off + (int32_t)field_off + (int32_t)src_sz,
                            field_sz - src_sz);
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
                const lr_type_t *arg_type = ops[i + 1].type;
                uint8_t agg_lane_size = 0;
                uint8_t agg_lane_count = 0;
                if (is_fp_abi_type(arg_type)) {
                    if (fp_used < 8) fp_used++;
                    else stack_args++;
                } else if (fp_abi_two_lane_aggregate(arg_type,
                                                      &agg_lane_size,
                                                      &agg_lane_count)) {
                    if (fp_used + agg_lane_count <= 8)
                        fp_used += agg_lane_count;
                    else stack_args++;
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
                uint8_t agg_lane_size = 0;
                uint8_t agg_lane_count = 0;
                if (is_fp_abi_type(ops[i + 1].type) && fp_used < 8) {
                    emit_load_fp_operand(cc, &ops[i + 1],
                                         call_fp_regs[fp_used],
                                         fp_abi_size(ops[i + 1].type));
                    fp_used++;
                    continue;
                }
                if (fp_abi_two_lane_aggregate(ops[i + 1].type,
                                              &agg_lane_size,
                                              &agg_lane_count) &&
                    ops[i + 1].kind == LR_VAL_VREG &&
                    fp_used + agg_lane_count <= 8) {
                    int32_t src_off = alloc_slot(cc, ops[i + 1].vreg,
                                                 8, 8);
                    emit_load_fp_mem_base(cc, X86_RBP, src_off,
                                          call_fp_regs[fp_used],
                                          agg_lane_size);
                    if (agg_lane_count > 1)
                        emit_load_fp_mem_base(
                            cc, X86_RBP,
                            src_off + (int32_t)agg_lane_size,
                            call_fp_regs[fp_used + 1],
                            agg_lane_size);
                    fp_used += agg_lane_count;
                    continue;
                }
                if (!is_fp_abi_type(ops[i + 1].type) && gp_used < 6) {
                    emit_load_operand(cc, &ops[i + 1],
                                      call_regs[gp_used]);
                    gp_used++;
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
            bool ret_fp_agg = use_external_sysv_fp &&
                fp_abi_two_lane_aggregate(desc->type, &ret_lane_size,
                                          &ret_lane_count);
            if (internal_sret) {
                /* Already materialized through hidden sret pointer. */
            } else if (ret_fp_agg) {
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
            } else if (use_external_sysv_fp &&
                       is_fp_abi_type(desc->type)) {
                emit_store_fp_slot(cc, desc->dest, X86_XMM0,
                                   fp_abi_size(desc->type));
            } else {
                emit_store_slot(cc, desc->dest, X86_RAX);
            }
        }
        break;
    }
    case LR_OP_PHI:
        (void)alloc_slot(cc, desc->dest, 8, 8);
        break;
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
        ctx->block_offset_pending = false;
    }
    if (flush_deferred_terminator(ctx) != 0)
        return -1;

    cc = &ctx->cc;

    for (uint32_t i = 0; i < cc->num_fixups; i++) {
        size_t fix_pos = cc->fixups[i].pos;
        uint32_t target = cc->fixups[i].target;
        if (target < cc->num_block_offsets && fix_pos + 4 <= cc->buflen) {
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
    entry->dest_vreg = dest_vreg;
    entry->src_op = operand_from_desc(src_op);
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
    .compile_func = x86_64_compile_func,
};

const lr_target_t *lr_target_x86_64(void) {
    return &x86_64_target;
}
