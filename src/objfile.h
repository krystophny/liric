#ifndef LIRIC_OBJFILE_H
#define LIRIC_OBJFILE_H

#include "ir.h"
#include "target.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ARM64 relocation types (used by aarch64 backend) */
enum {
    LR_RELOC_ARM64_BRANCH26           = 2,
    LR_RELOC_ARM64_PAGE21             = 3,
    LR_RELOC_ARM64_PAGEOFF12          = 4,
    LR_RELOC_ARM64_GOT_LOAD_PAGE21    = 5,
    LR_RELOC_ARM64_GOT_LOAD_PAGEOFF12 = 6,
};

/* x86_64 relocation types (used by x86_64 backend) */
enum {
    LR_RELOC_X86_64_PC32     = 20,
    LR_RELOC_X86_64_PLT32    = 21,
    LR_RELOC_X86_64_GOTPCREL = 22,
    LR_RELOC_X86_64_64       = 23,
};

typedef struct lr_obj_reloc {
    uint32_t offset;
    uint32_t symbol_idx;
    uint8_t type;
} lr_obj_reloc_t;

typedef struct lr_obj_symbol {
    const char *name;
    uint32_t hash;
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
    uint32_t *symbol_index;
    uint32_t symbol_index_cap;
    uint8_t *module_sym_defined;
    lr_func_t **module_sym_funcs;
    uint32_t module_sym_count;
} lr_objfile_ctx_t;

/* Mapped relocation info returned by format-specific reloc mappers */
typedef struct {
    uint32_t native_type;
    int64_t addend;
    bool is_pcrel;
} lr_reloc_mapped_t;

typedef lr_reloc_mapped_t (*lr_reloc_mapper_fn)(uint8_t liric_type);

int lr_emit_object(lr_module_t *m, const lr_target_t *target, FILE *out);

uint32_t lr_obj_ensure_symbol(lr_objfile_ctx_t *oc, const char *name,
                               bool is_defined, uint8_t section,
                               uint32_t offset);

void lr_obj_add_reloc(lr_objfile_ctx_t *oc, uint32_t offset,
                       uint32_t symbol_idx, uint8_t type);

/* Byte-level write helpers shared by Mach-O and ELF format writers */

static inline void w8(uint8_t **p, uint8_t v) {
    *(*p)++ = v;
}

static inline void w16(uint8_t **p, uint16_t v) {
    (*p)[0] = (uint8_t)(v);
    (*p)[1] = (uint8_t)(v >> 8);
    *p += 2;
}

static inline void w32(uint8_t **p, uint32_t v) {
    (*p)[0] = (uint8_t)(v);
    (*p)[1] = (uint8_t)(v >> 8);
    (*p)[2] = (uint8_t)(v >> 16);
    (*p)[3] = (uint8_t)(v >> 24);
    *p += 4;
}

static inline void w64(uint8_t **p, uint64_t v) {
    for (int i = 0; i < 8; i++)
        (*p)[i] = (uint8_t)(v >> (i * 8));
    *p += 8;
}

static inline void wbytes(uint8_t **p, const void *data, size_t n) {
    memcpy(*p, data, n);
    *p += n;
}

static inline void wpad(uint8_t **p, size_t n) {
    memset(*p, 0, n);
    *p += n;
}

static inline size_t obj_align_up(size_t val, size_t alignment) {
    return (val + alignment - 1) & ~(alignment - 1);
}

#endif
