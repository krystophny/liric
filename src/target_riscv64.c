#include "target_riscv64.h"

#include "ir.h"

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
#define RV_OPCODE_JALR   0x67u
#define RV_OPCODE_OPFP   0x53u

#define RV_FUNCT3_ADD_SUB 0x0u
#define RV_FUNCT3_AND      0x7u
#define RV_FUNCT3_OR       0x6u
#define RV_FUNCT3_XOR      0x4u
#define RV_FUNCT3_SLL      0x1u
#define RV_FUNCT3_SRL_SRA  0x5u
#define RV_FUNCT3_DIV      0x4u
#define RV_FUNCT3_REM      0x6u

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

static int rv_compile_func_with_features(lr_func_t *func, lr_module_t *mod,
                                         uint8_t *buf, size_t buflen, size_t *out_len,
                                         lr_arena_t *arena,
                                         const rv_features_t *feat) {
    (void)mod;
    (void)arena;

    if (!func || !buf || !out_len || !feat)
        return -1;
    if (func->is_decl)
        return -1;

    rv_emit_ctx_t ec = {.buf = buf, .buflen = buflen, .pos = 0};

    uint32_t vmap_n = func->next_vreg + 1u;
    rv_vreg_map_t *vmap = (rv_vreg_map_t *)calloc(vmap_n, sizeof(*vmap));
    if (!vmap)
        return -1;

    uint8_t next_iarg = RV_A0;
    uint8_t next_farg = RV_FA0;
    for (uint32_t i = 0; i < func->num_params; i++) {
        uint32_t v = func->param_vregs[i];
        lr_type_t *pt = func->param_types ? func->param_types[i] : NULL;
        if (v >= vmap_n) {
            free(vmap);
            return -1;
        }
        if (rv_type_is_fp(pt)) {
            if (pt->kind == LR_TYPE_DOUBLE && !feat->ext_d) {
                free(vmap);
                return -1;
            }
            if (pt->kind == LR_TYPE_FLOAT && !feat->ext_f) {
                free(vmap);
                return -1;
            }
            if (next_farg > RV_FA7) {
                free(vmap);
                return -1;
            }
            vmap[v].in_use = 1;
            vmap[v].cls = RV_REGCLS_FPR;
            vmap[v].reg = next_farg++;
        } else {
            if (!rv_type_is_intlike(pt)) {
                free(vmap);
                return -1;
            }
            if (next_iarg > RV_A7) {
                free(vmap);
                return -1;
            }
            vmap[v].in_use = 1;
            vmap[v].cls = RV_REGCLS_GPR;
            vmap[v].reg = next_iarg++;
        }
    }

    static const uint8_t gpr_tmp_pool[] = {
        RV_T3, RV_T4, RV_T5, RV_T6,
        RV_S2, RV_S3, RV_S4, RV_S5, RV_S6, RV_S7, RV_S8, RV_S9, RV_S10, RV_S11,
    };
    static const uint8_t fpr_tmp_pool[] = {
        RV_FT0, RV_FT1, RV_FT2, RV_FT3, RV_FT4, RV_FT5, RV_FT6, RV_FT7,
        RV_FT8, RV_FT9, RV_FT10, RV_FT11, RV_FS2, RV_FS3, RV_FS4, RV_FS5,
    };
    size_t gpr_next = 0;
    size_t fpr_next = 0;

    if (!func->first_block) {
        free(vmap);
        return -1;
    }

    for (lr_block_t *b = func->first_block; b; b = b->next) {
        for (lr_inst_t *inst = b->first; inst; inst = inst->next) {
            switch (inst->op) {
            case LR_OP_ADD:
            case LR_OP_SUB:
            case LR_OP_MUL:
            case LR_OP_SDIV:
            case LR_OP_SREM:
            case LR_OP_AND:
            case LR_OP_OR:
            case LR_OP_XOR:
            case LR_OP_SHL:
            case LR_OP_LSHR:
            case LR_OP_ASHR: {
                if (!rv_type_is_intlike(inst->type) || inst->num_operands != 2 || inst->dest >= vmap_n) {
                    free(vmap);
                    return -1;
                }
                if ((inst->op == LR_OP_MUL || inst->op == LR_OP_SDIV || inst->op == LR_OP_SREM) && !feat->ext_m) {
                    free(vmap);
                    return -1;
                }
                if (gpr_next >= sizeof(gpr_tmp_pool)) {
                    free(vmap);
                    return -1;
                }

                uint8_t rd = gpr_tmp_pool[gpr_next++];
                uint8_t rs1 = 0;
                uint8_t rs2 = 0;
                if (rv_operand_gpr(&ec, &inst->operands[0], vmap, vmap_n, RV_T1, RV_T0, &rs1) != 0 ||
                    rv_operand_gpr(&ec, &inst->operands[1], vmap, vmap_n, RV_T2, RV_T0, &rs2) != 0) {
                    free(vmap);
                    return -1;
                }

                uint32_t enc = 0;
                switch (inst->op) {
                case LR_OP_ADD: enc = rv_enc_r(RV_FUNCT7_ADD, rs2, rs1, RV_FUNCT3_ADD_SUB, rd, RV_OPCODE_OP); break;
                case LR_OP_SUB: enc = rv_enc_r(RV_FUNCT7_SUB, rs2, rs1, RV_FUNCT3_ADD_SUB, rd, RV_OPCODE_OP); break;
                case LR_OP_MUL: enc = rv_enc_r(RV_FUNCT7_MULDIV, rs2, rs1, RV_FUNCT3_ADD_SUB, rd, RV_OPCODE_OP); break;
                case LR_OP_SDIV: enc = rv_enc_r(RV_FUNCT7_MULDIV, rs2, rs1, RV_FUNCT3_DIV, rd, RV_OPCODE_OP); break;
                case LR_OP_SREM: enc = rv_enc_r(RV_FUNCT7_MULDIV, rs2, rs1, RV_FUNCT3_REM, rd, RV_OPCODE_OP); break;
                case LR_OP_AND: enc = rv_enc_r(RV_FUNCT7_ADD, rs2, rs1, RV_FUNCT3_AND, rd, RV_OPCODE_OP); break;
                case LR_OP_OR: enc = rv_enc_r(RV_FUNCT7_ADD, rs2, rs1, RV_FUNCT3_OR, rd, RV_OPCODE_OP); break;
                case LR_OP_XOR: enc = rv_enc_r(RV_FUNCT7_ADD, rs2, rs1, RV_FUNCT3_XOR, rd, RV_OPCODE_OP); break;
                case LR_OP_SHL: enc = rv_enc_r(RV_FUNCT7_ADD, rs2, rs1, RV_FUNCT3_SLL, rd, RV_OPCODE_OP); break;
                case LR_OP_LSHR: enc = rv_enc_r(RV_FUNCT7_SRL, rs2, rs1, RV_FUNCT3_SRL_SRA, rd, RV_OPCODE_OP); break;
                case LR_OP_ASHR: enc = rv_enc_r(RV_FUNCT7_SRA, rs2, rs1, RV_FUNCT3_SRL_SRA, rd, RV_OPCODE_OP); break;
                default: free(vmap); return -1;
                }
                if (rv_emit32(&ec, enc) != 0) {
                    free(vmap);
                    return -1;
                }
                vmap[inst->dest].in_use = 1;
                vmap[inst->dest].cls = RV_REGCLS_GPR;
                vmap[inst->dest].reg = rd;
                break;
            }

            case LR_OP_FADD:
            case LR_OP_FSUB:
            case LR_OP_FMUL:
            case LR_OP_FDIV:
            case LR_OP_FNEG: {
                bool is_double = inst->type && inst->type->kind == LR_TYPE_DOUBLE;
                bool is_float = inst->type && inst->type->kind == LR_TYPE_FLOAT;
                if ((!is_float && !is_double) || inst->dest >= vmap_n) {
                    free(vmap);
                    return -1;
                }
                if ((is_double && !feat->ext_d) || (is_float && !feat->ext_f)) {
                    free(vmap);
                    return -1;
                }
                if (fpr_next >= sizeof(fpr_tmp_pool)) {
                    free(vmap);
                    return -1;
                }

                uint8_t rd = fpr_tmp_pool[fpr_next++];
                uint8_t rs1 = 0;
                uint8_t rs2 = 0;
                uint8_t funct7_base = 0;
                uint8_t funct7 = 0;
                uint8_t rm = 0;

                if (inst->op == LR_OP_FNEG) {
                    if (inst->num_operands != 1 ||
                        rv_operand_fpr(&ec, &inst->operands[0], vmap, vmap_n,
                                       RV_T1, RV_T2, RV_FT0, feat, &rs1) != 0) {
                        free(vmap);
                        return -1;
                    }
                    funct7 = is_double ? 0x11u : 0x10u;
                    rm = 0x1u;
                    rs2 = rs1;
                } else {
                    if (inst->num_operands != 2 ||
                        rv_operand_fpr(&ec, &inst->operands[0], vmap, vmap_n,
                                       RV_T1, RV_T2, RV_FT0, feat, &rs1) != 0 ||
                        rv_operand_fpr(&ec, &inst->operands[1], vmap, vmap_n,
                                       RV_T1, RV_T2, RV_FT1, feat, &rs2) != 0) {
                        free(vmap);
                        return -1;
                    }
                    switch (inst->op) {
                    case LR_OP_FADD: funct7_base = 0x00u; break;
                    case LR_OP_FSUB: funct7_base = 0x04u; break;
                    case LR_OP_FMUL: funct7_base = 0x08u; break;
                    case LR_OP_FDIV: funct7_base = 0x0Cu; break;
                    default: free(vmap); return -1;
                    }
                    funct7 = funct7_base + (is_double ? 1u : 0u);
                    rm = 0u;
                }

                if (rv_emit32(&ec, rv_enc_r(funct7, rs2, rs1, rm, rd, RV_OPCODE_OPFP)) != 0) {
                    free(vmap);
                    return -1;
                }
                vmap[inst->dest].in_use = 1;
                vmap[inst->dest].cls = RV_REGCLS_FPR;
                vmap[inst->dest].reg = rd;
                break;
            }

            case LR_OP_SITOFP: {
                bool is_double = inst->type && inst->type->kind == LR_TYPE_DOUBLE;
                bool is_float = inst->type && inst->type->kind == LR_TYPE_FLOAT;
                if ((!is_float && !is_double) || inst->num_operands != 1 || inst->dest >= vmap_n) {
                    free(vmap);
                    return -1;
                }
                if ((is_double && !feat->ext_d) || (is_float && !feat->ext_f) || fpr_next >= sizeof(fpr_tmp_pool)) {
                    free(vmap);
                    return -1;
                }
                uint8_t rs1 = 0;
                if (rv_operand_gpr(&ec, &inst->operands[0], vmap, vmap_n, RV_T1, RV_T2, &rs1) != 0) {
                    free(vmap);
                    return -1;
                }
                uint8_t rd = fpr_tmp_pool[fpr_next++];
                uint8_t funct7 = is_double ? 0x69u : 0x68u;
                if (rv_emit32(&ec, rv_enc_r(funct7, 0x2u, rs1, 0x0u, rd, RV_OPCODE_OPFP)) != 0) {
                    free(vmap);
                    return -1;
                }
                vmap[inst->dest].in_use = 1;
                vmap[inst->dest].cls = RV_REGCLS_FPR;
                vmap[inst->dest].reg = rd;
                break;
            }

            case LR_OP_FPTOSI: {
                if (!rv_type_is_intlike(inst->type) || inst->num_operands != 1 || inst->dest >= vmap_n || gpr_next >= sizeof(gpr_tmp_pool)) {
                    free(vmap);
                    return -1;
                }
                const lr_operand_t *src = &inst->operands[0];
                bool src_double = src->type && src->type->kind == LR_TYPE_DOUBLE;
                bool src_float = src->type && src->type->kind == LR_TYPE_FLOAT;
                if ((!src_float && !src_double) || (src_double && !feat->ext_d) || (src_float && !feat->ext_f)) {
                    free(vmap);
                    return -1;
                }
                uint8_t frs = 0;
                if (rv_operand_fpr(&ec, src, vmap, vmap_n, RV_T1, RV_T2, RV_FT0, feat, &frs) != 0) {
                    free(vmap);
                    return -1;
                }
                uint8_t rd = gpr_tmp_pool[gpr_next++];
                uint8_t funct7 = src_double ? 0x61u : 0x60u;
                if (rv_emit32(&ec, rv_enc_r(funct7, 0x2u, frs, 0x1u, rd, RV_OPCODE_OPFP)) != 0) {
                    free(vmap);
                    return -1;
                }
                vmap[inst->dest].in_use = 1;
                vmap[inst->dest].cls = RV_REGCLS_GPR;
                vmap[inst->dest].reg = rd;
                break;
            }

            case LR_OP_FPEXT:
            case LR_OP_FPTRUNC: {
                bool to_double = inst->op == LR_OP_FPEXT;
                bool to_float = inst->op == LR_OP_FPTRUNC;
                if (inst->num_operands != 1 || inst->dest >= vmap_n || fpr_next >= sizeof(fpr_tmp_pool) || !feat->ext_d) {
                    free(vmap);
                    return -1;
                }
                const lr_operand_t *src = &inst->operands[0];
                if (!src->type || !inst->type) {
                    free(vmap);
                    return -1;
                }
                if (to_double && !(src->type->kind == LR_TYPE_FLOAT && inst->type->kind == LR_TYPE_DOUBLE)) {
                    free(vmap);
                    return -1;
                }
                if (to_float && !(src->type->kind == LR_TYPE_DOUBLE && inst->type->kind == LR_TYPE_FLOAT)) {
                    free(vmap);
                    return -1;
                }
                uint8_t frs = 0;
                if (rv_operand_fpr(&ec, src, vmap, vmap_n, RV_T1, RV_T2, RV_FT0, feat, &frs) != 0) {
                    free(vmap);
                    return -1;
                }
                uint8_t rd = fpr_tmp_pool[fpr_next++];
                uint8_t funct7 = to_double ? 0x21u : 0x20u;
                uint8_t rs2 = to_double ? 0u : 1u;
                if (rv_emit32(&ec, rv_enc_r(funct7, rs2, frs, 0x0u, rd, RV_OPCODE_OPFP)) != 0) {
                    free(vmap);
                    return -1;
                }
                vmap[inst->dest].in_use = 1;
                vmap[inst->dest].cls = RV_REGCLS_FPR;
                vmap[inst->dest].reg = rd;
                break;
            }

            case LR_OP_TRUNC:
            case LR_OP_ZEXT:
            case LR_OP_SEXT:
            case LR_OP_BITCAST: {
                if (inst->num_operands != 1 || inst->dest >= vmap_n) {
                    free(vmap);
                    return -1;
                }
                const lr_operand_t *src = &inst->operands[0];
                if (rv_type_is_intlike(inst->type) && rv_type_is_intlike(src->type)) {
                    uint8_t rs = 0;
                    if (rv_operand_gpr(&ec, src, vmap, vmap_n, RV_T1, RV_T2, &rs) != 0) {
                        free(vmap);
                        return -1;
                    }
                    if (gpr_next >= sizeof(gpr_tmp_pool)) {
                        free(vmap);
                        return -1;
                    }
                    uint8_t rd = gpr_tmp_pool[gpr_next++];
                    if (rv_emit_mv(&ec, rd, rs) != 0) {
                        free(vmap);
                        return -1;
                    }
                    vmap[inst->dest].in_use = 1;
                    vmap[inst->dest].cls = RV_REGCLS_GPR;
                    vmap[inst->dest].reg = rd;
                    break;
                }
                free(vmap);
                return -1;
            }

            case LR_OP_RET: {
                if (inst->num_operands != 1) {
                    free(vmap);
                    return -1;
                }
                const lr_operand_t *rop = &inst->operands[0];
                if (rv_type_is_fp(rop->type)) {
                    uint8_t src = 0;
                    if (rv_operand_fpr(&ec, rop, vmap, vmap_n, RV_T1, RV_T2, RV_FT0, feat, &src) != 0)
                        { free(vmap); return -1; }
                    bool is_double = rop->type->kind == LR_TYPE_DOUBLE;
                    uint8_t ret_reg = RV_FA0;
                    if (src != ret_reg && rv_emit_fp_move(&ec, ret_reg, src, is_double) != 0)
                        { free(vmap); return -1; }
                } else {
                    uint8_t src = 0;
                    if (rv_operand_gpr(&ec, rop, vmap, vmap_n, RV_T1, RV_T2, &src) != 0)
                        { free(vmap); return -1; }
                    if (src != RV_A0 && rv_emit_mv(&ec, RV_A0, src) != 0)
                        { free(vmap); return -1; }
                }
                if (rv_emit32(&ec, rv_enc_i(0, RV_RA, 0, RV_X0, RV_OPCODE_JALR)) != 0) {
                    free(vmap);
                    return -1;
                }
                *out_len = ec.pos;
                free(vmap);
                return 0;
            }

            case LR_OP_RET_VOID:
                if (rv_emit32(&ec, rv_enc_i(0, RV_RA, 0, RV_X0, RV_OPCODE_JALR)) != 0) {
                    free(vmap);
                    return -1;
                }
                *out_len = ec.pos;
                free(vmap);
                return 0;

            default:
                free(vmap);
                return -1;
            }
        }
    }

    free(vmap);
    return -1;
}

