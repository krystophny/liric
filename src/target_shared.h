#ifndef LIRIC_TARGET_SHARED_H
#define LIRIC_TARGET_SHARED_H

#include "target.h"

typedef int32_t (*lr_target_static_alloca_ensure_fn)(void *ctx,
                                                      const lr_inst_t *inst);

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

#endif
