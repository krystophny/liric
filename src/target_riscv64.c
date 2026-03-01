#include "target_riscv64.h"

#include "ir.h"
#include "objfile.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RV_OPCODE_OP     0x33u
#define RV_OPCODE_OPIMM  0x13u
#define RV_OPCODE_LUI    0x37u
#define RV_OPCODE_JAL    0x6Fu
#define RV_OPCODE_JALR   0x67u
#define RV_OPCODE_OPFP   0x53u

#define RV_FUNCT3_ADD_SUB 0x0u
#define RV_FUNCT3_AND      0x7u
#define RV_FUNCT3_OR       0x6u
#define RV_FUNCT3_XOR      0x4u
#define RV_FUNCT3_SLL      0x1u
#define RV_FUNCT3_SRL_SRA  0x5u
#define RV_FUNCT3_DIV      0x4u
#define RV_FUNCT3_DIVU     0x5u
#define RV_FUNCT3_REM      0x6u
#define RV_FUNCT3_REMU     0x7u

#define RV_FUNCT7_ADD     0x00u
#define RV_FUNCT7_SUB     0x20u
#define RV_FUNCT7_MULDIV  0x01u
#define RV_FUNCT7_SRL     0x00u
#define RV_FUNCT7_SRA     0x20u

typedef enum rv_reg_class {
    RV_REGCLS_GPR = 1,
    RV_REGCLS_FPR = 2,
} rv_reg_class_t;

typedef struct rv_emit_ctx {
    uint8_t *buf;
    size_t buflen;
    size_t pos;
} rv_emit_ctx_t;

typedef struct rv_vreg_map {
    uint8_t in_use;
    uint8_t cls;
    uint8_t reg;
} rv_vreg_map_t;

typedef struct rv_features {
    const char *name;
    bool ext_m;
    bool ext_f;
    bool ext_d;
} rv_features_t;

enum {
    RV_ERR_UNSUPPORTED_OP = -2,
};

static int rv_emit32(rv_emit_ctx_t *ec, uint32_t insn) {
    if (!ec || ec->pos + 4 > ec->buflen)
        return -1;
    ec->buf[ec->pos + 0] = (uint8_t)(insn);
    ec->buf[ec->pos + 1] = (uint8_t)(insn >> 8);
    ec->buf[ec->pos + 2] = (uint8_t)(insn >> 16);
    ec->buf[ec->pos + 3] = (uint8_t)(insn >> 24);
    ec->pos += 4;
    return 0;
}

static uint32_t rv_enc_r(uint8_t funct7, uint8_t rs2, uint8_t rs1,
                         uint8_t funct3, uint8_t rd, uint8_t opcode) {
    return ((uint32_t)(funct7 & 0x7Fu) << 25)
         | ((uint32_t)(rs2 & 0x1Fu) << 20)
         | ((uint32_t)(rs1 & 0x1Fu) << 15)
         | ((uint32_t)(funct3 & 0x7u) << 12)
         | ((uint32_t)(rd & 0x1Fu) << 7)
         | (uint32_t)(opcode & 0x7Fu);
}

static uint32_t rv_enc_i(int32_t imm, uint8_t rs1, uint8_t funct3,
                         uint8_t rd, uint8_t opcode) {
    uint32_t uimm = (uint32_t)imm & 0xFFFu;
    return (uimm << 20)
         | ((uint32_t)(rs1 & 0x1Fu) << 15)
         | ((uint32_t)(funct3 & 0x7u) << 12)
         | ((uint32_t)(rd & 0x1Fu) << 7)
         | (uint32_t)(opcode & 0x7Fu);
}

static uint32_t rv_enc_u(int32_t imm20, uint8_t rd, uint8_t opcode) {
    return ((uint32_t)imm20 & 0xFFFFF000u)
         | ((uint32_t)(rd & 0x1Fu) << 7)
         | (uint32_t)(opcode & 0x7Fu);
}

static uint32_t rv_enc_j(uint8_t rd, uint8_t opcode) {
    /* J-type with zero immediate -- offset filled by relocation patching */
    return ((uint32_t)(rd & 0x1Fu) << 7)
         | (uint32_t)(opcode & 0x7Fu);
}

static int rv_type_is_fp(const lr_type_t *t) {
    return t && (t->kind == LR_TYPE_FLOAT || t->kind == LR_TYPE_DOUBLE);
}

static int rv_type_is_intlike(const lr_type_t *t) {
    if (!t)
        return 0;
    return t->kind == LR_TYPE_I1 || t->kind == LR_TYPE_I8 ||
           t->kind == LR_TYPE_I16 || t->kind == LR_TYPE_I32 ||
           t->kind == LR_TYPE_I64 || t->kind == LR_TYPE_PTR;
}