static int rv_compile_func_rv64im(lr_func_t *func, lr_module_t *mod,
                                  uint8_t *buf, size_t buflen, size_t *out_len,
                                  lr_arena_t *arena) {
    static const rv_features_t feat = {
        .name = "rv64im",
        .ext_m = true,
        .ext_f = false,
        .ext_d = false,
    };
    return rv_compile_func_with_features(func, mod, buf, buflen, out_len, arena, &feat);
}

static int rv_compile_func_rv64gc(lr_func_t *func, lr_module_t *mod,
                                  uint8_t *buf, size_t buflen, size_t *out_len,
                                  lr_arena_t *arena) {
    static const rv_features_t feat = {
        .name = "rv64gc",
        .ext_m = true,
        .ext_f = true,
        .ext_d = true,
    };
    return rv_compile_func_with_features(func, mod, buf, buflen, out_len, arena, &feat);
}

static int rv_compile_func_cp_rv64im(lr_func_t *func, lr_module_t *mod,
                                     uint8_t *buf, size_t buflen, size_t *out_len,
                                     lr_arena_t *arena) {
    return rv_compile_func_rv64im(func, mod, buf, buflen, out_len, arena);
}

static int rv_compile_func_cp_rv64gc(lr_func_t *func, lr_module_t *mod,
                                     uint8_t *buf, size_t buflen, size_t *out_len,
                                     lr_arena_t *arena) {
    return rv_compile_func_rv64gc(func, mod, buf, buflen, out_len, arena);
}

