#ifndef LIRIC_TARGET_SHARED_H
#define LIRIC_TARGET_SHARED_H

#include "target.h"

typedef int32_t (*lr_target_static_alloca_ensure_fn)(void *ctx,
                                                      const lr_inst_t *inst);
typedef void (*lr_target_phi_dest_slot_fn)(void *ctx, uint32_t dest_vreg);

typedef struct lr_target_func_analysis {
    uint32_t num_vregs;
    uint32_t *vreg_use_counts;
    uint32_t num_static_allocas;
    uint32_t num_phi_copies;
    bool has_calls;
} lr_target_func_analysis_t;

int32_t lr_target_lookup_static_alloca_offset(const int32_t *offsets,
                                              uint32_t num_offsets,
                                              uint32_t vreg);
void lr_target_set_static_alloca_offset(lr_arena_t *arena,
                                        int32_t **offsets,
                                        uint32_t *num_offsets,
                                        uint32_t vreg,
                                        int32_t offset);
void lr_target_prescan_static_alloca_offsets(lr_func_t *func,
                                             lr_arena_t *arena,
                                             void *ctx,
                                             lr_target_static_alloca_ensure_fn ensure);
int lr_target_analyze_function(lr_func_t *func,
                               lr_arena_t *arena,
                               lr_block_phi_copies_t *phi_copies,
                               void *alloca_ctx,
                               lr_target_static_alloca_ensure_fn ensure_static_alloca,
                               void *phi_ctx,
                               lr_target_phi_dest_slot_fn reserve_phi_dest_slot,
                               lr_target_func_analysis_t *out);

#endif