static int rv_emit_shift_imm(rv_emit_ctx_t *ec, uint8_t rd, uint8_t rs,
                             uint8_t funct3, uint8_t shamt) {
    return rv_emit32(ec, rv_enc_i((int32_t)(shamt & 0x3Fu), rs, funct3, rd, RV_OPCODE_OPIMM));
}

static int rv_emit_mv(rv_emit_ctx_t *ec, uint8_t rd, uint8_t rs) {
    return rv_emit32(ec, rv_enc_i(0, rs, RV_FUNCT3_ADD_SUB, rd, RV_OPCODE_OPIMM));
}

static int rv_emit_li32(rv_emit_ctx_t *ec, uint8_t rd, int32_t imm) {
    if (imm >= -2048 && imm <= 2047)
        return rv_emit32(ec, rv_enc_i(imm, RV_X0, RV_FUNCT3_ADD_SUB, rd, RV_OPCODE_OPIMM));

    int32_t hi20 = (imm + 0x800) & ~0xFFF;
    int32_t lo12 = imm - hi20;
    if (rv_emit32(ec, rv_enc_u(hi20, rd, RV_OPCODE_LUI)) != 0)
        return -1;
    return rv_emit32(ec, rv_enc_i(lo12, rd, RV_FUNCT3_ADD_SUB, rd, RV_OPCODE_OPIMM));
}

static int rv_emit_li64(rv_emit_ctx_t *ec, uint8_t rd, uint8_t scratch, int64_t imm) {
    if (imm >= INT32_MIN && imm <= INT32_MAX)
        return rv_emit_li32(ec, rd, (int32_t)imm);

    if (rd == scratch)
        return -1;

    uint64_t u = (uint64_t)imm;
    uint32_t hi = (uint32_t)(u >> 32);
    uint32_t lo = (uint32_t)u;

    if (rv_emit_li32(ec, rd, (int32_t)hi) != 0)
        return -1;
    if (rv_emit_shift_imm(ec, rd, rd, RV_FUNCT3_SLL, 32) != 0)
        return -1;

    if (rv_emit_li32(ec, scratch, (int32_t)lo) != 0)
        return -1;
    if (rv_emit_shift_imm(ec, scratch, scratch, RV_FUNCT3_SLL, 32) != 0)
        return -1;
    if (rv_emit_shift_imm(ec, scratch, scratch, RV_FUNCT3_SRL_SRA, 32) != 0)
        return -1;

    return rv_emit32(ec, rv_enc_r(RV_FUNCT7_ADD, scratch, rd,
                                  RV_FUNCT3_ADD_SUB, rd, RV_OPCODE_OP));
}

static int rv_emit_fp_move(rv_emit_ctx_t *ec, uint8_t rd, uint8_t rs, bool is_double) {
    uint8_t funct7 = is_double ? 0x11u : 0x10u;
    return rv_emit32(ec, rv_enc_r(funct7, rs, rs, 0x0u, rd, RV_OPCODE_OPFP));
}

static int rv_emit_fmv_w_x(rv_emit_ctx_t *ec, uint8_t fd, uint8_t rs) {
    return rv_emit32(ec, rv_enc_r(0x78u, 0u, rs, 0u, fd, RV_OPCODE_OPFP));
}

static int rv_emit_fmv_d_x(rv_emit_ctx_t *ec, uint8_t fd, uint8_t rs) {
    return rv_emit32(ec, rv_enc_r(0x79u, 0u, rs, 0u, fd, RV_OPCODE_OPFP));
}

static int rv_operand_gpr(rv_emit_ctx_t *ec,
                          const lr_operand_t *op,
                          const rv_vreg_map_t *vmap,
                          uint32_t vmap_n,
                          uint8_t scratch1,
                          uint8_t scratch2,
                          uint8_t *out_reg) {
    if (!ec || !op || !out_reg)
        return -1;

    if (op->kind == LR_VAL_VREG) {
        if (op->vreg >= vmap_n || !vmap[op->vreg].in_use || vmap[op->vreg].cls != RV_REGCLS_GPR)
            return -1;
        *out_reg = vmap[op->vreg].reg;
        return 0;
    }

    if (op->kind == LR_VAL_IMM_I64) {
        if (rv_emit_li64(ec, scratch1, scratch2, op->imm_i64) != 0)
            return -1;
        *out_reg = scratch1;
        return 0;
    }

    return -1;
}