typedef int (*rv_compile_entry_t)(lr_func_t *func, lr_module_t *mod,
                                  uint8_t *buf, size_t buflen, size_t *out_len,
                                  lr_arena_t *arena);

typedef struct rv_stream_block_builder {
    lr_block_t block;
    lr_inst_t **inst_array;
    uint32_t inst_cap;
} rv_stream_block_builder_t;

typedef struct rv_stream_bridge_ctx {
    lr_func_t *func;
    lr_func_t stream_func;
    uint32_t *owned_param_vregs;
    rv_stream_block_builder_t **blocks;
    uint32_t block_cap;
    uint32_t max_block_id_plus1;
    uint32_t current_block_id;
    bool has_current_block;
    bool saw_stream_input;
    lr_module_t *mod;
    uint8_t *buf;
    size_t buflen;
    lr_arena_t *arena;
    lr_compile_mode_t mode;
    rv_compile_entry_t isel_entry;
    rv_compile_entry_t cp_entry;
} rv_stream_bridge_ctx_t;

static lr_operand_t rv_stream_operand_from_desc(const lr_operand_desc_t *desc) {
    lr_operand_t out;
    memset(&out, 0, sizeof(out));
    if (!desc)
        return out;
    out.type = desc->type;
    out.global_offset = desc->global_offset;
    switch (desc->kind) {
    case LR_OP_KIND_VREG:
        out.kind = LR_VAL_VREG;
        out.vreg = desc->vreg;
        break;
    case LR_OP_KIND_IMM_I64:
        out.kind = LR_VAL_IMM_I64;
        out.imm_i64 = desc->imm_i64;
        break;
    case LR_OP_KIND_IMM_F64:
        out.kind = LR_VAL_IMM_F64;
        out.imm_f64 = desc->imm_f64;
        break;
    case LR_OP_KIND_BLOCK:
        out.kind = LR_VAL_BLOCK;
        out.block_id = desc->block_id;
        break;
    case LR_OP_KIND_GLOBAL:
        out.kind = LR_VAL_GLOBAL;
        out.global_id = desc->global_id;
        break;
    case LR_OP_KIND_NULL:
        out.kind = LR_VAL_NULL;
        break;
    case LR_OP_KIND_UNDEF:
        out.kind = LR_VAL_UNDEF;
        break;
    default:
        break;
    }
    return out;
}

