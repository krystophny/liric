#include "objfile_elf.h"
#include <limits.h>
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
#define ET_EXEC         2

#define EM_X86_64       62
#define EM_AARCH64      183
#define EM_RISCV        243

/* Program header constants */
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_GNU_RELRO    0x6474E552
#define PF_X            0x1
#define PF_W            0x2
#define PF_R            0x4

/* Section header types */
#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4
#define SHT_HASH        5
#define SHT_DYNAMIC     6
#define SHT_DYNSYM      11

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
#define R_X86_64_GLOB_DAT   6

/* Dynamic section tags */
#define DT_NULL         0
#define DT_NEEDED       1
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_RELA         7
#define DT_RELASZ       8
#define DT_RELAENT      9
#define DT_STRSZ        10
#define DT_SYMENT       11
#define DT_HASH         4
#define DT_BIND_NOW     24
#define DT_FLAGS        30
#define DT_FLAGS_1      0x6FFFFFFB

#define DF_BIND_NOW     0x8
#define DF_1_NOW        0x1

/* ELF aarch64 relocation types */
#define R_AARCH64_ABS64             257
#define R_AARCH64_CALL26            283
#define R_AARCH64_ADR_PREL_PG_HI21  275
#define R_AARCH64_ADD_ABS_LO12_NC   277
#define R_AARCH64_ADR_GOT_PAGE      311
#define R_AARCH64_LD64_GOT_LO12_NC  312

/* ELF riscv relocation types */
#define R_RISCV_NONE  0
#define R_RISCV_JAL   17

#define ELF64_ST_INFO(bind, type) (((bind) << 4) | ((type) & 0xF))
#define ELF64_R_INFO(sym, type) (((uint64_t)(sym) << 32) | (uint32_t)(type))

static int write_u32_le(uint8_t *buf, size_t buflen, uint32_t off, uint32_t value) {
    if ((size_t)off + 4 > buflen)
        return -1;
    buf[off] = (uint8_t)value;
    buf[off + 1] = (uint8_t)(value >> 8);
    buf[off + 2] = (uint8_t)(value >> 16);
    buf[off + 3] = (uint8_t)(value >> 24);
    return 0;
}

static int write_u64_le(uint8_t *buf, size_t buflen, uint32_t off, uint64_t value) {
    if ((size_t)off + 8 > buflen)
        return -1;
    for (int i = 0; i < 8; i++)
        buf[off + i] = (uint8_t)(value >> (i * 8));
    return 0;
}

static int patch_rel32_vaddr(uint8_t *buf, size_t buflen, uint32_t off,
                             uint64_t place_vaddr, uint64_t target_vaddr) {
    int64_t disp = (int64_t)target_vaddr - (int64_t)(place_vaddr + 4u);
    if (disp < INT32_MIN || disp > INT32_MAX)
        return -1;
    return write_u32_le(buf, buflen, off, (uint32_t)(int32_t)disp);
}

static uint32_t read_u32_le(const uint8_t *buf, uint32_t off) {
    return (uint32_t)buf[off]
         | ((uint32_t)buf[off + 1] << 8)
         | ((uint32_t)buf[off + 2] << 16)
         | ((uint32_t)buf[off + 3] << 24);
}

static int patch_aarch64_branch26_vaddr(uint8_t *buf, size_t buflen, uint32_t off,
                                        uint64_t place_vaddr, uint64_t target_vaddr) {
    if ((size_t)off + 4 > buflen)
        return -1;
    int64_t imm = ((int64_t)target_vaddr - (int64_t)place_vaddr) / 4;
    if (imm < -(1LL << 25) || imm >= (1LL << 25))
        return -1;
    uint32_t insn = read_u32_le(buf, off);
    insn = (insn & 0xFC000000u) | ((uint32_t)imm & 0x03FFFFFFu);
    return write_u32_le(buf, buflen, off, insn);
}

static int patch_aarch64_page21_vaddr(uint8_t *buf, size_t buflen, uint32_t off,
                                      uint64_t place_vaddr, uint64_t target_vaddr) {
    if ((size_t)off + 4 > buflen)
        return -1;
    uint64_t target_page = target_vaddr & ~0xFFFULL;
    uint64_t place_page = place_vaddr & ~0xFFFULL;
    int64_t pages = ((int64_t)target_page - (int64_t)place_page) >> 12;
    if (pages < -(1LL << 20) || pages >= (1LL << 20))
        return -1;
    uint32_t insn = read_u32_le(buf, off);
    insn &= ~((0x3u << 29) | (0x7FFFFu << 5));
    insn |= (((uint32_t)pages & 0x3u) << 29);
    insn |= ((((uint32_t)pages >> 2) & 0x7FFFFu) << 5);
    return write_u32_le(buf, buflen, off, insn);
}

static int patch_aarch64_pageoff12_vaddr(uint8_t *buf, size_t buflen, uint32_t off,
                                         uint64_t target_vaddr, bool got_load) {
    if ((size_t)off + 4 > buflen)
        return -1;
    uint32_t imm = (uint32_t)(target_vaddr & 0xFFFu);
    if (got_load) {
        if ((imm & 0x7u) != 0)
            return -1;
        imm >>= 3;
    }
    uint32_t insn = read_u32_le(buf, off);
    insn &= ~(0xFFFu << 10);
    insn |= ((imm & 0xFFFu) << 10);
    return write_u32_le(buf, buflen, off, insn);
}

static int patch_riscv_jal_vaddr(uint8_t *buf, size_t buflen, uint32_t off,
                                 uint64_t place_vaddr, uint64_t target_vaddr) {
    if ((size_t)off + 4 > buflen)
        return -1;
    int64_t disp = (int64_t)target_vaddr - (int64_t)place_vaddr;
    if ((disp & 1LL) != 0)
        return -1;
    if (disp < -(1LL << 20) * 2LL || disp > (((1LL << 20) - 1LL) * 2LL))
        return -1;

    int32_t imm = (int32_t)(disp >> 1);
    uint32_t uimm = (uint32_t)imm;
    uint32_t insn = read_u32_le(buf, off);
    uint32_t low = insn & 0x00000FFFu; /* rd + opcode */
    uint32_t high = 0;
    high |= ((uimm >> 20) & 0x1u) << 31;
    high |= ((uimm >> 1) & 0x3FFu) << 21;
    high |= ((uimm >> 11) & 0x1u) << 20;
    high |= ((uimm >> 12) & 0xFFu) << 12;
    return write_u32_le(buf, buflen, off, low | high);
}

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

lr_reloc_mapped_t elf_reloc_aarch64(uint8_t liric_type) {
    lr_reloc_mapped_t m = {0};
    switch (liric_type) {
    case LR_RELOC_ARM64_BRANCH26:
        m.native_type = R_AARCH64_CALL26;
        m.addend = 0;
        m.is_pcrel = true;
        break;
    case LR_RELOC_ARM64_PAGE21:
        m.native_type = R_AARCH64_ADR_PREL_PG_HI21;
        m.addend = 0;
        m.is_pcrel = true;
        break;
    case LR_RELOC_ARM64_PAGEOFF12:
        m.native_type = R_AARCH64_ADD_ABS_LO12_NC;
        m.addend = 0;
        m.is_pcrel = false;
        break;
    case LR_RELOC_ARM64_GOT_LOAD_PAGE21:
        m.native_type = R_AARCH64_ADR_GOT_PAGE;
        m.addend = 0;
        m.is_pcrel = true;
        break;
    case LR_RELOC_ARM64_GOT_LOAD_PAGEOFF12:
        m.native_type = R_AARCH64_LD64_GOT_LO12_NC;
        m.addend = 0;
        m.is_pcrel = false;
        break;
    case LR_RELOC_ARM64_ABS64:
        m.native_type = R_AARCH64_ABS64;
        m.addend = 0;
        m.is_pcrel = false;
        break;
    default:
        m.native_type = R_AARCH64_CALL26;
        m.addend = 0;
        m.is_pcrel = true;
        break;
    }
    return m;
}

lr_reloc_mapped_t elf_reloc_riscv64(uint8_t liric_type) {
    lr_reloc_mapped_t m = {0};
    switch (liric_type) {
    default:
        m.native_type = R_RISCV_NONE;
        m.addend = 0;
        m.is_pcrel = false;
        break;
    }
    return m;
}