static int rv_operand_fpr(rv_emit_ctx_t *ec,
                          const lr_operand_t *op,
                          const rv_vreg_map_t *vmap,
                          uint32_t vmap_n,
                          uint8_t gpr_s1,
                          uint8_t gpr_s2,
                          uint8_t fpr_s,
                          const rv_features_t *feat,
                          uint8_t *out_reg) {
    if (!ec || !op || !feat || !out_reg)
        return -1;

    if (!feat->ext_f)
        return -1;

    if (op->kind == LR_VAL_VREG) {
        if (op->vreg >= vmap_n || !vmap[op->vreg].in_use || vmap[op->vreg].cls != RV_REGCLS_FPR)
            return -1;
        *out_reg = vmap[op->vreg].reg;
        return 0;
    }

    if (op->kind == LR_VAL_IMM_F64) {
        if (!op->type)
            return -1;
        if (op->type->kind == LR_TYPE_FLOAT) {
            float f = (float)op->imm_f64;
            uint32_t bits = 0;
            memcpy(&bits, &f, sizeof(bits));
            if (rv_emit_li64(ec, gpr_s1, gpr_s2, (int64_t)(int32_t)bits) != 0)
                return -1;
            if (rv_emit_fmv_w_x(ec, fpr_s, gpr_s1) != 0)
                return -1;
            *out_reg = fpr_s;
            return 0;
        }
        if (op->type->kind == LR_TYPE_DOUBLE) {
            if (!feat->ext_d)
                return -1;
            uint64_t bits = 0;
            memcpy(&bits, &op->imm_f64, sizeof(bits));
            if (rv_emit_li64(ec, gpr_s1, gpr_s2, (int64_t)bits) != 0)
                return -1;
            if (rv_emit_fmv_d_x(ec, fpr_s, gpr_s1) != 0)
                return -1;
            *out_reg = fpr_s;
            return 0;
        }
        return -1;
    }

    return -1;
}

/* ---- Streaming direct-emission ISel ------------------------------------ */

typedef struct rv_direct_ctx {
    rv_emit_ctx_t ec;
    rv_vreg_map_t *vmap;
    uint32_t vmap_n;
    size_t gpr_next;
    size_t fpr_next;
    lr_module_t *mod;
    lr_arena_t *arena;
    lr_compile_mode_t mode;
    const rv_features_t *feat;
    uint32_t current_block_id;
    bool has_current_block;
    bool ra_saved;
    uint32_t next_vreg;
} rv_direct_ctx_t;