static int rv_stream_ensure_block_capacity(rv_stream_bridge_ctx_t *ctx,
                                           uint32_t need) {
    rv_stream_block_builder_t **new_blocks = NULL;
    uint32_t new_cap;
    if (!ctx)
        return -1;
    if (need <= ctx->block_cap)
        return 0;
    new_cap = ctx->block_cap == 0 ? 8u : ctx->block_cap;
    while (new_cap < need)
        new_cap *= 2u;
    new_blocks = lr_arena_array(ctx->arena, rv_stream_block_builder_t *, new_cap);
    if (!new_blocks)
        return -1;
    if (ctx->block_cap > 0)
        memcpy(new_blocks, ctx->blocks, sizeof(*new_blocks) * ctx->block_cap);
    ctx->blocks = new_blocks;
    ctx->block_cap = new_cap;
    return 0;
}

static rv_stream_block_builder_t *rv_stream_get_or_create_block(
        rv_stream_bridge_ctx_t *ctx, uint32_t block_id) {
    rv_stream_block_builder_t *b = NULL;
    char name_buf[32];
    if (!ctx)
        return NULL;
    if (rv_stream_ensure_block_capacity(ctx, block_id + 1u) != 0)
        return NULL;
    b = ctx->blocks[block_id];
    if (b)
        return b;
    b = lr_arena_new(ctx->arena, rv_stream_block_builder_t);
    if (!b)
        return NULL;
    (void)snprintf(name_buf, sizeof(name_buf), "b%u", block_id);
    b->block.id = block_id;
    b->block.name = lr_arena_strdup(ctx->arena, name_buf, strlen(name_buf));
    b->block.func = &ctx->stream_func;
    if (!b->block.name)
        return NULL;
    ctx->blocks[block_id] = b;
    if (ctx->max_block_id_plus1 < block_id + 1u)
        ctx->max_block_id_plus1 = block_id + 1u;
    return b;
}

