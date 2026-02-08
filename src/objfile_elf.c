#include "objfile_elf.h"
#include <stdlib.h>
#include <string.h>

/* ELF64 constants */
#define ELFMAG0 0x7F
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

#define ELFCLASS64      2
#define ELFDATA2LSB     1
#define EV_CURRENT      1
#define ELFOSABI_NONE   0

#define ET_REL          1

#define EM_X86_64       62
#define EM_AARCH64      183

/* Section header types */
#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4

/* Section header flags */
#define SHF_WRITE       0x1
#define SHF_ALLOC       0x2
#define SHF_EXECINSTR   0x4
#define SHF_INFO_LINK   0x40

/* Symbol binding/type */
#define STB_LOCAL       0
#define STB_GLOBAL      1
#define STT_NOTYPE      0
#define STT_FUNC        2
#define STT_SECTION     3
#define SHN_UNDEF       0

/* ELF x86_64 relocation types */
#define R_X86_64_PC32       2
#define R_X86_64_PLT32      4
#define R_X86_64_GOTPCRELX  41
#define R_X86_64_64         1

#define ELF64_ST_INFO(bind, type) (((bind) << 4) | ((type) & 0xF))
#define ELF64_R_INFO(sym, type) (((uint64_t)(sym) << 32) | (uint32_t)(type))

lr_reloc_mapped_t elf_reloc_x86_64(uint8_t liric_type) {
    lr_reloc_mapped_t m = {0};
    switch (liric_type) {
    case LR_RELOC_X86_64_PC32:
        m.native_type = R_X86_64_PC32;
        m.addend = -4;
        m.is_pcrel = true;
        break;
    case LR_RELOC_X86_64_PLT32:
        m.native_type = R_X86_64_PLT32;
        m.addend = -4;
        m.is_pcrel = true;
        break;
    case LR_RELOC_X86_64_GOTPCREL:
        m.native_type = R_X86_64_GOTPCRELX;
        m.addend = -4;
        m.is_pcrel = true;
        break;
    case LR_RELOC_X86_64_64:
        m.native_type = R_X86_64_64;
        m.addend = 0;
        m.is_pcrel = false;
        break;
    default:
        m.native_type = R_X86_64_PC32;
        m.addend = 0;
        m.is_pcrel = true;
        break;
    }
    return m;
}

/*
 * ELF64 relocatable object layout:
 *   ELF header          (64 bytes)
 *   .text section        (code)
 *   .data section        (globals, if any)
 *   .rela.text section   (relocations with explicit addends)
 *   .symtab section      (Elf64_Sym entries, 24 bytes each)
 *   .strtab section      (symbol name strings)
 *   .shstrtab section    (section name strings)
 *   Section headers      (at end of file)
 */