/*
 * ELF64 relocatable object layout:
 *   ELF header          (64 bytes)
 *   .text section        (code)
 *   .data section        (globals, if any)
 *   .rela.text section   (text relocations)
 *   .rela.data section   (data relocations, if any)
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
    bool has_data_relocs = oc->num_data_relocs > 0;

    /* Section name strings:
     * \0.text\0.data\0.rela.text\0.symtab\0.strtab\0.shstrtab\0.rela.data\0
     * The .rela.data entry is appended at the end so existing offsets stay stable. */
    const char shstrtab_content[] =
        "\0.text\0.data\0.rela.text\0.symtab\0.strtab\0.shstrtab\0.rela.data";
    size_t shstrtab_size = sizeof(shstrtab_content);
    /* Section name offsets within shstrtab */
    uint32_t sh_name_text      = 1;   /* .text */
    uint32_t sh_name_data      = 7;   /* .data */
    uint32_t sh_name_rela_text = 13;  /* .rela.text */
    uint32_t sh_name_symtab    = 24;  /* .symtab */
    uint32_t sh_name_strtab    = 32;  /* .strtab */
    uint32_t sh_name_shstrtab  = 40;  /* .shstrtab */
    uint32_t sh_name_rela_data = 50;  /* .rela.data */

    /* Section indices:
     * 0: SHT_NULL
     * 1: .text
     * 2: .data (if has_data)
     * N: .rela.text
     * N+1: .rela.data (if has_data_relocs)
     * M: .symtab
     * M+1: .strtab
     * M+2: .shstrtab
     */
    uint16_t text_shndx = 1;
    uint16_t data_shndx = has_data ? 2 : 0;
    uint16_t rela_text_shndx = has_data ? 3 : 2;
    uint16_t rela_data_shndx = has_data_relocs ? (rela_text_shndx + 1) : 0;
    uint16_t symtab_shndx = (has_data_relocs ? rela_data_shndx : rela_text_shndx) + 1;
    uint16_t strtab_shndx = symtab_shndx + 1;
    uint16_t shstrtab_shndx = strtab_shndx + 1;
    uint16_t num_sections = shstrtab_shndx + 1;

    size_t strtab_size = 1;
    uint32_t *str_offsets = calloc(oc->num_symbols, sizeof(uint32_t));
    uint32_t *elf_sym_index = calloc(oc->num_symbols, sizeof(uint32_t));
    uint32_t *sym_order = calloc(oc->num_symbols, sizeof(uint32_t));
    if (!str_offsets || !elf_sym_index || !sym_order) {
        free(str_offsets);
        free(elf_sym_index);
        free(sym_order);
        return -1;
    }
    for (uint32_t i = 0; i < oc->num_symbols; i++) {
        str_offsets[i] = (uint32_t)strtab_size;
        strtab_size += strlen(oc->symbols[i].name) + 1;
    }

    uint32_t num_section_syms = has_data ? 2 : 1; /* .text, optionally .data */
    uint32_t local_syms = 0;
    for (uint32_t i = 0; i < oc->num_symbols; i++) {
        const lr_obj_symbol_t *sym = &oc->symbols[i];
        if (sym->is_defined && sym->is_local)
            local_syms++;
    }
    uint32_t first_local = 1 + num_section_syms;
    uint32_t first_global = first_local + local_syms;
    uint32_t total_syms = first_global + (oc->num_symbols - local_syms);
    uint32_t next_local = first_local;
    uint32_t next_global = first_global;
    uint32_t local_pos = 0;
    uint32_t global_pos = local_syms;
    for (uint32_t i = 0; i < oc->num_symbols; i++) {
        const lr_obj_symbol_t *sym = &oc->symbols[i];
        if (sym->is_defined && sym->is_local) {
            elf_sym_index[i] = next_local++;
            sym_order[local_pos++] = i;
        } else {
            elf_sym_index[i] = next_global++;
            sym_order[global_pos++] = i;
        }
    }

    /* Layout computation */
    size_t ehdr_size = 64;
    size_t text_off = ehdr_size;
    size_t text_end = text_off + code_size;

    size_t data_off = has_data ? obj_align_up(text_end, 8) : text_end;
    size_t data_end = data_off + (has_data ? data_size : 0);

    /* .rela.text: Elf64_Rela entries are 24 bytes each */
    size_t rela_text_off = obj_align_up(data_end, 8);
    size_t rela_text_size = oc->num_relocs * 24;
    size_t rela_text_end = rela_text_off + rela_text_size;

    /* .rela.data (if any) */
    size_t rela_data_off = has_data_relocs ? obj_align_up(rela_text_end, 8) : rela_text_end;
    size_t rela_data_size = has_data_relocs ? (oc->num_data_relocs * 24) : 0;
    size_t rela_data_end = rela_data_off + rela_data_size;

    /* .symtab: Elf64_Sym entries are 24 bytes each */
    size_t symtab_off = obj_align_up(rela_data_end, 8);
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
        free(elf_sym_index);
        free(sym_order);
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

    memcpy(buf + text_off, code, code_size);

    if (has_data && data)
        memcpy(buf + data_off, data, data_size);

    /* .rela.text */
    {
        uint8_t *rp = buf + rela_text_off;
        for (uint32_t i = 0; i < oc->num_relocs; i++) {
            const lr_obj_reloc_t *r = &oc->relocs[i];
            lr_reloc_mapped_t mapped = reloc_mapper(r->type);
            uint32_t elf_sym = elf_sym_index[r->symbol_idx];
            w64(&rp, (uint64_t)r->offset);
            w64(&rp, ELF64_R_INFO(elf_sym, mapped.native_type));
            w64(&rp, (uint64_t)(int64_t)mapped.addend);
        }
    }

    /* .rela.data */
    if (has_data_relocs) {
        uint8_t *rp = buf + rela_data_off;
        for (uint32_t i = 0; i < oc->num_data_relocs; i++) {
            const lr_obj_reloc_t *r = &oc->data_relocs[i];
            lr_reloc_mapped_t mapped = reloc_mapper(r->type);
            uint32_t elf_sym = elf_sym_index[r->symbol_idx];
            w64(&rp, (uint64_t)r->offset);
            w64(&rp, ELF64_R_INFO(elf_sym, mapped.native_type));
            w64(&rp, (uint64_t)(int64_t)mapped.addend);
        }
    }

    /* .symtab */
    {
        uint8_t *sp = buf + symtab_off;
        sp += 24; /* STN_UNDEF */

        /* Section symbol: .text */
        w32(&sp, 0);
        w8(&sp, ELF64_ST_INFO(STB_LOCAL, STT_SECTION));
        w8(&sp, 0);
        w16(&sp, text_shndx);
        w64(&sp, 0);
        w64(&sp, 0);

        /* Section symbol: .data */
        if (has_data) {
            w32(&sp, 0);
            w8(&sp, ELF64_ST_INFO(STB_LOCAL, STT_SECTION));
            w8(&sp, 0);
            w16(&sp, data_shndx);
            w64(&sp, 0);
            w64(&sp, 0);
        }

        for (uint32_t oi = 0; oi < oc->num_symbols; oi++) {
            uint32_t i = sym_order[oi];
            const lr_obj_symbol_t *sym = &oc->symbols[i];
            uint8_t bind = (sym->is_defined && sym->is_local)
                ? STB_LOCAL
                : STB_GLOBAL;
            w32(&sp, str_offsets[i]);
            uint8_t stt = (sym->is_defined && sym->section == 1) ? STT_FUNC : STT_NOTYPE;
            w8(&sp, ELF64_ST_INFO(bind, stt));
            w8(&sp, 0);
            if (sym->is_defined) {
                uint16_t shndx = (sym->section == 1) ? text_shndx : data_shndx;
                w16(&sp, shndx);
                w64(&sp, (uint64_t)sym->offset);
            } else {
                w16(&sp, SHN_UNDEF);
                w64(&sp, 0);
            }
            w64(&sp, 0);
        }
    }

    /* .strtab */
    {
        uint8_t *tp = buf + strtab_off;
        *tp++ = 0;
        for (uint32_t i = 0; i < oc->num_symbols; i++) {
            size_t slen = strlen(oc->symbols[i].name);
            memcpy(tp, oc->symbols[i].name, slen + 1);
            tp += slen + 1;
        }
    }

    /* .shstrtab */
    memcpy(buf + shstrtab_off, shstrtab_content, shstrtab_size);

    /* Section headers */
    {
        uint8_t *sh = buf + shdr_off;
        sh += 64; /* SHT_NULL */

        /* .text */
        w32(&sh, sh_name_text);
        w32(&sh, SHT_PROGBITS);
        w64(&sh, SHF_ALLOC | SHF_EXECINSTR);
        w64(&sh, 0);
        w64(&sh, text_off);
        w64(&sh, code_size);
        w32(&sh, 0);
        w32(&sh, 0);
        w64(&sh, 16);
        w64(&sh, 0);

        /* .data */
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
        w64(&sh, rela_text_off);
        w64(&sh, rela_text_size);
        w32(&sh, symtab_shndx);
        w32(&sh, text_shndx);
        w64(&sh, 8);
        w64(&sh, 24);

        /* .rela.data */
        if (has_data_relocs) {
            w32(&sh, sh_name_rela_data);
            w32(&sh, SHT_RELA);
            w64(&sh, SHF_INFO_LINK);
            w64(&sh, 0);
            w64(&sh, rela_data_off);
            w64(&sh, rela_data_size);
            w32(&sh, symtab_shndx);
            w32(&sh, data_shndx);       /* sh_info = .data section */
            w64(&sh, 8);
            w64(&sh, 24);
        }

        /* .symtab */
        w32(&sh, sh_name_symtab);
        w32(&sh, SHT_SYMTAB);
        w64(&sh, 0);
        w64(&sh, 0);
        w64(&sh, symtab_off);
        w64(&sh, symtab_size);
        w32(&sh, strtab_shndx);
        w32(&sh, first_global);
        w64(&sh, 8);
        w64(&sh, 24);

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
    free(elf_sym_index);
    free(sym_order);

    return written == total_size ? 0 : -1;
}