static int rv_stream_push_inst_ref(rv_stream_bridge_ctx_t *ctx,
                                   rv_stream_block_builder_t *block,
                                   lr_inst_t *inst) {
    lr_inst_t **new_refs = NULL;
    uint32_t new_cap;
    if (!ctx || !block || !inst)
        return -1;
    if (block->block.num_insts == block->inst_cap) {
        new_cap = block->inst_cap == 0 ? 16u : block->inst_cap * 2u;
        new_refs = lr_arena_array(ctx->arena, lr_inst_t *, new_cap);
        if (!new_refs)
            return -1;
        if (block->inst_cap > 0)
            memcpy(new_refs, block->inst_array, sizeof(*new_refs) * block->inst_cap);
        block->inst_array = new_refs;
        block->inst_cap = new_cap;
    }
    block->inst_array[block->block.num_insts++] = inst;
    return 0;
}

static void rv_stream_note_vregs(rv_stream_bridge_ctx_t *ctx,
                                 const lr_compile_inst_desc_t *inst_desc) {
    if (!ctx || !inst_desc)
        return;
    if (inst_desc->dest >= ctx->stream_func.next_vreg &&
        inst_desc->dest != 0) {
        ctx->stream_func.next_vreg = inst_desc->dest + 1u;
    }
    for (uint32_t i = 0; i < inst_desc->num_operands; i++) {
        const lr_operand_desc_t *op = &inst_desc->operands[i];
        if (op->kind != LR_OP_KIND_VREG)
            continue;
        if (op->vreg >= ctx->stream_func.next_vreg)
            ctx->stream_func.next_vreg = op->vreg + 1u;
    }
}

