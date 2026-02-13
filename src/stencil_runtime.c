#include "stencil_runtime.h"

#include <stddef.h>
#include <string.h>

typedef struct lr_stencil_lookup_entry {
    lr_opcode_t op;
    lr_type_kind_t type_kind;
    const char *name;
} lr_stencil_lookup_entry_t;

static const lr_stencil_lookup_entry_t g_stencil_lookup[] = {
    { LR_OP_ADD,  LR_TYPE_I32,    "add_i32"  },
    { LR_OP_SUB,  LR_TYPE_I64,    "sub_i64"  },
    { LR_OP_FADD, LR_TYPE_DOUBLE, "fadd_f64" },
};

static uint64_t stencil_patch_value(const lr_stencil_emit_args_t *args, lr_stencil_hole_t hole) {
    switch (hole) {
    case LR_STENCIL_HOLE_SRC0_OFF:
        return (uint64_t)(int64_t)args->src0_off;
    case LR_STENCIL_HOLE_SRC1_OFF:
        return (uint64_t)(int64_t)args->src1_off;
    case LR_STENCIL_HOLE_DST_OFF:
        return (uint64_t)(int64_t)args->dst_off;
    case LR_STENCIL_HOLE_IMM64:
        return (uint64_t)args->imm64;
    case LR_STENCIL_HOLE_BRANCH_REL:
        return (uint64_t)(int64_t)args->branch_rel;
    case LR_STENCIL_HOLE_FUNC_ADDR:
        return (uint64_t)args->func_addr;
    case LR_STENCIL_HOLE_GLOBAL_ADDR:
        return (uint64_t)args->global_addr;
    default:
        return 0;
    }
}

const lr_stencil_t *lr_stencil_lookup_for_ir(lr_opcode_t op, lr_type_kind_t type_kind) {
    size_t i;
    for (i = 0; i < sizeof(g_stencil_lookup) / sizeof(g_stencil_lookup[0]); i++) {
        if (g_stencil_lookup[i].op == op && g_stencil_lookup[i].type_kind == type_kind) {
            return lr_stencil_lookup_generated(g_stencil_lookup[i].name);
        }
    }
    return NULL;
}

int lr_stencil_emit(uint8_t **code_ptr, uint8_t *code_end,
                    const lr_stencil_t *st, const lr_stencil_emit_args_t *args,
                    bool strip_trailing_ret) {
    lr_stencil_emit_args_t empty_args = {0};
    uint8_t *dst;
    size_t i;
    size_t emit_size;

    if (!code_ptr || !*code_ptr || !code_end || !st || !st->bytes) {
        return -1;
    }
    if (*code_ptr > code_end) {
        return -1;
    }
    if (!args) {
        args = &empty_args;
    }

    emit_size = st->size;
    if (strip_trailing_ret && emit_size > 0 && st->bytes[emit_size - 1] == 0xC3) {
        emit_size--;
    }
    if ((size_t)(code_end - *code_ptr) < emit_size) {
        return -1;
    }

    for (i = 0; i < st->n_relocs; i++) {
        const lr_stencil_reloc_t *rel = &st->relocs[i];
        if ((size_t)rel->offset + rel->size > emit_size) {
            return -1;
        }
        if (rel->size != 1 && rel->size != 2 &&
            rel->size != 4 && rel->size != 8) {
            return -1;
        }
    }

    dst = *code_ptr;
    memcpy(dst, st->bytes, emit_size);

    for (i = 0; i < st->n_relocs; i++) {
        const lr_stencil_reloc_t *rel = &st->relocs[i];
        uint64_t value;
        uint8_t *patch_site;
        value = stencil_patch_value(args, rel->hole);
        patch_site = dst + rel->offset;
        memcpy(patch_site, &value, rel->size);
    }

    *code_ptr = dst + emit_size;
    return 0;
}
