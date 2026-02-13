#ifndef LIRIC_STENCIL_DATA_H
#define LIRIC_STENCIL_DATA_H

#include <stddef.h>
#include <stdint.h>

typedef enum lr_stencil_hole {
    LR_STENCIL_HOLE_SRC0_OFF = 0,
    LR_STENCIL_HOLE_SRC1_OFF = 1,
    LR_STENCIL_HOLE_DST_OFF = 2,
    LR_STENCIL_HOLE_IMM64 = 3,
    LR_STENCIL_HOLE_BRANCH_REL = 4,
    LR_STENCIL_HOLE_FUNC_ADDR = 5,
    LR_STENCIL_HOLE_GLOBAL_ADDR = 6
} lr_stencil_hole_t;

typedef struct lr_stencil_reloc {
    uint16_t offset;
    uint8_t size;
    lr_stencil_hole_t hole;
} lr_stencil_reloc_t;

typedef struct lr_stencil {
    const char *name;
    const uint8_t *bytes;
    uint16_t size;
    const lr_stencil_reloc_t *relocs;
    uint8_t n_relocs;
} lr_stencil_t;

size_t lr_stencil_count_generated(void);
const lr_stencil_t *lr_stencil_at_generated(size_t index);
const lr_stencil_t *lr_stencil_lookup_generated(const char *name);

#endif