static int rv_stream_finalize_function(rv_stream_bridge_ctx_t *ctx) {
    lr_block_t **block_array = NULL;
    if (!ctx || !ctx->saw_stream_input || ctx->max_block_id_plus1 == 0)
        return -1;

    block_array = lr_arena_array(ctx->arena, lr_block_t *, ctx->max_block_id_plus1);
    if (!block_array)
        return -1;

    for (uint32_t i = 0; i < ctx->max_block_id_plus1; i++) {
        rv_stream_block_builder_t *b = ctx->blocks ? ctx->blocks[i] : NULL;
        if (!b)
            return -1;
        b->block.inst_array = b->inst_array;
        b->block.func = &ctx->stream_func;
        if (i + 1u < ctx->max_block_id_plus1) {
            rv_stream_block_builder_t *next_b = ctx->blocks[i + 1u];
            if (!next_b)
                return -1;
            b->block.next = &next_b->block;
        } else {
            b->block.next = NULL;
        }
        block_array[i] = &b->block;
    }

    ctx->stream_func.is_decl = false;
    ctx->stream_func.num_blocks = ctx->max_block_id_plus1;
    ctx->stream_func.first_block = &ctx->blocks[0]->block;
    ctx->stream_func.last_block = &ctx->blocks[ctx->max_block_id_plus1 - 1u]->block;
    ctx->stream_func.block_array = block_array;

    return lr_func_finalize(&ctx->stream_func, ctx->arena);
}