int write_elf_executable_x86_64(FILE *out, const uint8_t *code, size_t code_size,
                                const uint8_t *data, size_t data_size,
                                const lr_objfile_ctx_t *oc,
                                const char *entry_symbol) {
    if (!out || !code || !oc || !entry_symbol || !entry_symbol[0])
        return -1;

    const uint64_t image_base = 0x400000ULL;
    const size_t ehdr_size = 64;
    const size_t phdr_size = 56;
    const size_t file_align = 16;
    const size_t page_align = 4096;

    /* _start:
     *   call <entry_symbol>
     *   mov edi, eax
     *   mov eax, 60
     *   syscall
     */
    static const uint8_t start_stub_template[] = {
        0xE8, 0x00, 0x00, 0x00, 0x00,
        0x89, 0xC7,
        0xB8, 0x3C, 0x00, 0x00, 0x00,
        0x0F, 0x05
    };
    const size_t start_stub_size = sizeof(start_stub_template);

    uint32_t *got_slot_off = NULL;
    if (oc->num_symbols > 0) {
        got_slot_off = (uint32_t *)malloc(sizeof(uint32_t) * oc->num_symbols);
        if (!got_slot_off)
            return -1;
        for (uint32_t i = 0; i < oc->num_symbols; i++)
            got_slot_off[i] = UINT32_MAX;
    }

    size_t data_runtime_size = data_size;
    data_runtime_size = obj_align_up(data_runtime_size, 8);
    for (uint32_t i = 0; i < oc->num_relocs; i++) {
        const lr_obj_reloc_t *rel = &oc->relocs[i];
        if (rel->type != LR_RELOC_X86_64_GOTPCREL)
            continue;
        if (rel->symbol_idx >= oc->num_symbols) {
            free(got_slot_off);
            return -1;
        }
        if (got_slot_off[rel->symbol_idx] != UINT32_MAX)
            continue;
        if (data_runtime_size > UINT32_MAX - 8u) {
            free(got_slot_off);
            return -1;
        }
        got_slot_off[rel->symbol_idx] = (uint32_t)data_runtime_size;
        data_runtime_size += 8u;
    }

    size_t text_off = obj_align_up(ehdr_size + phdr_size, file_align);
    size_t code_off = text_off + start_stub_size;
    size_t data_off = obj_align_up(code_off + code_size, file_align);
    size_t total_size = data_off + data_runtime_size;

    uint64_t entry_vaddr = image_base + text_off;
    uint64_t code_vaddr = image_base + code_off;
    uint64_t data_vaddr = image_base + data_off;

    const lr_obj_symbol_t *entry_sym = NULL;
    for (uint32_t i = 0; i < oc->num_symbols; i++) {
        const lr_obj_symbol_t *sym = &oc->symbols[i];
        if (sym->is_defined && sym->section == 1 &&
            strcmp(sym->name, entry_symbol) == 0) {
            entry_sym = sym;
            break;
        }
    }
    if (!entry_sym || (size_t)entry_sym->offset >= code_size)
        return -1;

    uint8_t *buf = (uint8_t *)calloc(1, total_size);
    uint8_t *code_mut = (uint8_t *)malloc(code_size);
    uint8_t *data_mut = (uint8_t *)calloc(1, data_runtime_size > 0 ? data_runtime_size : 1u);
    if (!buf || !code_mut || !data_mut) {
        free(buf);
        free(code_mut);
        free(data_mut);
        free(got_slot_off);
        return -1;
    }
    memcpy(code_mut, code, code_size);
    if (data && data_size > 0)
        memcpy(data_mut, data, data_size);

    for (uint32_t i = 0; i < oc->num_relocs; i++) {
        const lr_obj_reloc_t *rel = &oc->relocs[i];
        if (rel->symbol_idx >= oc->num_symbols) {
            free(buf);
            free(code_mut);
            free(data_mut);
            free(got_slot_off);
            return -1;
        }
        if ((size_t)rel->offset + 4 > code_size) {
            free(buf);
            free(code_mut);
            free(data_mut);
            free(got_slot_off);
            return -1;
        }

        const lr_obj_symbol_t *sym = &oc->symbols[rel->symbol_idx];
        if (!sym->is_defined) {
            free(buf);
            free(code_mut);
            free(data_mut);
            free(got_slot_off);
            return -1;
        }

        uint64_t target_vaddr;
        if (sym->section == 1) {
            if ((size_t)sym->offset >= code_size) {
                free(buf);
                free(code_mut);
                free(data_mut);
                free(got_slot_off);
                return -1;
            }
            target_vaddr = code_vaddr + sym->offset;
        } else if (sym->section == 2) {
            if ((size_t)sym->offset >= data_runtime_size) {
                free(buf);
                free(code_mut);
                free(data_mut);
                free(got_slot_off);
                return -1;
            }
            target_vaddr = data_vaddr + sym->offset;
        } else {
            free(buf);
            free(code_mut);
            free(data_mut);
            free(got_slot_off);
            return -1;
        }

        uint64_t place_vaddr = code_vaddr + rel->offset;
        int rc = 0;
        switch (rel->type) {
        case LR_RELOC_X86_64_PC32:
        case LR_RELOC_X86_64_PLT32:
            rc = patch_rel32_vaddr(code_mut, code_size, rel->offset,
                                   place_vaddr, target_vaddr);
            break;
        case LR_RELOC_X86_64_GOTPCREL: {
            if (!got_slot_off || rel->symbol_idx >= oc->num_symbols ||
                got_slot_off[rel->symbol_idx] == UINT32_MAX) {
                rc = -1;
                break;
            }
            uint32_t slot_off = got_slot_off[rel->symbol_idx];
            uint64_t slot_vaddr = data_vaddr + (uint64_t)slot_off;
            if (write_u64_le(data_mut, data_runtime_size, slot_off, target_vaddr) != 0) {
                rc = -1;
                break;
            }
            rc = patch_rel32_vaddr(code_mut, code_size, rel->offset,
                                   place_vaddr, slot_vaddr);
            break;
        }
        case LR_RELOC_X86_64_64:
            rc = write_u64_le(code_mut, code_size, rel->offset, target_vaddr);
            break;
        default:
            rc = -1;
            break;
        }
        if (rc != 0) {
            free(buf);
            free(code_mut);
            free(data_mut);
            free(got_slot_off);
            return -1;
        }
    }

    for (uint32_t i = 0; i < oc->num_data_relocs; i++) {
        const lr_obj_reloc_t *rel = &oc->data_relocs[i];
        if (rel->symbol_idx >= oc->num_symbols ||
            rel->type != LR_RELOC_X86_64_64) {
            free(buf);
            free(code_mut);
            free(data_mut);
            free(got_slot_off);
            return -1;
        }
        if ((size_t)rel->offset + 8 > data_runtime_size) {
            free(buf);
            free(code_mut);
            free(data_mut);
            free(got_slot_off);
            return -1;
        }

        const lr_obj_symbol_t *sym = &oc->symbols[rel->symbol_idx];
        if (!sym->is_defined) {
            free(buf);
            free(code_mut);
            free(data_mut);
            free(got_slot_off);
            return -1;
        }

        uint64_t target_vaddr;
        if (sym->section == 1)
            target_vaddr = code_vaddr + sym->offset;
        else if (sym->section == 2)
            target_vaddr = data_vaddr + sym->offset;
        else {
            free(buf);
            free(code_mut);
            free(data_mut);
            free(got_slot_off);
            return -1;
        }

        if (write_u64_le(data_mut, data_runtime_size, rel->offset,
                          target_vaddr) != 0) {
            free(buf);
            free(code_mut);
            free(data_mut);
            free(got_slot_off);
            return -1;
        }
    }

    uint8_t *p = buf;
    w8(&p, ELFMAG0); w8(&p, ELFMAG1); w8(&p, ELFMAG2); w8(&p, ELFMAG3);
    w8(&p, ELFCLASS64);
    w8(&p, ELFDATA2LSB);
    w8(&p, EV_CURRENT);
    w8(&p, ELFOSABI_NONE);
    wpad(&p, 8);
    w16(&p, ET_EXEC);
    w16(&p, EM_X86_64);
    w32(&p, EV_CURRENT);
    w64(&p, entry_vaddr);
    w64(&p, ehdr_size);
    w64(&p, 0);
    w32(&p, 0);
    w16(&p, 64);
    w16(&p, 56);
    w16(&p, 1);
    w16(&p, 0);
    w16(&p, 0);
    w16(&p, 0);

    w32(&p, PT_LOAD);
    w32(&p, PF_R | PF_W | PF_X);
    w64(&p, 0);
    w64(&p, image_base);
    w64(&p, image_base);
    w64(&p, total_size);
    w64(&p, total_size);
    w64(&p, page_align);

    memcpy(buf + text_off, start_stub_template, start_stub_size);
    memcpy(buf + code_off, code_mut, code_size);
    if (data_runtime_size > 0)
        memcpy(buf + data_off, data_mut, data_runtime_size);

    if (patch_rel32_vaddr(buf, total_size, (uint32_t)(text_off + 1),
                          entry_vaddr + 1, code_vaddr + entry_sym->offset) != 0) {
        free(buf);
        free(code_mut);
        free(data_mut);
        free(got_slot_off);
        return -1;
    }

    size_t written = fwrite(buf, 1, total_size, out);
    free(buf);
    free(code_mut);
    free(data_mut);
    free(got_slot_off);
    return written == total_size ? 0 : -1;
}

