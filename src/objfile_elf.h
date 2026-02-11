#ifndef LIRIC_OBJFILE_ELF_H
#define LIRIC_OBJFILE_ELF_H

#include "objfile.h"

int write_elf(FILE *out, const uint8_t *code, size_t code_size,
              const uint8_t *data, size_t data_size,
              const lr_objfile_ctx_t *oc,
              uint16_t e_machine, lr_reloc_mapper_fn reloc_mapper);

int write_elf_executable_x86_64(FILE *out, const uint8_t *code, size_t code_size,
                                const uint8_t *data, size_t data_size,
                                const lr_objfile_ctx_t *oc,
                                const char *entry_symbol);

lr_reloc_mapped_t elf_reloc_x86_64(uint8_t liric_type);

#endif