static int rv_compile_begin_common(void **compile_ctx,
                                   const lr_compile_func_meta_t *func_meta,
                                   lr_module_t *mod,
                                   uint8_t *buf, size_t buflen,
                                   lr_arena_t *arena,
                                   rv_compile_entry_t isel_entry,
                                   rv_compile_entry_t cp_entry) {
    rv_stream_bridge_ctx_t *ctx = NULL;
    if (!compile_ctx || !func_meta || !mod || !arena ||
        !isel_entry || !cp_entry)
        return -1;
    ctx = lr_arena_new(arena, rv_stream_bridge_ctx_t);
    if (!ctx)
        return -1;
    ctx->func = func_meta->func;
    ctx->mod = mod;
    ctx->buf = buf;
    ctx->buflen = buflen;
    ctx->arena = arena;
    ctx->mode = func_meta->mode;
    ctx->isel_entry = isel_entry;
    ctx->cp_entry = cp_entry;
    if (func_meta->func && func_meta->func->name) {
        ctx->stream_func.name = func_meta->func->name;
    } else {
        ctx->stream_func.name = lr_arena_strdup(ctx->arena, "__liric_stream_fn",
                                                strlen("__liric_stream_fn"));
        if (!ctx->stream_func.name)
            return -1;
    }
    ctx->stream_func.ret_type = func_meta->ret_type ? func_meta->ret_type
                                                    : mod->type_void;
    ctx->stream_func.param_types = func_meta->param_types;
    ctx->stream_func.num_params = func_meta->num_params;
    ctx->stream_func.vararg = func_meta->vararg;
    ctx->stream_func.next_vreg = func_meta->next_vreg;
    if (func_meta->func && func_meta->func->param_vregs) {
        ctx->stream_func.param_vregs = func_meta->func->param_vregs;
    } else if (func_meta->num_params > 0) {
        ctx->owned_param_vregs = lr_arena_array(ctx->arena, uint32_t,
                                                func_meta->num_params);
        if (!ctx->owned_param_vregs)
            return -1;
        for (uint32_t i = 0; i < func_meta->num_params; i++) {
            ctx->owned_param_vregs[i] = i + 1u;
            if (ctx->stream_func.next_vreg <= i + 1u)
                ctx->stream_func.next_vreg = i + 2u;
        }
        ctx->stream_func.param_vregs = ctx->owned_param_vregs;
    }
    *compile_ctx = ctx;
    return 0;
}