/*
 * SysV ELF hash function for .hash section lookup.
 */
static uint32_t elf_sysv_hash(const char *name) {
    uint32_t h = 0, g;
    while (*name) {
        h = (h << 4) + (uint8_t)*name++;
        g = h & 0xF0000000;
        if (g)
            h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

/*
 * Write a dynamically-linked ELF executable for x86_64.
 *
 * Produces an ET_EXEC binary with PT_INTERP and PT_DYNAMIC that the Linux
 * dynamic linker (ld-linux-x86-64.so.2) can load.  Undefined symbols are
 * resolved at load time via GOT entries filled by R_X86_64_GLOB_DAT
 * relocations.  DT_BIND_NOW forces eager binding so no PLT is required.
 *
 * Calls to undefined external functions go through 6-byte trampolines
 * (jmp qword [rip+disp32]) appended after the user code.  The original
 * E8 rel32 call sites are patched to target the appropriate trampoline.
 *
 * Layout (two PT_LOAD segments):
 *   Text segment (R+X): ELF header, phdrs, .interp, .hash, .dynsym,
 *                        .dynstr, .text (start stub + code + trampolines)
 *   Data segment (R+W): .rela.dyn, .got, .data, .dynamic
 *   Section headers at end of file (non-loaded, for readelf/objdump)
 */
int write_elf_dynamic_executable_x86_64(FILE *out,
                                         const uint8_t *code, size_t code_size,
                                         const uint8_t *data, size_t data_size,
                                         const lr_objfile_ctx_t *oc,
                                         const char *entry_symbol) {
    if (!out || !code || !oc || !entry_symbol || !entry_symbol[0])
        return -1;

    const uint64_t image_base = 0x400000ULL;
    const size_t ehdr_size = 64;
    const size_t phdr_size = 56;
    const size_t page_align = 0x1000;

    /* _start stub: call <entry>; mov edi, eax; call <exit_trampoline>; hlt
     * Using libc exit() instead of raw syscall ensures stdio buffers are flushed. */
    static const uint8_t start_stub_template[] = {
        0xE8, 0x00, 0x00, 0x00, 0x00,  /* call rel32 (entry) */
        0x89, 0xC7,                      /* mov edi, eax */
        0xE8, 0x00, 0x00, 0x00, 0x00,  /* call rel32 (exit trampoline) */
        0xF4                             /* hlt (unreachable) */
    };
    const size_t start_stub_size = sizeof(start_stub_template);
    const size_t exit_call_off = 8; /* byte offset of disp32 in exit call */

    static const char interp[] = "/lib64/ld-linux-x86-64.so.2";
    const size_t interp_size = sizeof(interp); /* includes NUL */
    static const char libc_name[] = "libc.so.6";
    static const char libm_name[] = "libm.so.6";

    /* Count undefined symbols. Always inject "exit" for the _start stub. */
    uint32_t num_undef = 0;
    bool exit_in_oc = false;
    for (uint32_t i = 0; i < oc->num_symbols; i++) {
        if (!oc->symbols[i].is_defined) {
            num_undef++;
            if (strcmp(oc->symbols[i].name, "exit") == 0)
                exit_in_oc = true;
        }
    }
    if (num_undef == 0)
        return -1; /* caller should use static writer instead */

    /* num_dynimport = module undefined symbols + synthetic "exit" if needed */
    uint32_t num_dynimport = num_undef + (exit_in_oc ? 0 : 1);
    uint32_t exit_dyn_idx = 0; /* 0-based index into dyn_* arrays */

    const char **dyn_names = (const char **)malloc(num_dynimport * sizeof(char *));
    uint32_t *dyn_oc_idx = (uint32_t *)malloc(num_dynimport * sizeof(uint32_t));
    uint32_t *sym_to_dynsym = (uint32_t *)calloc(oc->num_symbols, sizeof(uint32_t));
    if (!dyn_names || !dyn_oc_idx || !sym_to_dynsym) {
        free(dyn_names); free(dyn_oc_idx); free(sym_to_dynsym);
        return -1;
    }
    {
        uint32_t di = 0;
        for (uint32_t i = 0; i < oc->num_symbols; i++) {
            if (!oc->symbols[i].is_defined) {
                dyn_names[di] = oc->symbols[i].name;
                dyn_oc_idx[di] = i;
                sym_to_dynsym[i] = di + 1;
                if (strcmp(oc->symbols[i].name, "exit") == 0)
                    exit_dyn_idx = di;
                di++;
            }
        }
        if (!exit_in_oc) {
            dyn_names[di] = "exit";
            dyn_oc_idx[di] = UINT32_MAX;
            exit_dyn_idx = di;
            di++;
        }
    }

    /* Build .dynstr: "\0libc.so.6\0libm.so.6\0sym1\0sym2\0..." */
    size_t dynstr_size = 1; /* leading NUL */
    size_t libc_name_off = dynstr_size;
    dynstr_size += sizeof(libc_name); /* includes NUL */
    size_t libm_name_off = dynstr_size;
    dynstr_size += sizeof(libm_name); /* includes NUL */
    uint32_t *dyn_name_off = (uint32_t *)malloc(num_dynimport * sizeof(uint32_t));
    if (!dyn_name_off) {
        free(dyn_names); free(dyn_oc_idx); free(sym_to_dynsym);
        return -1;
    }
    for (uint32_t i = 0; i < num_dynimport; i++) {
        dyn_name_off[i] = (uint32_t)dynstr_size;
        dynstr_size += strlen(dyn_names[i]) + 1;
    }

    uint32_t dynsym_count = 1 + num_dynimport;
    size_t dynsym_size = (size_t)dynsym_count * 24;

    uint32_t nbucket = num_dynimport > 0 ? num_dynimport : 1;
    uint32_t nchain = dynsym_count;
    size_t hash_size = (2 + nbucket + nchain) * 4;

    size_t rela_dyn_size = (size_t)num_dynimport * 24;
    size_t trampoline_size = (size_t)num_dynimport * 6;
    size_t got_size = (size_t)num_dynimport * 8;

    /* .dynamic: tag entries (each 16 bytes) */
    /* DT_NEEDED(libc), DT_NEEDED(libm), DT_HASH, DT_STRTAB, DT_SYMTAB,
     * DT_STRSZ, DT_SYMENT, DT_RELA, DT_RELASZ, DT_RELAENT, DT_BIND_NOW,
     * DT_FLAGS, DT_FLAGS_1, DT_NULL  = 14 entries */
    const uint32_t num_dynamic_entries = 14;
    size_t dynamic_size = (size_t)num_dynamic_entries * 16;

    /* -- Layout computation --
     *
     * Text segment (page-aligned, R+X):
     *   ehdr + phdrs | .interp | .hash | .dynsym | .dynstr | .text
     * Data segment (page-aligned, R+W):
     *   .rela.dyn | .got | .data | .dynamic
     * Section headers (not loaded):
     *   null + .interp + .hash + .dynsym + .dynstr + .text + .rela.dyn
     *   + .got + .data + .dynamic + .shstrtab = 11 sections
     */
    const uint32_t num_phdrs = 4; /* PT_INTERP, PT_LOAD(text), PT_LOAD(data), PT_DYNAMIC */
    size_t phdrs_end = ehdr_size + (size_t)num_phdrs * phdr_size;

    size_t interp_off = obj_align_up(phdrs_end, 1);
    size_t interp_end = interp_off + interp_size;

    size_t hash_off = obj_align_up(interp_end, 4);
    size_t hash_end = hash_off + hash_size;

    size_t dynsym_off = obj_align_up(hash_end, 8);
    size_t dynsym_end = dynsym_off + dynsym_size;

    size_t dynstr_off = dynsym_end;
    size_t dynstr_end = dynstr_off + dynstr_size;

    size_t text_off = obj_align_up(dynstr_end, 16);
    size_t code_off = text_off + start_stub_size;
    size_t tramp_off = code_off + code_size;
    size_t text_end = tramp_off + trampoline_size;

    /* Data segment starts on next page boundary */
    size_t data_seg_off = obj_align_up(text_end, page_align);

    size_t rela_dyn_off = data_seg_off;
    size_t rela_dyn_end = rela_dyn_off + rela_dyn_size;

    size_t got_off = obj_align_up(rela_dyn_end, 8);
    size_t got_end = got_off + got_size;

    /* Internal defined-symbol GOT slots (for GOTPCREL relocations to
     * defined symbols, same approach as the static writer) */
    uint32_t *int_got_slot_off = NULL;
    if (oc->num_symbols > 0) {
        int_got_slot_off = (uint32_t *)calloc(oc->num_symbols, sizeof(uint32_t));
        if (!int_got_slot_off) {
            free(dyn_names); free(dyn_oc_idx);
            free(sym_to_dynsym); free(dyn_name_off);
            return -1;
        }
        for (uint32_t i = 0; i < oc->num_symbols; i++)
            int_got_slot_off[i] = UINT32_MAX;
    }

    size_t extra_got_size = 0;
    for (uint32_t i = 0; i < oc->num_relocs; i++) {
        const lr_obj_reloc_t *rel = &oc->relocs[i];
        if (rel->type != LR_RELOC_X86_64_GOTPCREL)
            continue;
        if (rel->symbol_idx >= oc->num_symbols)
            continue;
        if (!oc->symbols[rel->symbol_idx].is_defined)
            continue; /* handled by dynamic GOT */
        if (int_got_slot_off[rel->symbol_idx] != UINT32_MAX)
            continue;
        int_got_slot_off[rel->symbol_idx] = (uint32_t)(got_end - data_seg_off + extra_got_size);
        extra_got_size += 8;
    }

    size_t user_data_off = obj_align_up(got_end + extra_got_size, 8);
    size_t user_data_end = user_data_off + data_size;

    size_t dynamic_off = obj_align_up(user_data_end, 8);
    size_t dynamic_end = dynamic_off + dynamic_size;

    size_t data_seg_end = dynamic_end;

    /* Section header string table */
    /* \0.interp\0.hash\0.dynsym\0.dynstr\0.text\0.rela.dyn\0.got\0.data\0.dynamic\0.shstrtab\0 */
    const char shstrtab_content[] =
        "\0.interp\0.hash\0.dynsym\0.dynstr\0.text\0.rela.dyn\0.got\0.data\0.dynamic\0.shstrtab";
    size_t shstrtab_size = sizeof(shstrtab_content);
    uint32_t shn_interp  = 1;
    uint32_t shn_hash    = 9;
    uint32_t shn_dynsym  = 15;
    uint32_t shn_dynstr  = 23;
    uint32_t shn_text    = 31;
    uint32_t shn_reladyn = 37;
    uint32_t shn_got     = 47;
    uint32_t shn_data    = 52;
    uint32_t shn_dynamic = 58;
    uint32_t shn_shstrtab = 67;

    size_t shstrtab_off = data_seg_end;
    size_t shstrtab_end = shstrtab_off + shstrtab_size;

    uint16_t num_sections = 11; /* null + 10 named sections */
    size_t shdr_off = obj_align_up(shstrtab_end, 8);
    size_t total_size = shdr_off + (size_t)num_sections * 64;

    /* Virtual addresses */
    uint64_t interp_vaddr  = image_base + interp_off;
    uint64_t hash_vaddr    = image_base + hash_off;
    uint64_t dynsym_vaddr  = image_base + dynsym_off;
    uint64_t dynstr_vaddr  = image_base + dynstr_off;
    uint64_t text_vaddr    = image_base + text_off;
    uint64_t code_vaddr    = image_base + code_off;
    uint64_t tramp_vaddr   = image_base + tramp_off;
    uint64_t data_seg_vaddr = image_base + data_seg_off;
    uint64_t rela_dyn_vaddr = image_base + rela_dyn_off;
    uint64_t got_vaddr     = image_base + got_off;
    uint64_t user_data_vaddr = image_base + user_data_off;
    uint64_t dynamic_vaddr = image_base + dynamic_off;
    (void)text_vaddr;

    uint64_t entry_vaddr = image_base + text_off;

    /* Find the entry point symbol */
    const lr_obj_symbol_t *entry_sym = NULL;
    for (uint32_t i = 0; i < oc->num_symbols; i++) {
        const lr_obj_symbol_t *sym = &oc->symbols[i];
        if (sym->is_defined && sym->section == 1 &&
            strcmp(sym->name, entry_symbol) == 0) {
            entry_sym = sym;
            break;
        }
    }
    if (!entry_sym || (size_t)entry_sym->offset >= code_size) {
        free(dyn_names); free(dyn_oc_idx);
        free(sym_to_dynsym); free(dyn_name_off);
        free(int_got_slot_off);
        return -1;
    }

    /* Allocate output buffer and mutable code/data copies */
    uint8_t *buf = (uint8_t *)calloc(1, total_size);
    uint8_t *code_mut = (uint8_t *)malloc(code_size);
    size_t data_area_size = data_seg_end - data_seg_off;
    uint8_t *data_area = (uint8_t *)calloc(1, data_area_size > 0 ? data_area_size : 1);
    if (!buf || !code_mut || !data_area) {
        free(buf); free(code_mut); free(data_area);
        free(dyn_names); free(dyn_oc_idx);
        free(sym_to_dynsym); free(dyn_name_off);
        free(int_got_slot_off);
        return -1;
    }
    memcpy(code_mut, code, code_size);
    if (data && data_size > 0)
        memcpy(data_area + (user_data_off - data_seg_off), data, data_size);

    /* Apply relocations for defined symbols (same logic as static writer) */
    for (uint32_t i = 0; i < oc->num_relocs; i++) {
        const lr_obj_reloc_t *rel = &oc->relocs[i];
        if (rel->symbol_idx >= oc->num_symbols)
            goto fail;
        if ((size_t)rel->offset + 4 > code_size)
            goto fail;

        const lr_obj_symbol_t *sym = &oc->symbols[rel->symbol_idx];

        if (!sym->is_defined) {
            /* Undefined symbol: PC32/PLT32 will be redirected to trampoline below */
            if (rel->type == LR_RELOC_X86_64_PC32 ||
                rel->type == LR_RELOC_X86_64_PLT32) {
                /* Redirect call to trampoline for this dynamic import */
                uint32_t dsym = sym_to_dynsym[rel->symbol_idx];
                if (dsym == 0) goto fail;
                uint32_t undef_idx = dsym - 1;
                uint64_t tramp_target = tramp_vaddr + (uint64_t)undef_idx * 6;
                uint64_t place_vaddr = code_vaddr + rel->offset;
                if (patch_rel32_vaddr(code_mut, code_size, rel->offset,
                                      place_vaddr, tramp_target) != 0)
                    goto fail;
                continue;
            }
            if (rel->type == LR_RELOC_X86_64_GOTPCREL) {
                /* Undefined symbol with GOTPCREL: point to dynamic GOT slot */
                uint32_t dsym = sym_to_dynsym[rel->symbol_idx];
                if (dsym == 0) goto fail;
                uint32_t undef_idx = dsym - 1;
                uint64_t slot_vaddr = got_vaddr + (uint64_t)undef_idx * 8;
                uint64_t place_vaddr = code_vaddr + rel->offset;
                if (patch_rel32_vaddr(code_mut, code_size, rel->offset,
                                      place_vaddr, slot_vaddr) != 0)
                    goto fail;
                continue;
            }
            goto fail; /* unsupported reloc type for undefined symbol */
        }

        /* Defined symbol: resolve directly */
        uint64_t target_vaddr;
        if (sym->section == 1) {
            if ((size_t)sym->offset >= code_size) goto fail;
            target_vaddr = code_vaddr + sym->offset;
        } else if (sym->section == 2) {
            target_vaddr = user_data_vaddr + sym->offset;
        } else {
            goto fail;
        }

        uint64_t place_vaddr = code_vaddr + rel->offset;
        int rc = 0;
        switch (rel->type) {
        case LR_RELOC_X86_64_PC32:
        case LR_RELOC_X86_64_PLT32:
            rc = patch_rel32_vaddr(code_mut, code_size, rel->offset,
                                   place_vaddr, target_vaddr);
            break;
        case LR_RELOC_X86_64_GOTPCREL: {
            if (!int_got_slot_off ||
                int_got_slot_off[rel->symbol_idx] == UINT32_MAX) {
                rc = -1;
                break;
            }
            uint32_t slot_rel = int_got_slot_off[rel->symbol_idx];
            uint64_t slot_vaddr = data_seg_vaddr + slot_rel;
            if (write_u64_le(data_area, data_area_size, slot_rel,
                              target_vaddr) != 0) {
                rc = -1;
                break;
            }
            rc = patch_rel32_vaddr(code_mut, code_size, rel->offset,
                                   place_vaddr, slot_vaddr);
            break;
        }
        case LR_RELOC_X86_64_64:
            rc = write_u64_le(code_mut, code_size, rel->offset, target_vaddr);
            break;
        default:
            rc = -1;
            break;
        }
        if (rc != 0) goto fail;
    }

    /* Apply data relocations for defined symbols */
    for (uint32_t i = 0; i < oc->num_data_relocs; i++) {
        const lr_obj_reloc_t *rel = &oc->data_relocs[i];
        if (rel->symbol_idx >= oc->num_symbols ||
            rel->type != LR_RELOC_X86_64_64)
            goto fail;
        const lr_obj_symbol_t *sym = &oc->symbols[rel->symbol_idx];
        if (!sym->is_defined) goto fail;

        uint64_t target_vaddr;
        if (sym->section == 1)
            target_vaddr = code_vaddr + sym->offset;
        else if (sym->section == 2)
            target_vaddr = user_data_vaddr + sym->offset;
        else
            goto fail;

        uint32_t da_off = (uint32_t)(user_data_off - data_seg_off) + rel->offset;
        if ((size_t)da_off + 8 > data_area_size) goto fail;
        if (write_u64_le(data_area, data_area_size, da_off, target_vaddr) != 0)
            goto fail;
    }

    /* Build trampolines in the output buffer (after code) */
    {
        uint8_t *tp = buf + tramp_off;
        for (uint32_t i = 0; i < num_dynimport; i++) {
            uint64_t slot_va = got_vaddr + (uint64_t)i * 8;
            uint64_t tramp_ip = tramp_vaddr + (uint64_t)i * 6 + 6; /* RIP after jmp */
            int32_t disp = (int32_t)((int64_t)slot_va - (int64_t)tramp_ip);
            *tp++ = 0xFF; /* jmp qword [rip+disp32] */
            *tp++ = 0x25;
            tp[0] = (uint8_t)(disp);
            tp[1] = (uint8_t)(disp >> 8);
            tp[2] = (uint8_t)(disp >> 16);
            tp[3] = (uint8_t)(disp >> 24);
            tp += 4;
        }
    }

    /* -- Write .interp -- */
    memcpy(buf + interp_off, interp, interp_size);

    /* -- Write .hash -- */
    {
        uint8_t *hp = buf + hash_off;
        w32(&hp, nbucket);
        w32(&hp, nchain);
        /* bucket array: hash(name) % nbucket -> first dynsym index */
        uint32_t *buckets = (uint32_t *)calloc(nbucket, sizeof(uint32_t));
        uint32_t *chains = (uint32_t *)calloc(nchain, sizeof(uint32_t));
        if (!buckets || !chains) {
            free(buckets); free(chains);
            goto fail;
        }
        /* dynsym[0] is STN_UNDEF, hash chains for real symbols start at 1 */
        for (uint32_t i = 0; i < num_dynimport; i++) {
            uint32_t dsym_idx = i + 1;
            const char *name = dyn_names[i];
            uint32_t h = elf_sysv_hash(name) % nbucket;
            chains[dsym_idx] = buckets[h];
            buckets[h] = dsym_idx;
        }
        for (uint32_t i = 0; i < nbucket; i++)
            w32(&hp, buckets[i]);
        for (uint32_t i = 0; i < nchain; i++)
            w32(&hp, chains[i]);
        free(buckets);
        free(chains);
    }

    /* -- Write .dynsym -- */
    {
        uint8_t *sp = buf + dynsym_off;
        /* STN_UNDEF entry (24 zero bytes) */
        wpad(&sp, 24);
        for (uint32_t i = 0; i < num_dynimport; i++) {
            w32(&sp, dyn_name_off[i]);                          /* st_name */
            w8(&sp, ELF64_ST_INFO(STB_GLOBAL, STT_NOTYPE));      /* st_info */
            w8(&sp, 0);                                            /* st_other */
            w16(&sp, SHN_UNDEF);                                  /* st_shndx */
            w64(&sp, 0);                                           /* st_value */
            w64(&sp, 0);                                           /* st_size */
        }
    }

    /* -- Write .dynstr -- */
    {
        uint8_t *dp = buf + dynstr_off;
        *dp++ = 0; /* leading NUL */
        memcpy(dp, libc_name, sizeof(libc_name));
        dp += sizeof(libc_name);
        memcpy(dp, libm_name, sizeof(libm_name));
        dp += sizeof(libm_name);
        for (uint32_t i = 0; i < num_dynimport; i++) {
            const char *name = dyn_names[i];
            size_t slen = strlen(name) + 1;
            memcpy(dp, name, slen);
            dp += slen;
        }
    }

    /* -- Write .text (start stub + code) -- */
    memcpy(buf + text_off, start_stub_template, start_stub_size);
    memcpy(buf + code_off, code_mut, code_size);

    /* Patch _start call to entry symbol */
    if (patch_rel32_vaddr(buf, total_size, (uint32_t)(text_off + 1),
                          entry_vaddr + 1, code_vaddr + entry_sym->offset) != 0)
        goto fail;

    /* Patch _start call to exit trampoline */
    {
        uint64_t exit_tramp_va = tramp_vaddr + (uint64_t)exit_dyn_idx * 6;
        uint64_t exit_call_ip = entry_vaddr + exit_call_off;
        if (patch_rel32_vaddr(buf, total_size, (uint32_t)(text_off + exit_call_off),
                              exit_call_ip, exit_tramp_va) != 0)
            goto fail;
    }

    /* -- Write data segment sections into data_area, then copy -- */

    /* .rela.dyn */
    {
        uint8_t *rp = data_area + (rela_dyn_off - data_seg_off);
        for (uint32_t i = 0; i < num_dynimport; i++) {
            uint64_t got_slot = got_vaddr + (uint64_t)i * 8;
            uint32_t dsym_idx = i + 1;
            w64(&rp, got_slot);                                       /* r_offset */
            w64(&rp, ELF64_R_INFO(dsym_idx, R_X86_64_GLOB_DAT));    /* r_info */
            w64(&rp, 0);                                              /* r_addend */
        }
    }

    /* .got is zero-initialized (dynamic linker fills it); already calloc'd */

    /* .dynamic */
    {
        uint8_t *dp = data_area + (dynamic_off - data_seg_off);
        /* DT_NEEDED "libc.so.6" */
        w64(&dp, DT_NEEDED);  w64(&dp, libc_name_off);
        /* DT_NEEDED "libm.so.6" */
        w64(&dp, DT_NEEDED);  w64(&dp, libm_name_off);
        /* DT_HASH */
        w64(&dp, DT_HASH);    w64(&dp, hash_vaddr);
        /* DT_STRTAB */
        w64(&dp, DT_STRTAB);  w64(&dp, dynstr_vaddr);
        /* DT_SYMTAB */
        w64(&dp, DT_SYMTAB);  w64(&dp, dynsym_vaddr);
        /* DT_STRSZ */
        w64(&dp, DT_STRSZ);   w64(&dp, dynstr_size);
        /* DT_SYMENT */
        w64(&dp, DT_SYMENT);  w64(&dp, 24);
        /* DT_RELA */
        w64(&dp, DT_RELA);    w64(&dp, rela_dyn_vaddr);
        /* DT_RELASZ */
        w64(&dp, DT_RELASZ);  w64(&dp, rela_dyn_size);
        /* DT_RELAENT */
        w64(&dp, DT_RELAENT); w64(&dp, 24);
        /* DT_BIND_NOW */
        w64(&dp, DT_BIND_NOW); w64(&dp, 0);
        /* DT_FLAGS */
        w64(&dp, DT_FLAGS);   w64(&dp, DF_BIND_NOW);
        /* DT_FLAGS_1 */
        w64(&dp, DT_FLAGS_1); w64(&dp, DF_1_NOW);
        /* DT_NULL */
        w64(&dp, DT_NULL);    w64(&dp, 0);
    }

    /* Copy data segment into output buffer */
    memcpy(buf + data_seg_off, data_area, data_area_size);

    /* -- ELF header -- */
    {
        uint8_t *p = buf;
        w8(&p, ELFMAG0); w8(&p, ELFMAG1); w8(&p, ELFMAG2); w8(&p, ELFMAG3);
        w8(&p, ELFCLASS64);
        w8(&p, ELFDATA2LSB);
        w8(&p, EV_CURRENT);
        w8(&p, ELFOSABI_NONE);
        wpad(&p, 8);                    /* e_ident padding */
        w16(&p, ET_EXEC);               /* e_type */
        w16(&p, EM_X86_64);             /* e_machine */
        w32(&p, EV_CURRENT);            /* e_version */
        w64(&p, entry_vaddr);           /* e_entry */
        w64(&p, ehdr_size);             /* e_phoff */
        w64(&p, shdr_off);              /* e_shoff */
        w32(&p, 0);                      /* e_flags */
        w16(&p, 64);                     /* e_ehsize */
        w16(&p, 56);                     /* e_phentsize */
        w16(&p, num_phdrs);             /* e_phnum */
        w16(&p, 64);                     /* e_shentsize */
        w16(&p, num_sections);           /* e_shnum */
        w16(&p, num_sections - 1);       /* e_shstrndx = last section */
    }

    /* -- Program headers -- */
    {
        uint8_t *p = buf + ehdr_size;

        /* PT_INTERP */
        w32(&p, PT_INTERP);
        w32(&p, PF_R);
        w64(&p, interp_off);              /* p_offset */
        w64(&p, interp_vaddr);            /* p_vaddr */
        w64(&p, interp_vaddr);            /* p_paddr */
        w64(&p, interp_size);             /* p_filesz */
        w64(&p, interp_size);             /* p_memsz */
        w64(&p, 1);                        /* p_align */

        /* PT_LOAD: text segment (R+X) */
        w32(&p, PT_LOAD);
        w32(&p, PF_R | PF_X);
        w64(&p, 0);                        /* p_offset: from file start */
        w64(&p, image_base);              /* p_vaddr */
        w64(&p, image_base);              /* p_paddr */
        w64(&p, text_end);                /* p_filesz */
        w64(&p, text_end);                /* p_memsz */
        w64(&p, page_align);              /* p_align */

        /* PT_LOAD: data segment (R+W) */
        w32(&p, PT_LOAD);
        w32(&p, PF_R | PF_W);
        w64(&p, data_seg_off);            /* p_offset */
        w64(&p, data_seg_vaddr);          /* p_vaddr */
        w64(&p, data_seg_vaddr);          /* p_paddr */
        w64(&p, data_seg_end - data_seg_off); /* p_filesz */
        w64(&p, data_seg_end - data_seg_off); /* p_memsz */
        w64(&p, page_align);              /* p_align */

        /* PT_DYNAMIC */
        w32(&p, PT_DYNAMIC);
        w32(&p, PF_R | PF_W);
        w64(&p, dynamic_off);             /* p_offset */
        w64(&p, dynamic_vaddr);           /* p_vaddr */
        w64(&p, dynamic_vaddr);           /* p_paddr */
        w64(&p, dynamic_size);            /* p_filesz */
        w64(&p, dynamic_size);            /* p_memsz */
        w64(&p, 8);                        /* p_align */
    }

    /* -- Write .shstrtab -- */
    memcpy(buf + shstrtab_off, shstrtab_content, shstrtab_size);

    /* -- Section headers -- */
    {
        uint8_t *sh = buf + shdr_off;

        /* [0] SHT_NULL */
        wpad(&sh, 64);

        /* [1] .interp */
        w32(&sh, shn_interp);
        w32(&sh, SHT_PROGBITS);
        w64(&sh, SHF_ALLOC);
        w64(&sh, interp_vaddr);
        w64(&sh, interp_off);
        w64(&sh, interp_size);
        w32(&sh, 0); w32(&sh, 0);
        w64(&sh, 1); w64(&sh, 0);

        /* [2] .hash */
        w32(&sh, shn_hash);
        w32(&sh, SHT_HASH);
        w64(&sh, SHF_ALLOC);
        w64(&sh, hash_vaddr);
        w64(&sh, hash_off);
        w64(&sh, hash_size);
        w32(&sh, 3); /* sh_link = .dynsym section index */
        w32(&sh, 0);
        w64(&sh, 4); w64(&sh, 4);

        /* [3] .dynsym */
        w32(&sh, shn_dynsym);
        w32(&sh, SHT_DYNSYM);
        w64(&sh, SHF_ALLOC);
        w64(&sh, dynsym_vaddr);
        w64(&sh, dynsym_off);
        w64(&sh, dynsym_size);
        w32(&sh, 4); /* sh_link = .dynstr section index */
        w32(&sh, 1); /* sh_info = index of first non-local symbol */
        w64(&sh, 8); w64(&sh, 24);

        /* [4] .dynstr */
        w32(&sh, shn_dynstr);
        w32(&sh, SHT_STRTAB);
        w64(&sh, SHF_ALLOC);
        w64(&sh, dynstr_vaddr);
        w64(&sh, dynstr_off);
        w64(&sh, dynstr_size);
        w32(&sh, 0); w32(&sh, 0);
        w64(&sh, 1); w64(&sh, 0);

        /* [5] .text */
        w32(&sh, shn_text);
        w32(&sh, SHT_PROGBITS);
        w64(&sh, SHF_ALLOC | SHF_EXECINSTR);
        w64(&sh, image_base + text_off);
        w64(&sh, text_off);
        w64(&sh, text_end - text_off);
        w32(&sh, 0); w32(&sh, 0);
        w64(&sh, 16); w64(&sh, 0);

        /* [6] .rela.dyn */
        w32(&sh, shn_reladyn);
        w32(&sh, SHT_RELA);
        w64(&sh, SHF_ALLOC);
        w64(&sh, rela_dyn_vaddr);
        w64(&sh, rela_dyn_off);
        w64(&sh, rela_dyn_size);
        w32(&sh, 3); /* sh_link = .dynsym section index */
        w32(&sh, 0);
        w64(&sh, 8); w64(&sh, 24);

        /* [7] .got */
        w32(&sh, shn_got);
        w32(&sh, SHT_PROGBITS);
        w64(&sh, SHF_WRITE | SHF_ALLOC);
        w64(&sh, got_vaddr);
        w64(&sh, got_off);
        w64(&sh, got_size + extra_got_size);
        w32(&sh, 0); w32(&sh, 0);
        w64(&sh, 8); w64(&sh, 8);

        /* [8] .data */
        w32(&sh, shn_data);
        w32(&sh, SHT_PROGBITS);
        w64(&sh, SHF_WRITE | SHF_ALLOC);
        w64(&sh, user_data_vaddr);
        w64(&sh, user_data_off);
        w64(&sh, data_size);
        w32(&sh, 0); w32(&sh, 0);
        w64(&sh, 8); w64(&sh, 0);

        /* [9] .dynamic */
        w32(&sh, shn_dynamic);
        w32(&sh, SHT_DYNAMIC);
        w64(&sh, SHF_WRITE | SHF_ALLOC);
        w64(&sh, dynamic_vaddr);
        w64(&sh, dynamic_off);
        w64(&sh, dynamic_size);
        w32(&sh, 4); /* sh_link = .dynstr section index */
        w32(&sh, 0);
        w64(&sh, 8); w64(&sh, 16);

        /* [10] .shstrtab */
        w32(&sh, shn_shstrtab);
        w32(&sh, SHT_STRTAB);
        w64(&sh, 0);
        w64(&sh, 0);
        w64(&sh, shstrtab_off);
        w64(&sh, shstrtab_size);
        w32(&sh, 0); w32(&sh, 0);
        w64(&sh, 1); w64(&sh, 0);
    }

    size_t written = fwrite(buf, 1, total_size, out);
    free(buf);
    free(code_mut);
    free(data_area);
    free(dyn_names);
    free(dyn_oc_idx);
    free(sym_to_dynsym);
    free(dyn_name_off);
    free(int_got_slot_off);
    return written == total_size ? 0 : -1;

fail:
    free(buf);
    free(code_mut);
    free(data_area);
    free(dyn_names);
    free(dyn_oc_idx);
    free(sym_to_dynsym);
    free(dyn_name_off);
    free(int_got_slot_off);
    return -1;
}

int write_elf_executable_aarch64(FILE *out, const uint8_t *code, size_t code_size,
                                 const uint8_t *data, size_t data_size,
                                 const lr_objfile_ctx_t *oc,
                                 const char *entry_symbol) {
    if (!out || !code || !oc || !entry_symbol || !entry_symbol[0])
        return -1;
    if (oc->num_data_relocs != 0)
        return -1;

    const uint64_t image_base = 0x400000ULL;
    const size_t ehdr_size = 64;
    const size_t phdr_size = 56;
    const size_t file_align = 16;
    const size_t page_align = 4096;

    /* _start:
     *   bl <entry_symbol>
     *   mov x8, #93
     *   svc #0
     * Returns from entry are already in w0.
     */
    static const uint8_t start_stub_template[] = {
        0x00, 0x00, 0x00, 0x94,
        0xA8, 0x0B, 0x80, 0xD2,
        0x01, 0x00, 0x00, 0xD4
    };
    const size_t start_stub_size = sizeof(start_stub_template);

    uint32_t *got_slot_off = NULL;
    if (oc->num_symbols > 0) {
        got_slot_off = (uint32_t *)malloc(sizeof(uint32_t) * oc->num_symbols);
        if (!got_slot_off)
            return -1;
        for (uint32_t i = 0; i < oc->num_symbols; i++)
            got_slot_off[i] = UINT32_MAX;
    }

    size_t data_runtime_size = data_size;
    data_runtime_size = obj_align_up(data_runtime_size, 8);
    for (uint32_t i = 0; i < oc->num_relocs; i++) {
        const lr_obj_reloc_t *rel = &oc->relocs[i];
        if (rel->type != LR_RELOC_ARM64_GOT_LOAD_PAGE21 &&
            rel->type != LR_RELOC_ARM64_GOT_LOAD_PAGEOFF12)
            continue;
        if (rel->symbol_idx >= oc->num_symbols) {
            free(got_slot_off);
            return -1;
        }
        if (got_slot_off[rel->symbol_idx] != UINT32_MAX)
            continue;
        if (data_runtime_size > UINT32_MAX - 8u) {
            free(got_slot_off);
            return -1;
        }
        got_slot_off[rel->symbol_idx] = (uint32_t)data_runtime_size;
        data_runtime_size += 8u;
    }

    size_t text_off = obj_align_up(ehdr_size + phdr_size, file_align);
    size_t code_off = text_off + start_stub_size;
    size_t data_off = obj_align_up(code_off + code_size, file_align);
    size_t total_size = data_off + data_runtime_size;

    uint64_t entry_vaddr = image_base + text_off;
    uint64_t code_vaddr = image_base + code_off;
    uint64_t data_vaddr = image_base + data_off;

    const lr_obj_symbol_t *entry_sym = NULL;
    for (uint32_t i = 0; i < oc->num_symbols; i++) {
        const lr_obj_symbol_t *sym = &oc->symbols[i];
        if (sym->is_defined && sym->section == 1 &&
            strcmp(sym->name, entry_symbol) == 0) {
            entry_sym = sym;
            break;
        }
    }
    if (!entry_sym || (size_t)entry_sym->offset >= code_size) {
        free(got_slot_off);
        return -1;
    }

    uint8_t *buf = (uint8_t *)calloc(1, total_size);
    uint8_t *code_mut = (uint8_t *)malloc(code_size);
    uint8_t *data_mut = (uint8_t *)calloc(1, data_runtime_size > 0 ? data_runtime_size : 1u);
    if (!buf || !code_mut || !data_mut) {
        free(buf);
        free(code_mut);
        free(data_mut);
        free(got_slot_off);
        return -1;
    }
    memcpy(code_mut, code, code_size);
    if (data && data_size > 0)
        memcpy(data_mut, data, data_size);

    for (uint32_t i = 0; i < oc->num_relocs; i++) {
        const lr_obj_reloc_t *rel = &oc->relocs[i];
        if (rel->symbol_idx >= oc->num_symbols) {
            free(buf);
            free(code_mut);
            free(data_mut);
            free(got_slot_off);
            return -1;
        }
        if ((size_t)rel->offset + 4 > code_size) {
            free(buf);
            free(code_mut);
            free(data_mut);
            free(got_slot_off);
            return -1;
        }

        const lr_obj_symbol_t *sym = &oc->symbols[rel->symbol_idx];
        if (!sym->is_defined) {
            free(buf);
            free(code_mut);
            free(data_mut);
            free(got_slot_off);
            return -1;
        }

        uint64_t target_vaddr;
        if (sym->section == 1) {
            if ((size_t)sym->offset >= code_size) {
                free(buf);
                free(code_mut);
                free(data_mut);
                free(got_slot_off);
                return -1;
            }
            target_vaddr = code_vaddr + sym->offset;
        } else if (sym->section == 2) {
            if ((size_t)sym->offset >= data_runtime_size) {
                free(buf);
                free(code_mut);
                free(data_mut);
                free(got_slot_off);
                return -1;
            }
            target_vaddr = data_vaddr + sym->offset;
        } else {
            free(buf);
            free(code_mut);
            free(data_mut);
            free(got_slot_off);
            return -1;
        }

        uint64_t place_vaddr = code_vaddr + rel->offset;
        int rc = 0;
        switch (rel->type) {
        case LR_RELOC_ARM64_BRANCH26:
            rc = patch_aarch64_branch26_vaddr(code_mut, code_size, rel->offset,
                                              place_vaddr, target_vaddr);
            break;
        case LR_RELOC_ARM64_PAGE21:
            rc = patch_aarch64_page21_vaddr(code_mut, code_size, rel->offset,
                                            place_vaddr, target_vaddr);
            break;
        case LR_RELOC_ARM64_PAGEOFF12:
            rc = patch_aarch64_pageoff12_vaddr(code_mut, code_size, rel->offset,
                                               target_vaddr, false);
            break;
        case LR_RELOC_ARM64_GOT_LOAD_PAGE21:
        case LR_RELOC_ARM64_GOT_LOAD_PAGEOFF12: {
            if (!got_slot_off || rel->symbol_idx >= oc->num_symbols ||
                got_slot_off[rel->symbol_idx] == UINT32_MAX) {
                rc = -1;
                break;
            }
            uint32_t slot_off = got_slot_off[rel->symbol_idx];
            uint64_t slot_vaddr = data_vaddr + (uint64_t)slot_off;
            if (write_u64_le(data_mut, data_runtime_size, slot_off, target_vaddr) != 0) {
                rc = -1;
                break;
            }
            if (rel->type == LR_RELOC_ARM64_GOT_LOAD_PAGE21) {
                rc = patch_aarch64_page21_vaddr(code_mut, code_size, rel->offset,
                                                place_vaddr, slot_vaddr);
            } else {
                rc = patch_aarch64_pageoff12_vaddr(code_mut, code_size, rel->offset,
                                                   slot_vaddr, true);
            }
            break;
        }
        default:
            rc = -1;
            break;
        }
        if (rc != 0) {
            free(buf);
            free(code_mut);
            free(data_mut);
            free(got_slot_off);
            return -1;
        }
    }

    uint8_t *p = buf;
    w8(&p, ELFMAG0); w8(&p, ELFMAG1); w8(&p, ELFMAG2); w8(&p, ELFMAG3);
    w8(&p, ELFCLASS64);
    w8(&p, ELFDATA2LSB);
    w8(&p, EV_CURRENT);
    w8(&p, ELFOSABI_NONE);
    wpad(&p, 8);
    w16(&p, ET_EXEC);
    w16(&p, EM_AARCH64);
    w32(&p, EV_CURRENT);
    w64(&p, entry_vaddr);
    w64(&p, ehdr_size);
    w64(&p, 0);
    w32(&p, 0);
    w16(&p, 64);
    w16(&p, 56);
    w16(&p, 1);
    w16(&p, 0);
    w16(&p, 0);
    w16(&p, 0);

    w32(&p, PT_LOAD);
    w32(&p, PF_R | PF_W | PF_X);
    w64(&p, 0);
    w64(&p, image_base);
    w64(&p, image_base);
    w64(&p, total_size);
    w64(&p, total_size);
    w64(&p, page_align);

    memcpy(buf + text_off, start_stub_template, start_stub_size);
    memcpy(buf + code_off, code_mut, code_size);
    if (data_runtime_size > 0)
        memcpy(buf + data_off, data_mut, data_runtime_size);

    if (patch_aarch64_branch26_vaddr(buf, total_size, (uint32_t)text_off,
                                     entry_vaddr, code_vaddr + entry_sym->offset) != 0) {
        free(buf);
        free(code_mut);
        free(data_mut);
        free(got_slot_off);
        return -1;
    }

    size_t written = fwrite(buf, 1, total_size, out);
    free(buf);
    free(code_mut);
    free(data_mut);
    free(got_slot_off);
    return written == total_size ? 0 : -1;
}

int write_elf_executable_riscv64(FILE *out, const uint8_t *code, size_t code_size,
                                 const uint8_t *data, size_t data_size,
                                 const lr_objfile_ctx_t *oc,
                                 const char *entry_symbol) {
    if (!out || !code || !oc || !entry_symbol || !entry_symbol[0])
        return -1;

    if (oc->num_relocs != 0 || oc->num_data_relocs != 0)
        return -1;

    const uint64_t image_base = 0x400000ULL;
    const size_t ehdr_size = 64;
    const size_t phdr_size = 56;
    const size_t file_align = 16;
    const size_t page_align = 4096;

    /* _start:
     *   jal ra, <entry_symbol>
     *   addi a7, x0, 93
     *   ecall
     */
    static const uint8_t start_stub_template[] = {
        0xEF, 0x00, 0x00, 0x00,
        0x93, 0x08, 0xD0, 0x05,
        0x73, 0x00, 0x00, 0x00
    };
    const size_t start_stub_size = sizeof(start_stub_template);

    size_t text_off = obj_align_up(ehdr_size + phdr_size, file_align);
    size_t code_off = text_off + start_stub_size;
    size_t data_off = obj_align_up(code_off + code_size, file_align);
    size_t total_size = data_off + data_size;

    uint64_t entry_vaddr = image_base + text_off;
    uint64_t code_vaddr = image_base + code_off;

    const lr_obj_symbol_t *entry_sym = NULL;
    for (uint32_t i = 0; i < oc->num_symbols; i++) {
        const lr_obj_symbol_t *sym = &oc->symbols[i];
        if (sym->is_defined && sym->section == 1 &&
            strcmp(sym->name, entry_symbol) == 0) {
            entry_sym = sym;
            break;
        }
    }
    if (!entry_sym || (size_t)entry_sym->offset >= code_size)
        return -1;

    uint8_t *buf = (uint8_t *)calloc(1, total_size);
    uint8_t *code_mut = (uint8_t *)malloc(code_size);
    if (!buf || !code_mut) {
        free(buf);
        free(code_mut);
        return -1;
    }
    memcpy(code_mut, code, code_size);

    uint8_t *p = buf;
    w8(&p, ELFMAG0); w8(&p, ELFMAG1); w8(&p, ELFMAG2); w8(&p, ELFMAG3);
    w8(&p, ELFCLASS64);
    w8(&p, ELFDATA2LSB);
    w8(&p, EV_CURRENT);
    w8(&p, ELFOSABI_NONE);
    wpad(&p, 8);
    w16(&p, ET_EXEC);
    w16(&p, EM_RISCV);
    w32(&p, EV_CURRENT);
    w64(&p, entry_vaddr);
    w64(&p, ehdr_size);
    w64(&p, 0);
    w32(&p, 0);
    w16(&p, 64);
    w16(&p, 56);
    w16(&p, 1);
    w16(&p, 0);
    w16(&p, 0);
    w16(&p, 0);

    w32(&p, PT_LOAD);
    w32(&p, PF_R | PF_W | PF_X);
    w64(&p, 0);
    w64(&p, image_base);
    w64(&p, image_base);
    w64(&p, total_size);
    w64(&p, total_size);
    w64(&p, page_align);

    memcpy(buf + text_off, start_stub_template, start_stub_size);
    memcpy(buf + code_off, code_mut, code_size);
    if (data_size > 0 && data)
        memcpy(buf + data_off, data, data_size);

    if (patch_riscv_jal_vaddr(buf, total_size, (uint32_t)text_off,
                              entry_vaddr, code_vaddr + entry_sym->offset) != 0) {
        free(buf);
        free(code_mut);
        return -1;
    }

    size_t written = fwrite(buf, 1, total_size, out);
    free(buf);
    free(code_mut);
    return written == total_size ? 0 : -1;
}
