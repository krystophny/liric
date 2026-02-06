#include "target_aarch64.h"
#include "target_x86_64.h"
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static uint8_t map_reg(uint8_t x86_reg) {
    switch (x86_reg) {
    case X86_RAX: return A64_X0;
    case X86_RCX: return A64_X3;
    case X86_RDX: return A64_X2;
    case X86_RBX: return A64_X19;
    case X86_RSP: return A64_SP;
    case X86_RBP: return A64_FP;
    case X86_RSI: return A64_X1;
    case X86_RDI: return A64_X0;
    case X86_R8:  return A64_X4;
    case X86_R9:  return A64_X5;
    case X86_R10: return A64_X10;
    case X86_R11: return A64_X11;
    case X86_R12: return A64_X12;
    case X86_R13: return A64_X13;
    case X86_R14: return A64_X14;
    case X86_R15: return A64_X15;
    default:      return A64_X9;
    }
}

static uint8_t map_cc(uint8_t x86_cc) {
    switch (x86_cc) {
    case X86_CC_E:  return 0;  /* eq */
    case X86_CC_NE: return 1;  /* ne */
    case X86_CC_AE: return 2;  /* hs */
    case X86_CC_B:  return 3;  /* lo */
    case X86_CC_S:  return 4;  /* mi */
    case X86_CC_NS: return 5;  /* pl */
    case X86_CC_O:  return 6;  /* vs */
    case X86_CC_NO: return 7;  /* vc */
    case X86_CC_A:  return 8;  /* hi */
    case X86_CC_BE: return 9;  /* ls */
    case X86_CC_GE: return 10; /* ge */
    case X86_CC_L:  return 11; /* lt */
    case X86_CC_G:  return 12; /* gt */
    case X86_CC_LE: return 13; /* le */
    default:        return 0;
    }
}

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

static uint32_t enc_shiftv(lr_x86_op_t op, bool is64, uint8_t rd, uint8_t rn,
                           uint8_t rm) {
    uint32_t base;
    switch (op) {
    case LR_X86_SHR: base = is64 ? 0x9AC02400u : 0x1AC02400u; break;
    case LR_X86_SAR: base = is64 ? 0x9AC02800u : 0x1AC02800u; break;
    default:         base = is64 ? 0x9AC02000u : 0x1AC02000u; break;
    }
    return base | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rd;
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
    emit_addr(buf, pos, len, A64_X9, rn, disp);
    emit_u32(buf, pos, len, enc_ldur(size, rt, A64_X9, 0));
}

static void emit_store(uint8_t *buf, size_t *pos, size_t len, uint8_t rt,
                       uint8_t rn, int32_t disp, uint8_t size) {
    if (disp >= -256 && disp <= 255) {
        emit_u32(buf, pos, len, enc_stur(size, rt, rn, disp));
        return;
    }
    emit_addr(buf, pos, len, A64_X9, rn, disp);
    emit_u32(buf, pos, len, enc_stur(size, rt, A64_X9, 0));
}

static void emit_mov_reg(uint8_t *buf, size_t *pos, size_t len, uint8_t rd,
                         uint8_t rm, bool is64) {
    emit_u32(buf, pos, len, enc_logic_reg(0xAA000000u, is64, rd, A64_SP, rm));
}

static int aarch64_isel_func(lr_func_t *func, lr_mfunc_t *mf, lr_module_t *mod) {
    return lr_target_x86_64()->isel_func(func, mf, mod);
}