static int rv_compile_emit(void *compile_ctx,
                           const lr_compile_inst_desc_t *inst_desc) {
    rv_stream_bridge_ctx_t *ctx = (rv_stream_bridge_ctx_t *)compile_ctx;
    rv_stream_block_builder_t *block = NULL;
    lr_inst_t *inst = NULL;

    if (!ctx || !inst_desc || !ctx->has_current_block)
        return -1;
    if (inst_desc->num_operands > 0 && !inst_desc->operands)
        return -1;
    if (inst_desc->num_indices > 0 && !inst_desc->indices)
        return -1;

    block = rv_stream_get_or_create_block(ctx, ctx->current_block_id);
    if (!block)
        return -1;

    inst = lr_arena_new(ctx->arena, lr_inst_t);
    if (!inst)
        return -1;
    inst->op = inst_desc->op;
    inst->type = inst_desc->type;
    inst->dest = inst_desc->dest;
    inst->num_operands = inst_desc->num_operands;
    inst->num_indices = inst_desc->num_indices;
    inst->icmp_pred = (lr_icmp_pred_t)inst_desc->icmp_pred;
    inst->fcmp_pred = (lr_fcmp_pred_t)inst_desc->fcmp_pred;
    inst->call_external_abi = inst_desc->call_external_abi;
    inst->call_vararg = inst_desc->call_vararg;
    inst->call_fixed_args = inst_desc->call_fixed_args;

    if (inst_desc->num_operands > 0) {
        inst->operands = lr_arena_array(ctx->arena, lr_operand_t,
                                        inst_desc->num_operands);
        if (!inst->operands)
            return -1;
        for (uint32_t i = 0; i < inst_desc->num_operands; i++)
            inst->operands[i] = rv_stream_operand_from_desc(&inst_desc->operands[i]);
    }
    if (inst_desc->num_indices > 0) {
        inst->indices = lr_arena_array(ctx->arena, uint32_t,
                                       inst_desc->num_indices);
        if (!inst->indices)
            return -1;
        memcpy(inst->indices, inst_desc->indices,
               sizeof(*inst->indices) * inst_desc->num_indices);
    }

    if (!block->block.first) {
        block->block.first = inst;
        block->block.last = inst;
    } else {
        block->block.last->next = inst;
        block->block.last = inst;
    }
    if (rv_stream_push_inst_ref(ctx, block, inst) != 0)
        return -1;
    rv_stream_note_vregs(ctx, inst_desc);
    ctx->saw_stream_input = true;
    return 0;
}

static int rv_compile_set_block(void *compile_ctx, uint32_t block_id) {
    rv_stream_bridge_ctx_t *ctx = (rv_stream_bridge_ctx_t *)compile_ctx;
    if (!ctx)
        return -1;
    if (!rv_stream_get_or_create_block(ctx, block_id))
        return -1;
    ctx->current_block_id = block_id;
    ctx->has_current_block = true;
    return 0;
}

static int rv_compile_end(void *compile_ctx, size_t *out_len) {
    rv_stream_bridge_ctx_t *ctx = (rv_stream_bridge_ctx_t *)compile_ctx;
    lr_func_t *func_to_compile = NULL;
    if (!ctx || !out_len)
        return -1;
    if (ctx->saw_stream_input) {
        if (rv_stream_finalize_function(ctx) != 0)
            return -1;
        func_to_compile = &ctx->stream_func;
    } else {
        func_to_compile = ctx->func;
    }
    if (!func_to_compile)
        return -1;
    if (ctx->mode == LR_COMPILE_COPY_PATCH)
        return ctx->cp_entry(func_to_compile, ctx->mod, ctx->buf, ctx->buflen,
                             out_len, ctx->arena);
    if (ctx->mode == LR_COMPILE_ISEL)
        return ctx->isel_entry(func_to_compile, ctx->mod, ctx->buf, ctx->buflen,
                               out_len, ctx->arena);
    return -1;
}

static int rv_compile_begin_rv64gc(void **compile_ctx,
                                   const lr_compile_func_meta_t *func_meta,
                                   lr_module_t *mod,
                                   uint8_t *buf, size_t buflen,
                                   lr_arena_t *arena) {
    return rv_compile_begin_common(compile_ctx, func_meta, mod, buf, buflen,
                                   arena, rv_compile_func_rv64gc,
                                   rv_compile_func_cp_rv64gc);
}

static int rv_compile_begin_rv64im(void **compile_ctx,
                                   const lr_compile_func_meta_t *func_meta,
                                   lr_module_t *mod,
                                   uint8_t *buf, size_t buflen,
                                   lr_arena_t *arena) {
    return rv_compile_begin_common(compile_ctx, func_meta, mod, buf, buflen,
                                   arena, rv_compile_func_rv64im,
                                   rv_compile_func_cp_rv64im);
}

static const lr_target_t target_riscv64gc = {
    .name = "riscv64gc",
    .ptr_size = 8,
    .compile_begin = rv_compile_begin_rv64gc,
    .compile_emit = rv_compile_emit,
    .compile_set_block = rv_compile_set_block,
    .compile_end = rv_compile_end,
};

static const lr_target_t target_riscv64im = {
    .name = "riscv64im",
    .ptr_size = 8,
    .compile_begin = rv_compile_begin_rv64im,
    .compile_emit = rv_compile_emit,
    .compile_set_block = rv_compile_set_block,
    .compile_end = rv_compile_end,
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