int write_elf(FILE *out, const uint8_t *code, size_t code_size,
              const uint8_t *data, size_t data_size,
              const lr_objfile_ctx_t *oc,
              uint16_t e_machine, lr_reloc_mapper_fn reloc_mapper) {
    bool has_data = data_size > 0;

    /* Section name strings: \0.text\0.data\0.rela.text\0.symtab\0.strtab\0.shstrtab\0 */
    const char shstrtab_content[] =
        "\0.text\0.data\0.rela.text\0.symtab\0.strtab\0.shstrtab";
    size_t shstrtab_size = sizeof(shstrtab_content);
    /* Section name offsets within shstrtab */
    uint32_t sh_name_text      = 1;   /* .text */
    uint32_t sh_name_data      = 7;   /* .data */
    uint32_t sh_name_rela_text = 13;  /* .rela.text */
    uint32_t sh_name_symtab    = 24;  /* .symtab */
    uint32_t sh_name_strtab    = 32;  /* .strtab */
    uint32_t sh_name_shstrtab  = 40;  /* .shstrtab */

    /* Number of sections:
     * 0: SHT_NULL
     * 1: .text
     * 2: .data (if has_data)
     * N: .rela.text
     * N+1: .symtab
     * N+2: .strtab
     * N+3: .shstrtab
     */
    uint16_t text_shndx = 1;
    uint16_t data_shndx = has_data ? 2 : 0;
    uint16_t rela_shndx = has_data ? 3 : 2;
    uint16_t symtab_shndx = rela_shndx + 1;
    uint16_t strtab_shndx = symtab_shndx + 1;
    uint16_t shstrtab_shndx = strtab_shndx + 1;
    uint16_t num_sections = shstrtab_shndx + 1;

    /* Build symbol string table (strtab):
     * ELF does NOT prefix symbol names with underscore (unlike Mach-O) */
    size_t strtab_size = 1;
    uint32_t *str_offsets = calloc(oc->num_symbols, sizeof(uint32_t));
    if (!str_offsets) return -1;
    for (uint32_t i = 0; i < oc->num_symbols; i++) {
        str_offsets[i] = (uint32_t)strtab_size;
        strtab_size += strlen(oc->symbols[i].name) + 1;
    }
    /* Account for section name entries in strtab too -- wait, no.
     * Section names go in .shstrtab, symbol names go in .strtab. They are separate. */

    /* Build symbol table.
     * ELF requires: first entry is STN_UNDEF (all zeros),
     * then local symbols (section symbols), then global symbols.
     * sh_info in .symtab = index of first non-local symbol. */

    /* We need section symbols for .text and .data (for local relocations).
     * Then all our symbols are global. */
    uint32_t num_section_syms = has_data ? 2 : 1; /* .text, optionally .data */
    uint32_t first_global = 1 + num_section_syms; /* after null + section syms */
    uint32_t total_syms = first_global + oc->num_symbols;

    /* Layout computation */
    size_t ehdr_size = 64;
    size_t text_off = ehdr_size;
    size_t text_end = text_off + code_size;

    size_t data_off = has_data ? obj_align_up(text_end, 8) : text_end;
    size_t data_end = data_off + (has_data ? data_size : 0);

    /* .rela.text: Elf64_Rela entries are 24 bytes each */
    size_t rela_off = obj_align_up(data_end, 8);
    size_t rela_size = oc->num_relocs * 24;
    size_t rela_end = rela_off + rela_size;

    /* .symtab: Elf64_Sym entries are 24 bytes each */
    size_t symtab_off = obj_align_up(rela_end, 8);
    size_t symtab_size = total_syms * 24;
    size_t symtab_end = symtab_off + symtab_size;

    /* .strtab */
    size_t strtab_off = symtab_end;
    size_t strtab_end = strtab_off + strtab_size;

    /* .shstrtab */
    size_t shstrtab_off = strtab_end;
    size_t shstrtab_end = shstrtab_off + shstrtab_size;

    /* Section headers (64 bytes each) */
    size_t shdr_off = obj_align_up(shstrtab_end, 8);
    size_t shdr_size = num_sections * 64;
    size_t total_size = shdr_off + shdr_size;

    uint8_t *buf = calloc(1, total_size);
    if (!buf) {
        free(str_offsets);
        return -1;
    }
    uint8_t *p = buf;

    /* ELF header (64 bytes) */
    w8(&p, ELFMAG0); w8(&p, ELFMAG1); w8(&p, ELFMAG2); w8(&p, ELFMAG3);
    w8(&p, ELFCLASS64);
    w8(&p, ELFDATA2LSB);
    w8(&p, EV_CURRENT);
    w8(&p, ELFOSABI_NONE);
    wpad(&p, 8);               /* e_ident padding */
    w16(&p, ET_REL);           /* e_type */
    w16(&p, e_machine);        /* e_machine */
    w32(&p, EV_CURRENT);       /* e_version */
    w64(&p, 0);                /* e_entry */
    w64(&p, 0);                /* e_phoff */
    w64(&p, shdr_off);         /* e_shoff */
    w32(&p, 0);                /* e_flags */
    w16(&p, 64);               /* e_ehsize */
    w16(&p, 0);                /* e_phentsize */
    w16(&p, 0);                /* e_phnum */
    w16(&p, 64);               /* e_shentsize */
    w16(&p, num_sections);     /* e_shnum */
    w16(&p, shstrtab_shndx);   /* e_shstrndx */

    /* .text section data */
    memcpy(buf + text_off, code, code_size);

    /* .data section data */
    if (has_data && data)
        memcpy(buf + data_off, data, data_size);

    /* .rela.text: Elf64_Rela entries (24 bytes: offset, info, addend) */
    {
        uint8_t *rp = buf + rela_off;
        for (uint32_t i = 0; i < oc->num_relocs; i++) {
            const lr_obj_reloc_t *r = &oc->relocs[i];
            lr_reloc_mapped_t mapped = reloc_mapper(r->type);

            /* Symbol index in ELF symtab = first_global + original index */
            uint32_t elf_sym = first_global + r->symbol_idx;

            w64(&rp, (uint64_t)r->offset);                          /* r_offset */
            w64(&rp, ELF64_R_INFO(elf_sym, mapped.native_type));    /* r_info */
            w64(&rp, (uint64_t)(int64_t)mapped.addend);             /* r_addend */
        }
    }

    /* .symtab: Elf64_Sym entries (24 bytes each) */
    {
        uint8_t *sp = buf + symtab_off;

        /* Entry 0: STN_UNDEF (all zeros, already zeroed by calloc) */
        sp += 24;

        /* Section symbol for .text */
        w32(&sp, 0);                                      /* st_name */
        w8(&sp, ELF64_ST_INFO(STB_LOCAL, STT_SECTION));   /* st_info */
        w8(&sp, 0);                                       /* st_other */
        w16(&sp, text_shndx);                              /* st_shndx */
        w64(&sp, 0);                                       /* st_value */
        w64(&sp, 0);                                       /* st_size */

        /* Section symbol for .data (if present) */
        if (has_data) {
            w32(&sp, 0);
            w8(&sp, ELF64_ST_INFO(STB_LOCAL, STT_SECTION));
            w8(&sp, 0);
            w16(&sp, data_shndx);
            w64(&sp, 0);
            w64(&sp, 0);
        }

        /* Global symbols (all liric symbols are global) */
        for (uint32_t i = 0; i < oc->num_symbols; i++) {
            const lr_obj_symbol_t *sym = &oc->symbols[i];

            w32(&sp, str_offsets[i]);                          /* st_name */
            uint8_t stt = (sym->is_defined && sym->section == 1) ? STT_FUNC : STT_NOTYPE;
            w8(&sp, ELF64_ST_INFO(STB_GLOBAL, stt));          /* st_info */
            w8(&sp, 0);                                        /* st_other */
            if (sym->is_defined) {
                uint16_t shndx = (sym->section == 1) ? text_shndx : data_shndx;
                w16(&sp, shndx);                               /* st_shndx */
                uint64_t value = (uint64_t)sym->offset;
                w64(&sp, value);                               /* st_value */
            } else {
                w16(&sp, SHN_UNDEF);                           /* st_shndx */
                w64(&sp, 0);                                   /* st_value */
            }
            w64(&sp, 0);                                       /* st_size */
        }
    }

    /* .strtab */
    {
        uint8_t *tp = buf + strtab_off;
        *tp++ = 0; /* initial NUL byte */
        for (uint32_t i = 0; i < oc->num_symbols; i++) {
            size_t slen = strlen(oc->symbols[i].name);
            memcpy(tp, oc->symbols[i].name, slen + 1);
            tp += slen + 1;
        }
    }

    /* .shstrtab */
    memcpy(buf + shstrtab_off, shstrtab_content, shstrtab_size);

    /* Section headers (64 bytes each) */
    {
        uint8_t *sh = buf + shdr_off;

        /* SHT_NULL (index 0) - already zeros */
        sh += 64;

        /* .text */
        w32(&sh, sh_name_text);     /* sh_name */
        w32(&sh, SHT_PROGBITS);     /* sh_type */
        w64(&sh, SHF_ALLOC | SHF_EXECINSTR); /* sh_flags */
        w64(&sh, 0);                /* sh_addr */
        w64(&sh, text_off);         /* sh_offset */
        w64(&sh, code_size);        /* sh_size */
        w32(&sh, 0);                /* sh_link */
        w32(&sh, 0);                /* sh_info */
        w64(&sh, 16);               /* sh_addralign */
        w64(&sh, 0);                /* sh_entsize */

        /* .data (optional) */
        if (has_data) {
            w32(&sh, sh_name_data);
            w32(&sh, SHT_PROGBITS);
            w64(&sh, SHF_WRITE | SHF_ALLOC);
            w64(&sh, 0);
            w64(&sh, data_off);
            w64(&sh, data_size);
            w32(&sh, 0);
            w32(&sh, 0);
            w64(&sh, 8);
            w64(&sh, 0);
        }

        /* .rela.text */
        w32(&sh, sh_name_rela_text);
        w32(&sh, SHT_RELA);
        w64(&sh, SHF_INFO_LINK);
        w64(&sh, 0);
        w64(&sh, rela_off);
        w64(&sh, rela_size);
        w32(&sh, symtab_shndx);     /* sh_link = .symtab */
        w32(&sh, text_shndx);       /* sh_info = .text */
        w64(&sh, 8);
        w64(&sh, 24);               /* sh_entsize = sizeof(Elf64_Rela) */

        /* .symtab */
        w32(&sh, sh_name_symtab);
        w32(&sh, SHT_SYMTAB);
        w64(&sh, 0);
        w64(&sh, 0);
        w64(&sh, symtab_off);
        w64(&sh, symtab_size);
        w32(&sh, strtab_shndx);     /* sh_link = .strtab */
        w32(&sh, first_global);      /* sh_info = first non-local sym */
        w64(&sh, 8);
        w64(&sh, 24);               /* sh_entsize = sizeof(Elf64_Sym) */

        /* .strtab */
        w32(&sh, sh_name_strtab);
        w32(&sh, SHT_STRTAB);
        w64(&sh, 0);
        w64(&sh, 0);
        w64(&sh, strtab_off);
        w64(&sh, strtab_size);
        w32(&sh, 0);
        w32(&sh, 0);
        w64(&sh, 1);
        w64(&sh, 0);

        /* .shstrtab */
        w32(&sh, sh_name_shstrtab);
        w32(&sh, SHT_STRTAB);
        w64(&sh, 0);
        w64(&sh, 0);
        w64(&sh, shstrtab_off);
        w64(&sh, shstrtab_size);
        w32(&sh, 0);
        w32(&sh, 0);
        w64(&sh, 1);
        w64(&sh, 0);
    }

    size_t written = fwrite(buf, 1, total_size, out);

    free(buf);
    free(str_offsets);

    return written == total_size ? 0 : -1;
}
