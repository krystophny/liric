#include "target_shared.h"
#include <string.h>

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
