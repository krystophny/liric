#ifndef LIRIC_STENCIL_RUNTIME_H
#define LIRIC_STENCIL_RUNTIME_H

#include "ir.h"
#include "stencil_data.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct lr_stencil_emit_args {
    int32_t src0_off;
    int32_t src1_off;
    int32_t dst_off;
    int64_t imm64;
    int32_t branch_rel;
    uintptr_t func_addr;
    uintptr_t global_addr;
} lr_stencil_emit_args_t;

const lr_stencil_t *lr_stencil_lookup_for_ir(lr_opcode_t op, lr_type_kind_t type_kind);

int lr_stencil_emit(uint8_t **code_ptr, uint8_t *code_end,
                    const lr_stencil_t *st, const lr_stencil_emit_args_t *args,
                    bool strip_trailing_ret);

#endif
