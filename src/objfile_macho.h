#ifndef LIRIC_OBJFILE_MACHO_H
#define LIRIC_OBJFILE_MACHO_H

#include "objfile.h"

int write_macho(FILE *out, const uint8_t *code, size_t code_size,
                const uint8_t *data, size_t data_size,
                const lr_objfile_ctx_t *oc,
                uint32_t cpu_type, lr_reloc_mapper_fn reloc_mapper);

lr_reloc_mapped_t macho_reloc_arm64(uint8_t liric_type);

#endif