static int aarch64_encode_func(lr_mfunc_t *mf, uint8_t *buf, size_t buflen,
                               size_t *out_len) {
    size_t pos = 0;
    size_t block_offsets[1024];
    struct fixup_t {
        size_t insn_pos;
        uint32_t target;
        uint8_t kind;
        uint8_t cond;
    } fixups[4096];
    uint32_t nfix = 0;
    uint32_t block_idx = 0;

    emit_u32(buf, &pos, buflen, 0xA9BF7BFDu); /* stp x29, x30, [sp, #-16]! */
    emit_u32(buf, &pos, buflen, 0x910003FDu); /* mov x29, sp */
    if (mf->stack_size > 0)
        emit_sp_adjust(buf, &pos, buflen, mf->stack_size, true);

    for (lr_mblock_t *mb = mf->first_block; mb; mb = mb->next, block_idx++) {
        block_offsets[block_idx] = pos;

        for (lr_minst_t *mi = mb->first; mi; mi = mi->next) {
            bool is64 = mi->size > 4;
            uint8_t dst = map_reg(mi->dst.reg);
            uint8_t src = map_reg(mi->src.reg);

            switch (mi->op) {
            case LR_X86_RET:
                if (mf->stack_size > 0)
                    emit_sp_adjust(buf, &pos, buflen, mf->stack_size, false);
                emit_u32(buf, &pos, buflen, 0xA8C17BFDu); /* ldp x29, x30, [sp], #16 */
                emit_u32(buf, &pos, buflen, 0xD65F03C0u); /* ret */
                break;

            case LR_X86_MOV_IMM:
                emit_move_imm(buf, &pos, buflen, dst, mi->src.imm, is64);
                break;

            case LR_X86_MOV:
                if (mi->src.kind == LR_MOP_MEM && mi->dst.kind == LR_MOP_REG) {
                    emit_load(buf, &pos, buflen, dst, map_reg(mi->src.mem.base),
                              mi->src.mem.disp, mi->size);
                } else if (mi->dst.kind == LR_MOP_MEM && mi->src.kind == LR_MOP_REG) {
                    emit_store(buf, &pos, buflen, src, map_reg(mi->dst.mem.base),
                               mi->dst.mem.disp, mi->size);
                } else if (mi->src.kind == LR_MOP_REG && mi->dst.kind == LR_MOP_REG) {
                    emit_mov_reg(buf, &pos, buflen, dst, src, is64);
                }
                break;

            case LR_X86_ADD:
                emit_u32(buf, &pos, buflen, enc_add_reg(is64, dst, dst, src));
                break;
            case LR_X86_SUB:
                emit_u32(buf, &pos, buflen, enc_sub_reg(is64, dst, dst, src));
                break;
            case LR_X86_AND:
                emit_u32(buf, &pos, buflen,
                         enc_logic_reg(0x8A000000u, is64, dst, dst, src));
                break;
            case LR_X86_OR:
                emit_u32(buf, &pos, buflen,
                         enc_logic_reg(0xAA000000u, is64, dst, dst, src));
                break;
            case LR_X86_XOR:
                emit_u32(buf, &pos, buflen,
                         enc_logic_reg(0xCA000000u, is64, dst, dst, src));
                break;

            case LR_X86_IMUL:
                emit_u32(buf, &pos, buflen, enc_mul(is64, dst, dst, src));
                break;

            case LR_X86_IDIV: {
                uint8_t qreg = map_reg(X86_RAX);
                uint8_t rreg = map_reg(X86_RDX);
                uint8_t treg = A64_X11;
                emit_mov_reg(buf, &pos, buflen, treg, qreg, is64);
                emit_u32(buf, &pos, buflen, enc_sdiv(is64, qreg, qreg, src));
                emit_u32(buf, &pos, buflen, enc_msub(is64, rreg, qreg, src, treg));
                break;
            }

            case LR_X86_CDQ:
            case LR_X86_CQO:
                break;

            case LR_X86_SAL:
            case LR_X86_SAR:
            case LR_X86_SHR:
                emit_u32(buf, &pos, buflen, enc_shiftv(mi->op, is64, dst, dst, src));
                break;

            case LR_X86_CMP:
                emit_u32(buf, &pos, buflen, enc_subs_reg(is64, dst, src));
                break;

            case LR_X86_TEST:
                emit_u32(buf, &pos, buflen, enc_ands_reg(is64, dst, src));
                break;

            case LR_X86_SETCC: {
                uint8_t cond = map_cc(mi->cc);
                emit_move_imm(buf, &pos, buflen, dst, 1, false);
                emit_u32(buf, &pos, buflen, enc_csel(false, dst, dst, A64_SP, cond));
                break;
            }

            case LR_X86_MOVZX:
                if (mi->src.kind == LR_MOP_REG && mi->dst.kind == LR_MOP_REG && dst != src)
                    emit_mov_reg(buf, &pos, buflen, dst, src, false);
                break;

            case LR_X86_MOVSX:
                if (is64)
                    emit_u32(buf, &pos, buflen,
                             0x93407C00u | ((uint32_t)src << 5) | dst); /* sxtw */
                break;

            case LR_X86_CMOVCC: {
                uint8_t cond = map_cc(mi->cc);
                emit_u32(buf, &pos, buflen, enc_csel(is64, dst, src, dst, cond));
                break;
            }

            case LR_X86_JMP:
                if (nfix < 4096) {
                    fixups[nfix].insn_pos = pos;
                    fixups[nfix].target = mi->dst.label;
                    fixups[nfix].kind = 0;
                    fixups[nfix].cond = 0;
                    nfix++;
                }
                emit_u32(buf, &pos, buflen, 0x14000000u);
                break;

            case LR_X86_JCC:
                if (nfix < 4096) {
                    fixups[nfix].insn_pos = pos;
                    fixups[nfix].target = mi->dst.label;
                    fixups[nfix].kind = 1;
                    fixups[nfix].cond = map_cc(mi->cc);
                    nfix++;
                }
                emit_u32(buf, &pos, buflen, 0x54000000u);
                break;

            case LR_X86_LEA:
                emit_addr(buf, &pos, buflen, dst, map_reg(mi->src.mem.base),
                          mi->src.mem.disp);
                break;

            case LR_X86_CALL:
                emit_u32(buf, &pos, buflen, 0xD63F0000u | ((uint32_t)src << 5));
                break;

            case LR_X86_SUB_RSP:
                emit_sp_adjust(buf, &pos, buflen, (uint32_t)mi->src.imm, true);
                break;

            case LR_X86_ADD_RSP:
                emit_sp_adjust(buf, &pos, buflen, (uint32_t)mi->src.imm, false);
                break;

            default:
                break;
            }
        }
    }

    for (uint32_t i = 0; i < nfix; i++) {
        const struct fixup_t *fx = &fixups[i];
        if (fx->target >= block_idx) continue;

        int64_t target_pos = (int64_t)block_offsets[fx->target];
        int64_t here = (int64_t)fx->insn_pos;
        int64_t imm = (target_pos - (here + 4)) / 4;

        if (fx->kind == 0) {
            if (imm >= -(1LL << 25) && imm < (1LL << 25)) {
                uint32_t insn = 0x14000000u | ((uint32_t)imm & 0x03FFFFFFu);
                patch_u32(buf, buflen, fx->insn_pos, insn);
            }
        } else {
            if (imm >= -(1LL << 18) && imm < (1LL << 18)) {
                uint32_t insn = 0x54000000u | (((uint32_t)imm & 0x7FFFFu) << 5)
                              | (fx->cond & 0xF);
                patch_u32(buf, buflen, fx->insn_pos, insn);
            }
        }
    }

    *out_len = pos;
    return 0;
}

static int aarch64_print_inst(const lr_minst_t *mi, char *buf, size_t len) {
    (void)mi;
    return snprintf(buf, len, "aarch64-op");
}

static const lr_target_t aarch64_target = {
    .name = "aarch64",
    .ptr_size = 8,
    .isel_func = aarch64_isel_func,
    .encode_func = aarch64_encode_func,
    .print_inst = aarch64_print_inst,
};

const lr_target_t *lr_target_aarch64(void) {
    return &aarch64_target;
}