static lr_operand_t rv_operand_from_desc(const lr_operand_desc_t *desc) {
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

static void rv_direct_note_vregs(rv_direct_ctx_t *ctx,
                                 const lr_compile_inst_desc_t *desc) {
    if (desc->dest != 0 && desc->dest >= ctx->next_vreg)
        ctx->next_vreg = desc->dest + 1u;
    for (uint32_t i = 0; i < desc->num_operands; i++) {
        if (desc->operands[i].kind == LR_OP_KIND_VREG &&
            desc->operands[i].vreg >= ctx->next_vreg)
            ctx->next_vreg = desc->operands[i].vreg + 1u;
    }
}

static int rv_direct_ensure_vmap(rv_direct_ctx_t *ctx, uint32_t vreg) {
    if (vreg < ctx->vmap_n)
        return 0;
    uint32_t new_n = ctx->vmap_n == 0 ? 64u : ctx->vmap_n;
    while (new_n <= vreg)
        new_n *= 2u;
    rv_vreg_map_t *nv = lr_arena_array(ctx->arena, rv_vreg_map_t, new_n);
    if (!nv)
        return -1;
    if (ctx->vmap_n > 0)
        memcpy(nv, ctx->vmap, sizeof(rv_vreg_map_t) * ctx->vmap_n);
    ctx->vmap = nv;
    ctx->vmap_n = new_n;
    return 0;
}

static const uint8_t rv_gpr_tmp_pool[] = {
    RV_T3, RV_T4, RV_T5, RV_T6,
    RV_S2, RV_S3, RV_S4, RV_S5, RV_S6, RV_S7, RV_S8, RV_S9, RV_S10, RV_S11,
};
static const uint8_t rv_fpr_tmp_pool[] = {
    RV_FT0, RV_FT1, RV_FT2, RV_FT3, RV_FT4, RV_FT5, RV_FT6, RV_FT7,
    RV_FT8, RV_FT9, RV_FT10, RV_FT11, RV_FS2, RV_FS3, RV_FS4, RV_FS5,
};

static int rv_compile_begin_common(void **compile_ctx,
                                   const lr_compile_func_meta_t *func_meta,
                                   lr_module_t *mod,
                                   uint8_t *buf, size_t buflen,
                                   lr_arena_t *arena,
                                   const rv_features_t *feat) {
    rv_direct_ctx_t *ctx = NULL;
    if (!compile_ctx || !func_meta || !mod || !arena || !feat)
        return -1;

    ctx = lr_arena_new(arena, rv_direct_ctx_t);
    if (!ctx)
        return -1;
    memset(ctx, 0, sizeof(*ctx));
    ctx->ec.buf = buf;
    ctx->ec.buflen = buflen;
    ctx->ec.pos = 0;
    ctx->mod = mod;
    ctx->arena = arena;
    ctx->mode = func_meta->mode;
    ctx->feat = feat;
    ctx->next_vreg = func_meta->next_vreg;

    uint32_t initial_vmap = ctx->next_vreg > 64 ? ctx->next_vreg : 64;
    ctx->vmap = lr_arena_array(arena, rv_vreg_map_t, initial_vmap);
    if (!ctx->vmap)
        return -1;
    ctx->vmap_n = initial_vmap;

    uint32_t *param_vregs = NULL;
    uint32_t num_params = func_meta->num_params;

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

    uint8_t next_iarg = RV_A0;
    uint8_t next_farg = RV_FA0;
    for (uint32_t i = 0; i < num_params; i++) {
        uint32_t v = param_vregs[i];
        lr_type_t *pt = func_meta->param_types ? func_meta->param_types[i] : NULL;
        if (rv_direct_ensure_vmap(ctx, v) != 0)
            return -1;
        if (rv_type_is_fp(pt)) {
            if ((pt->kind == LR_TYPE_DOUBLE && !feat->ext_d) ||
                (pt->kind == LR_TYPE_FLOAT && !feat->ext_f) ||
                next_farg > RV_FA7)
                return -1;
            ctx->vmap[v].in_use = 1;
            ctx->vmap[v].cls = RV_REGCLS_FPR;
            ctx->vmap[v].reg = next_farg++;
        } else {
            if (!rv_type_is_intlike(pt) || next_iarg > RV_A7)
                return -1;
            ctx->vmap[v].in_use = 1;
            ctx->vmap[v].cls = RV_REGCLS_GPR;
            ctx->vmap[v].reg = next_iarg++;
        }
    }

    *compile_ctx = ctx;
    return 0;
}

static int rv_compile_emit(void *compile_ctx,
                           const lr_compile_inst_desc_t *desc) {
    rv_direct_ctx_t *ctx = (rv_direct_ctx_t *)compile_ctx;
    lr_operand_t ops[16];

    if (!ctx || !desc || !ctx->has_current_block)
        return -1;
    if (desc->num_operands > 0 && !desc->operands)
        return -1;

    rv_direct_note_vregs(ctx, desc);

    uint32_t nops = desc->num_operands;
    for (uint32_t i = 0; i < nops && i < 16; i++)
        ops[i] = rv_operand_from_desc(&desc->operands[i]);

    rv_emit_ctx_t *ec = &ctx->ec;
    const rv_features_t *feat = ctx->feat;

    if (rv_direct_ensure_vmap(ctx, desc->dest) != 0)
        return -1;

    switch (desc->op) {
    case LR_OP_ALLOCA:
    case LR_OP_BR:
    case LR_OP_CONDBR:
    case LR_OP_EXTRACTVALUE:
    case LR_OP_FCMP:
    case LR_OP_FPTOUI:
    case LR_OP_FREM:
    case LR_OP_GEP:
    case LR_OP_ICMP:
    case LR_OP_INSERTVALUE:
    case LR_OP_INTTOPTR:
    case LR_OP_LOAD:
    case LR_OP_PTRTOINT:
    case LR_OP_SELECT:
    case LR_OP_STORE:
    case LR_OP_UITOFP:
    case LR_OP_UNREACHABLE:
        return RV_ERR_UNSUPPORTED_OP;
    case LR_OP_CALL: {
        if (nops < 1 || ops[0].kind != LR_VAL_GLOBAL || !ctx->mod)
            return -1;

        const char *callee_name = lr_module_symbol_name(
            ctx->mod, ops[0].global_id);
        if (!callee_name)
            return -1;

        lr_objfile_ctx_t *oc = (lr_objfile_ctx_t *)ctx->mod->obj_ctx;
        if (!oc)
            return -1;

        /* Save ra to s1 before the first call so ret can restore it */
        if (!ctx->ra_saved) {
            if (rv_emit_mv(ec, RV_S1, RV_RA) != 0)
                return -1;
            ctx->ra_saved = true;
        }

        /* Move arguments into ABI registers */
        uint32_t nargs = nops - 1;
        uint8_t next_iarg = RV_A0;
        uint8_t next_farg = RV_FA0;
        for (uint32_t i = 0; i < nargs; i++) {
            const lr_type_t *at = ops[i + 1].type;
            if (rv_type_is_fp(at)) {
                if (next_farg > RV_FA7)
                    return -1;
                uint8_t src = 0;
                if (rv_operand_fpr(ec, &ops[i + 1], ctx->vmap,
                                   ctx->vmap_n, RV_T1, RV_T2,
                                   RV_FT0, feat, &src) != 0)
                    return -1;
                bool is_double = at->kind == LR_TYPE_DOUBLE;
                if (src != next_farg &&
                    rv_emit_fp_move(ec, next_farg, src, is_double) != 0)
                    return -1;
                next_farg++;
            } else {
                if (next_iarg > RV_A7)
                    return -1;
                uint8_t src = 0;
                if (rv_operand_gpr(ec, &ops[i + 1], ctx->vmap,
                                   ctx->vmap_n, RV_T1, RV_T0,
                                   &src) != 0)
                    return -1;
                if (src != next_iarg &&
                    rv_emit_mv(ec, next_iarg, src) != 0)
                    return -1;
                next_iarg++;
            }
        }

        /* Emit JAL ra, 0 (placeholder) and record relocation */
        uint32_t sym_idx = lr_obj_ensure_symbol(oc, callee_name,
                                                false, 0, 0);
        if (sym_idx == UINT32_MAX)
            return -1;
        uint32_t jal_off = (uint32_t)ec->pos;
        if (rv_emit32(ec, rv_enc_j(RV_RA, RV_OPCODE_JAL)) != 0)
            return -1;
        lr_obj_add_reloc(oc, jal_off, sym_idx, LR_RELOC_RISCV64_JAL);

        /* Map return value */
        if (desc->type && desc->type->kind != LR_TYPE_VOID) {
            if (rv_direct_ensure_vmap(ctx, desc->dest) != 0)
                return -1;
            if (rv_type_is_fp(desc->type)) {
                ctx->vmap[desc->dest].in_use = 1;
                ctx->vmap[desc->dest].cls = RV_REGCLS_FPR;
                ctx->vmap[desc->dest].reg = RV_FA0;
            } else {
                ctx->vmap[desc->dest].in_use = 1;
                ctx->vmap[desc->dest].cls = RV_REGCLS_GPR;
                ctx->vmap[desc->dest].reg = RV_A0;
            }
        }
        break;
    }
    case LR_OP_ADD: case LR_OP_SUB: case LR_OP_MUL:
    case LR_OP_SDIV: case LR_OP_SREM:
    case LR_OP_UDIV: case LR_OP_UREM:
    case LR_OP_AND: case LR_OP_OR: case LR_OP_XOR:
    case LR_OP_SHL: case LR_OP_LSHR: case LR_OP_ASHR: {
        if (!rv_type_is_intlike(desc->type) || nops != 2)
            return -1;
        if ((desc->op == LR_OP_MUL || desc->op == LR_OP_SDIV ||
             desc->op == LR_OP_SREM || desc->op == LR_OP_UDIV ||
             desc->op == LR_OP_UREM) && !feat->ext_m)
            return -1;
        if (ctx->gpr_next >= sizeof(rv_gpr_tmp_pool))
            return -1;

        uint8_t rd = rv_gpr_tmp_pool[ctx->gpr_next++];
        uint8_t rs1 = 0, rs2 = 0;
        if (rv_operand_gpr(ec, &ops[0], ctx->vmap, ctx->vmap_n,
                           RV_T1, RV_T0, &rs1) != 0 ||
            rv_operand_gpr(ec, &ops[1], ctx->vmap, ctx->vmap_n,
                           RV_T2, RV_T0, &rs2) != 0)
            return -1;

        uint32_t enc = 0;
        switch (desc->op) {
        case LR_OP_ADD: enc = rv_enc_r(RV_FUNCT7_ADD, rs2, rs1, RV_FUNCT3_ADD_SUB, rd, RV_OPCODE_OP); break;
        case LR_OP_SUB: enc = rv_enc_r(RV_FUNCT7_SUB, rs2, rs1, RV_FUNCT3_ADD_SUB, rd, RV_OPCODE_OP); break;
        case LR_OP_MUL: enc = rv_enc_r(RV_FUNCT7_MULDIV, rs2, rs1, RV_FUNCT3_ADD_SUB, rd, RV_OPCODE_OP); break;
        case LR_OP_SDIV: enc = rv_enc_r(RV_FUNCT7_MULDIV, rs2, rs1, RV_FUNCT3_DIV, rd, RV_OPCODE_OP); break;
        case LR_OP_SREM: enc = rv_enc_r(RV_FUNCT7_MULDIV, rs2, rs1, RV_FUNCT3_REM, rd, RV_OPCODE_OP); break;
        case LR_OP_UDIV: enc = rv_enc_r(RV_FUNCT7_MULDIV, rs2, rs1, RV_FUNCT3_DIVU, rd, RV_OPCODE_OP); break;
        case LR_OP_UREM: enc = rv_enc_r(RV_FUNCT7_MULDIV, rs2, rs1, RV_FUNCT3_REMU, rd, RV_OPCODE_OP); break;
        case LR_OP_AND: enc = rv_enc_r(RV_FUNCT7_ADD, rs2, rs1, RV_FUNCT3_AND, rd, RV_OPCODE_OP); break;
        case LR_OP_OR: enc = rv_enc_r(RV_FUNCT7_ADD, rs2, rs1, RV_FUNCT3_OR, rd, RV_OPCODE_OP); break;
        case LR_OP_XOR: enc = rv_enc_r(RV_FUNCT7_ADD, rs2, rs1, RV_FUNCT3_XOR, rd, RV_OPCODE_OP); break;
        case LR_OP_SHL: enc = rv_enc_r(RV_FUNCT7_ADD, rs2, rs1, RV_FUNCT3_SLL, rd, RV_OPCODE_OP); break;
        case LR_OP_LSHR: enc = rv_enc_r(RV_FUNCT7_SRL, rs2, rs1, RV_FUNCT3_SRL_SRA, rd, RV_OPCODE_OP); break;
        case LR_OP_ASHR: enc = rv_enc_r(RV_FUNCT7_SRA, rs2, rs1, RV_FUNCT3_SRL_SRA, rd, RV_OPCODE_OP); break;
        default: return -1;
        }
        if (rv_emit32(ec, enc) != 0)
            return -1;
        ctx->vmap[desc->dest].in_use = 1;
        ctx->vmap[desc->dest].cls = RV_REGCLS_GPR;
        ctx->vmap[desc->dest].reg = rd;
        break;
    }
    case LR_OP_FADD: case LR_OP_FSUB:
    case LR_OP_FMUL: case LR_OP_FDIV:
    case LR_OP_FNEG: {
        bool is_double = desc->type && desc->type->kind == LR_TYPE_DOUBLE;
        bool is_float = desc->type && desc->type->kind == LR_TYPE_FLOAT;
        if ((!is_float && !is_double) ||
            (is_double && !feat->ext_d) || (is_float && !feat->ext_f))
            return -1;
        if (ctx->fpr_next >= sizeof(rv_fpr_tmp_pool))
            return -1;

        uint8_t rd = rv_fpr_tmp_pool[ctx->fpr_next++];
        uint8_t rs1 = 0, rs2 = 0;
        uint8_t funct7 = 0, rm = 0;

        if (desc->op == LR_OP_FNEG) {
            if (nops != 1 ||
                rv_operand_fpr(ec, &ops[0], ctx->vmap, ctx->vmap_n,
                               RV_T1, RV_T2, RV_FT0, feat, &rs1) != 0)
                return -1;
            funct7 = is_double ? 0x11u : 0x10u;
            rm = 0x1u;
            rs2 = rs1;
        } else {
            if (nops != 2 ||
                rv_operand_fpr(ec, &ops[0], ctx->vmap, ctx->vmap_n,
                               RV_T1, RV_T2, RV_FT0, feat, &rs1) != 0 ||
                rv_operand_fpr(ec, &ops[1], ctx->vmap, ctx->vmap_n,
                               RV_T1, RV_T2, RV_FT1, feat, &rs2) != 0)
                return -1;
            uint8_t funct7_base = 0;
            switch (desc->op) {
            case LR_OP_FADD: funct7_base = 0x00u; break;
            case LR_OP_FSUB: funct7_base = 0x04u; break;
            case LR_OP_FMUL: funct7_base = 0x08u; break;
            case LR_OP_FDIV: funct7_base = 0x0Cu; break;
            default: return -1;
            }
            funct7 = funct7_base + (is_double ? 1u : 0u);
            rm = 0u;
        }

        if (rv_emit32(ec, rv_enc_r(funct7, rs2, rs1, rm, rd, RV_OPCODE_OPFP)) != 0)
            return -1;
        ctx->vmap[desc->dest].in_use = 1;
        ctx->vmap[desc->dest].cls = RV_REGCLS_FPR;
        ctx->vmap[desc->dest].reg = rd;
        break;
    }
    case LR_OP_SITOFP: {
        bool is_double = desc->type && desc->type->kind == LR_TYPE_DOUBLE;
        bool is_float = desc->type && desc->type->kind == LR_TYPE_FLOAT;
        if ((!is_float && !is_double) || nops != 1 ||
            (is_double && !feat->ext_d) || (is_float && !feat->ext_f) ||
            ctx->fpr_next >= sizeof(rv_fpr_tmp_pool))
            return -1;
        uint8_t rs1 = 0;
        if (rv_operand_gpr(ec, &ops[0], ctx->vmap, ctx->vmap_n,
                           RV_T1, RV_T2, &rs1) != 0)
            return -1;
        uint8_t rd = rv_fpr_tmp_pool[ctx->fpr_next++];
        uint8_t f7 = is_double ? 0x69u : 0x68u;
        if (rv_emit32(ec, rv_enc_r(f7, 0x2u, rs1, 0x0u, rd, RV_OPCODE_OPFP)) != 0)
            return -1;
        ctx->vmap[desc->dest].in_use = 1;
        ctx->vmap[desc->dest].cls = RV_REGCLS_FPR;
        ctx->vmap[desc->dest].reg = rd;
        break;
    }
    case LR_OP_FPTOSI: {
        if (!rv_type_is_intlike(desc->type) || nops != 1 ||
            ctx->gpr_next >= sizeof(rv_gpr_tmp_pool))
            return -1;
        bool src_double = ops[0].type && ops[0].type->kind == LR_TYPE_DOUBLE;
        bool src_float = ops[0].type && ops[0].type->kind == LR_TYPE_FLOAT;
        if ((!src_float && !src_double) ||
            (src_double && !feat->ext_d) || (src_float && !feat->ext_f))
            return -1;
        uint8_t frs = 0;
        if (rv_operand_fpr(ec, &ops[0], ctx->vmap, ctx->vmap_n,
                           RV_T1, RV_T2, RV_FT0, feat, &frs) != 0)
            return -1;
        uint8_t rd = rv_gpr_tmp_pool[ctx->gpr_next++];
        uint8_t f7 = src_double ? 0x61u : 0x60u;
        if (rv_emit32(ec, rv_enc_r(f7, 0x2u, frs, 0x1u, rd, RV_OPCODE_OPFP)) != 0)
            return -1;
        ctx->vmap[desc->dest].in_use = 1;
        ctx->vmap[desc->dest].cls = RV_REGCLS_GPR;
        ctx->vmap[desc->dest].reg = rd;
        break;
    }
    case LR_OP_FPEXT:
    case LR_OP_FPTRUNC: {
        bool to_double = desc->op == LR_OP_FPEXT;
        if (nops != 1 || ctx->fpr_next >= sizeof(rv_fpr_tmp_pool) || !feat->ext_d)
            return -1;
        if (!ops[0].type || !desc->type)
            return -1;
        if (to_double && !(ops[0].type->kind == LR_TYPE_FLOAT &&
                           desc->type->kind == LR_TYPE_DOUBLE))
            return -1;
        if (!to_double && !(ops[0].type->kind == LR_TYPE_DOUBLE &&
                            desc->type->kind == LR_TYPE_FLOAT))
            return -1;
        uint8_t frs = 0;
        if (rv_operand_fpr(ec, &ops[0], ctx->vmap, ctx->vmap_n,
                           RV_T1, RV_T2, RV_FT0, feat, &frs) != 0)
            return -1;
        uint8_t rd = rv_fpr_tmp_pool[ctx->fpr_next++];
        uint8_t f7 = to_double ? 0x21u : 0x20u;
        uint8_t rs2_val = to_double ? 0u : 1u;
        if (rv_emit32(ec, rv_enc_r(f7, rs2_val, frs, 0x0u, rd, RV_OPCODE_OPFP)) != 0)
            return -1;
        ctx->vmap[desc->dest].in_use = 1;
        ctx->vmap[desc->dest].cls = RV_REGCLS_FPR;
        ctx->vmap[desc->dest].reg = rd;
        break;
    }
    case LR_OP_TRUNC: case LR_OP_ZEXT:
    case LR_OP_SEXT: case LR_OP_BITCAST: {
        if (nops != 1)
            return -1;
        if (rv_type_is_intlike(desc->type) && rv_type_is_intlike(ops[0].type)) {
            uint8_t rs = 0;
            if (rv_operand_gpr(ec, &ops[0], ctx->vmap, ctx->vmap_n,
                               RV_T1, RV_T2, &rs) != 0)
                return -1;
            if (ctx->gpr_next >= sizeof(rv_gpr_tmp_pool))
                return -1;
            uint8_t rd = rv_gpr_tmp_pool[ctx->gpr_next++];
            if (rv_emit_mv(ec, rd, rs) != 0)
                return -1;
            ctx->vmap[desc->dest].in_use = 1;
            ctx->vmap[desc->dest].cls = RV_REGCLS_GPR;
            ctx->vmap[desc->dest].reg = rd;
            break;
        }
        return -1;
    }
    case LR_OP_RET: {
        if (nops != 1)
            return -1;
        if (rv_type_is_fp(ops[0].type)) {
            uint8_t src = 0;
            if (rv_operand_fpr(ec, &ops[0], ctx->vmap, ctx->vmap_n,
                               RV_T1, RV_T2, RV_FT0, feat, &src) != 0)
                return -1;
            bool is_double = ops[0].type->kind == LR_TYPE_DOUBLE;
            if (src != RV_FA0 &&
                rv_emit_fp_move(ec, RV_FA0, src, is_double) != 0)
                return -1;
        } else {
            uint8_t src = 0;
            if (rv_operand_gpr(ec, &ops[0], ctx->vmap, ctx->vmap_n,
                               RV_T1, RV_T2, &src) != 0)
                return -1;
            if (src != RV_A0 && rv_emit_mv(ec, RV_A0, src) != 0)
                return -1;
        }
        if (ctx->ra_saved && rv_emit_mv(ec, RV_RA, RV_S1) != 0)
            return -1;
        if (rv_emit32(ec, rv_enc_i(0, RV_RA, 0, RV_X0, RV_OPCODE_JALR)) != 0)
            return -1;
        break;
    }
    case LR_OP_RET_VOID:
        if (ctx->ra_saved && rv_emit_mv(ec, RV_RA, RV_S1) != 0)
            return -1;
        if (rv_emit32(ec, rv_enc_i(0, RV_RA, 0, RV_X0, RV_OPCODE_JALR)) != 0)
            return -1;
        break;
    case LR_OP_PHI:
        break;
    default:
        return -1;
    }
    return 0;
}

static int rv_compile_set_block(void *compile_ctx, uint32_t block_id) {
    rv_direct_ctx_t *ctx = (rv_direct_ctx_t *)compile_ctx;
    if (!ctx)
        return -1;
    ctx->current_block_id = block_id;
    ctx->has_current_block = true;
    return 0;
}

static int rv_compile_end(void *compile_ctx, size_t *out_len) {
    rv_direct_ctx_t *ctx = (rv_direct_ctx_t *)compile_ctx;
    if (!ctx || !out_len)
        return -1;
    *out_len = ctx->ec.pos;
    if (ctx->ec.pos > ctx->ec.buflen)
        return -1;
    return 0;
}

static int rv_compile_add_phi_copy(void *compile_ctx,
                                   uint32_t pred_block_id,
                                   uint32_t succ_block_id,
                                   uint32_t dest_vreg,
                                   const lr_operand_desc_t *src_op) {
    (void)compile_ctx;
    (void)pred_block_id;
    (void)succ_block_id;
    (void)dest_vreg;
    (void)src_op;
    return -1;
}

static const rv_features_t rv_feat_gc = {
    .name = "rv64gc",
    .ext_m = true,
    .ext_f = true,
    .ext_d = true,
};
static const rv_features_t rv_feat_im = {
    .name = "rv64im",
    .ext_m = true,
    .ext_f = false,
    .ext_d = false,
};

static int rv_compile_begin_rv64gc(void **compile_ctx,
                                   const lr_compile_func_meta_t *func_meta,
                                   lr_module_t *mod,
                                   uint8_t *buf, size_t buflen,
                                   lr_arena_t *arena) {
    return rv_compile_begin_common(compile_ctx, func_meta, mod, buf, buflen,
                                   arena, &rv_feat_gc);
}

static int rv_compile_begin_rv64im(void **compile_ctx,
                                   const lr_compile_func_meta_t *func_meta,
                                   lr_module_t *mod,
                                   uint8_t *buf, size_t buflen,
                                   lr_arena_t *arena) {
    return rv_compile_begin_common(compile_ctx, func_meta, mod, buf, buflen,
                                   arena, &rv_feat_im);
}

static const lr_target_t target_riscv64gc = {
    .name = "riscv64gc",
    .ptr_size = 8,
    .compile_begin = rv_compile_begin_rv64gc,
    .compile_emit = rv_compile_emit,
    .compile_set_block = rv_compile_set_block,
    .compile_end = rv_compile_end,
    .compile_add_phi_copy = rv_compile_add_phi_copy,
};

static const lr_target_t target_riscv64im = {
    .name = "riscv64im",
    .ptr_size = 8,
    .compile_begin = rv_compile_begin_rv64im,
    .compile_emit = rv_compile_emit,
    .compile_set_block = rv_compile_set_block,
    .compile_end = rv_compile_end,
    .compile_add_phi_copy = rv_compile_add_phi_copy,
};

const lr_target_t *lr_target_riscv64(void) {
    return &target_riscv64gc;
}

const lr_target_t *lr_target_riscv64gc(void) {
    return &target_riscv64gc;
}

const lr_target_t *lr_target_riscv64im(void) {
    return &target_riscv64im;
}
