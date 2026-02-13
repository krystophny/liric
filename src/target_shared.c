#include "target_shared.h"
#include "target_common.h"
#include <string.h>

static void lr_target_count_vreg_use(uint32_t *counts,
                                     uint32_t num_counts,
                                     const lr_operand_t *op) {
    if (!counts || !op || op->kind != LR_VAL_VREG) {
        return;
    }
    if (op->vreg < num_counts) {
        counts[op->vreg]++;
    }
}

int32_t lr_target_lookup_static_alloca_offset(const int32_t *offsets,
                                              uint32_t num_offsets,
                                              uint32_t vreg) {
    if (!offsets || vreg >= num_offsets) {
        return 0;
    }
    return offsets[vreg];
}

void lr_target_set_static_alloca_offset(lr_arena_t *arena,
                                        int32_t **offsets,
                                        uint32_t *num_offsets,
                                        uint32_t vreg,
                                        int32_t offset) {
    int32_t *table;
    uint32_t cap;

    if (!arena || !offsets || !num_offsets) {
        return;
    }

    table = *offsets;
    cap = *num_offsets;
    while (vreg >= cap) {
        uint32_t old = cap;
        uint32_t new_cap = old == 0 ? 64 : old * 2;
        int32_t *next = lr_arena_array_uninit(arena, int32_t, new_cap);
        if (old > 0) {
            memcpy(next, table, old * sizeof(int32_t));
        }
        for (uint32_t i = old; i < new_cap; i++) {
            next[i] = 0;
        }
        table = next;
        cap = new_cap;
    }

    table[vreg] = offset;
    *offsets = table;
    *num_offsets = cap;
}

void lr_target_prescan_static_alloca_offsets(lr_func_t *func,
                                             lr_arena_t *arena,
                                             void *ctx,
                                             lr_target_static_alloca_ensure_fn ensure) {
    if (!func || !arena || !ensure) {
        return;
    }

    if (!lr_func_is_finalized(func) && lr_func_finalize(func, arena) != 0) {
        return;
    }

    for (uint32_t ii = 0; ii < func->num_linear_insts; ii++) {
        const lr_inst_t *inst = func->linear_inst_array[ii];
        if (inst->op != LR_OP_ALLOCA) {
            continue;
        }
        if (!lr_target_alloca_uses_static_storage(inst)) {
            continue;
        }
        (void)ensure(ctx, inst);
    }
}

int lr_target_analyze_function(lr_func_t *func,
                               lr_arena_t *arena,
                               lr_block_phi_copies_t *phi_copies,
                               void *alloca_ctx,
                               lr_target_static_alloca_ensure_fn ensure_static_alloca,
                               void *phi_ctx,
                               lr_target_phi_dest_slot_fn reserve_phi_dest_slot,
                               lr_target_func_analysis_t *out) {
    uint32_t num_vregs;

    if (!func || !arena || !out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    if (!lr_func_is_finalized(func) && lr_func_finalize(func, arena) != 0) {
        return -1;
    }

    num_vregs = func->next_vreg;
    out->num_vregs = num_vregs;
    if (num_vregs > 0) {
        out->vreg_use_counts = lr_arena_array(arena, uint32_t, num_vregs);
        for (uint32_t i = 0; i < num_vregs; i++) {
            out->vreg_use_counts[i] = 0;
        }
    }

    for (uint32_t ii = 0; ii < func->num_linear_insts; ii++) {
        const lr_inst_t *inst = func->linear_inst_array[ii];
        if (inst->op == LR_OP_CALL) {
            out->has_calls = true;
        }
        for (uint32_t oi = 0; oi < inst->num_operands; oi++) {
            lr_target_count_vreg_use(out->vreg_use_counts, num_vregs,
                                     &inst->operands[oi]);
        }
        if (inst->op == LR_OP_ALLOCA && lr_target_alloca_uses_static_storage(inst)) {
            if (ensure_static_alloca) {
                (void)ensure_static_alloca(alloca_ctx, inst);
            }
            out->num_static_allocas++;
        }
    }

    if (!phi_copies) {
        return 0;
    }

    for (uint32_t bi = 0; bi < func->num_blocks; bi++) {
        for (uint32_t ci = 0; ci < phi_copies[bi].count; ci++) {
            const lr_phi_copy_t *copy = &phi_copies[bi].copies[ci];
            lr_target_count_vreg_use(out->vreg_use_counts, num_vregs,
                                     &copy->src_op);
            if (reserve_phi_dest_slot) {
                reserve_phi_dest_slot(phi_ctx, copy->dest_vreg);
            }
            out->num_phi_copies++;
        }
    }

    return 0;
}
