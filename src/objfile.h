#ifndef LIRIC_OBJFILE_H
#define LIRIC_OBJFILE_H

#include "ir.h"
#include "target.h"
#include <stdio.h>
#include <stdint.h>

/* Mach-O arm64 relocation types */
enum {
    LR_RELOC_ARM64_BRANCH26  = 2,
    LR_RELOC_ARM64_PAGE21    = 3,
    LR_RELOC_ARM64_PAGEOFF12 = 4,
};

typedef struct lr_obj_reloc {
    uint32_t offset;
    uint32_t symbol_idx;
    uint8_t type;
} lr_obj_reloc_t;

typedef struct lr_obj_symbol {
    const char *name;
    uint32_t offset;
    uint8_t section;
    bool is_defined;
} lr_obj_symbol_t;

typedef struct lr_objfile_ctx {
    lr_obj_reloc_t *relocs;
    uint32_t num_relocs;
    uint32_t reloc_cap;
    lr_obj_symbol_t *symbols;
    uint32_t num_symbols;
    uint32_t symbol_cap;
} lr_objfile_ctx_t;

int lr_emit_object(lr_module_t *m, const lr_target_t *target, FILE *out);

uint32_t lr_obj_ensure_symbol(lr_objfile_ctx_t *oc, const char *name,
                               bool is_defined, uint8_t section,
                               uint32_t offset);

void lr_obj_add_reloc(lr_objfile_ctx_t *oc, uint32_t offset,
                       uint32_t symbol_idx, uint8_t type);

#endif
