#include "objfile_macho.h"
#include <stdlib.h>
#include <string.h>

#define MH_MAGIC_64     0xFEEDFACFu
#define MH_OBJECT       0x1u
#define CPU_TYPE_ARM64  0x0100000Cu
#define CPU_SUBTYPE_ALL 0x00000000u
#define MH_SUBSECTIONS_VIA_SYMBOLS 0x00002000u

#define LC_SEGMENT_64   0x19u
#define LC_SYMTAB       0x02u
#define LC_BUILD_VERSION 0x32u

#define S_REGULAR           0x0u
#define S_ATTR_PURE_INSTRUCTIONS 0x80000000u
#define S_ATTR_SOME_INSTRUCTIONS 0x00000400u

#define N_EXT   0x1u
#define N_SECT  0xEu

#define PLATFORM_MACOS 1u

lr_reloc_mapped_t macho_reloc_arm64(uint8_t liric_type) {
    lr_reloc_mapped_t m = {0};
    m.native_type = liric_type;
    switch (liric_type) {
    case LR_RELOC_ARM64_BRANCH26:
    case LR_RELOC_ARM64_PAGE21:
    case LR_RELOC_ARM64_GOT_LOAD_PAGE21:
        m.is_pcrel = true;
        break;
    default:
        m.is_pcrel = false;
        break;
    }
    return m;
}

int write_macho(FILE *out, const uint8_t *code, size_t code_size,
                const uint8_t *data, size_t data_size,
                const lr_objfile_ctx_t *oc,
                uint32_t cpu_type, lr_reloc_mapper_fn reloc_mapper) {
    uint32_t n_defined = 0;
    for (uint32_t i = 0; i < oc->num_symbols; i++) {
        if (oc->symbols[i].is_defined)
            n_defined++;
    }

    uint32_t *sym_remap = calloc(oc->num_symbols, sizeof(uint32_t));
    uint32_t *sym_order = calloc(oc->num_symbols, sizeof(uint32_t));
    if (!sym_remap || !sym_order) {
        free(sym_remap);
        free(sym_order);
        return -1;
    }
    uint32_t di = 0;
    uint32_t ui = n_defined;
    for (uint32_t i = 0; i < oc->num_symbols; i++) {
        if (oc->symbols[i].is_defined) {
            sym_remap[i] = di;
            sym_order[di] = i;
            di++;
        } else {
            sym_remap[i] = ui;
            sym_order[ui] = i;
            ui++;
        }
    }

    size_t strtab_size = 1;
    uint32_t *str_offsets = calloc(oc->num_symbols, sizeof(uint32_t));
    if (!str_offsets) {
        free(sym_remap);
        free(sym_order);
        return -1;
    }
    for (uint32_t i = 0; i < oc->num_symbols; i++) {
        str_offsets[i] = (uint32_t)strtab_size;
        strtab_size += 1 + strlen(oc->symbols[i].name) + 1;
    }
    uint8_t *strtab = calloc(1, strtab_size);
    if (!strtab) {
        free(str_offsets);
        free(sym_remap);
        free(sym_order);
        return -1;
    }
    strtab[0] = 0;
    for (uint32_t i = 0; i < oc->num_symbols; i++) {
        size_t slen = strlen(oc->symbols[i].name);
        strtab[str_offsets[i]] = '_';
        memcpy(strtab + str_offsets[i] + 1, oc->symbols[i].name, slen);
    }

    bool has_data = data_size > 0;
    uint32_t num_sections = has_data ? 2 : 1;

    size_t header_size = 32;
    size_t segment_cmd_size = 72 + 80 * num_sections;
    size_t symtab_cmd_size = 24;
    size_t build_version_cmd_size = 24;

    uint32_t ncmds = 3;
    size_t sizeofcmds = segment_cmd_size + symtab_cmd_size
                      + build_version_cmd_size;

    size_t section_data_off = header_size + sizeofcmds;
    size_t text_file_off = section_data_off;
    size_t text_size = code_size;

    size_t data_align = 8;
    size_t data_file_off = obj_align_up(text_file_off + text_size, data_align);
    size_t data_pad = data_file_off - (text_file_off + text_size);
    size_t data_vmaddr = has_data ? obj_align_up(text_size, data_align) : 0;

    size_t reloc_off = data_file_off + (has_data ? data_size : 0);
    size_t reloc_size = oc->num_relocs * 8;

    size_t symtab_off = reloc_off + reloc_size;
    size_t symtab_entries_size = oc->num_symbols * 16;

    size_t strtab_off = symtab_off + symtab_entries_size;

    size_t total_size = strtab_off + strtab_size;

    uint8_t *buf = calloc(1, total_size);
    if (!buf) {
        free(strtab);
        free(str_offsets);
        free(sym_remap);
        free(sym_order);
        return -1;
    }
    uint8_t *p = buf;

    /* mach_header_64 */
    w32(&p, MH_MAGIC_64);
    w32(&p, cpu_type);
    w32(&p, CPU_SUBTYPE_ALL);
    w32(&p, MH_OBJECT);
    w32(&p, ncmds);
    w32(&p, (uint32_t)sizeofcmds);
    w32(&p, MH_SUBSECTIONS_VIA_SYMBOLS);
    w32(&p, 0);

    /* LC_SEGMENT_64 */
    w32(&p, LC_SEGMENT_64);
    w32(&p, (uint32_t)segment_cmd_size);
    wpad(&p, 16);
    size_t seg_vmsize = has_data ? (data_vmaddr + data_size) : text_size;
    size_t seg_filesize = text_size + (has_data ? (data_pad + data_size) : 0);
    w64(&p, 0);
    w64(&p, seg_vmsize);
    w64(&p, text_file_off);
    w64(&p, seg_filesize);
    w32(&p, 7);
    w32(&p, 7);
    w32(&p, num_sections);
    w32(&p, 0);

    /* section_64: __text */
    {
        char sectname[16] = {0};
        char segname[16] = {0};
        memcpy(sectname, "__text", 6);
        memcpy(segname, "__TEXT", 6);
        wbytes(&p, sectname, 16);
        wbytes(&p, segname, 16);
        w64(&p, 0);
        w64(&p, text_size);
        w32(&p, (uint32_t)text_file_off);
        w32(&p, 2);
        w32(&p, (uint32_t)reloc_off);
        w32(&p, oc->num_relocs);
        w32(&p, S_REGULAR | S_ATTR_PURE_INSTRUCTIONS
               | S_ATTR_SOME_INSTRUCTIONS);
        w32(&p, 0);
        w32(&p, 0);
        w32(&p, 0);
    }

    /* section_64: __data (optional) */
    if (has_data) {
        char sectname[16] = {0};
        char segname[16] = {0};
        memcpy(sectname, "__data", 6);
        memcpy(segname, "__DATA", 6);
        wbytes(&p, sectname, 16);
        wbytes(&p, segname, 16);
        w64(&p, data_vmaddr);
        w64(&p, data_size);
        w32(&p, (uint32_t)data_file_off);
        w32(&p, 3);
        w32(&p, 0);
        w32(&p, 0);
        w32(&p, S_REGULAR);
        w32(&p, 0);
        w32(&p, 0);
        w32(&p, 0);
    }

    /* LC_SYMTAB */
    w32(&p, LC_SYMTAB);
    w32(&p, (uint32_t)symtab_cmd_size);
    w32(&p, (uint32_t)symtab_off);
    w32(&p, oc->num_symbols);
    w32(&p, (uint32_t)strtab_off);
    w32(&p, (uint32_t)strtab_size);

    /* LC_BUILD_VERSION */
    w32(&p, LC_BUILD_VERSION);
    w32(&p, (uint32_t)build_version_cmd_size);
    w32(&p, PLATFORM_MACOS);
    w32(&p, (14 << 16));
    w32(&p, (14 << 16));
    w32(&p, 0);

    /* Section data: __text */
    memcpy(buf + text_file_off, code, code_size);

    /* Section data: __data */
    if (has_data && data)
        memcpy(buf + data_file_off, data, data_size);

    /* Relocation entries (Mach-O relocation_info: 8 bytes) */
    {
        uint8_t *rp = buf + reloc_off;
        for (uint32_t i = 0; i < oc->num_relocs; i++) {
            const lr_obj_reloc_t *r = &oc->relocs[i];
            uint32_t r_address = r->offset;
            uint32_t mapped_sym = sym_remap[r->symbol_idx];

            lr_reloc_mapped_t mapped = reloc_mapper(r->type);

            uint32_t packed = (mapped_sym & 0x00FFFFFFu)
                            | ((mapped.is_pcrel ? 1u : 0u) << 24)
                            | (2u << 25)
                            | (1u << 27)
                            | ((mapped.native_type & 0xFu) << 28);

            w32(&rp, r_address);
            w32(&rp, packed);
        }
    }

    /* nlist_64 entries (16 bytes each), ordered: defined then undefined */
    {
        uint8_t *sp = buf + symtab_off;
        for (uint32_t oi = 0; oi < oc->num_symbols; oi++) {
            uint32_t orig_idx = sym_order[oi];
            const lr_obj_symbol_t *sym = &oc->symbols[orig_idx];

            w32(&sp, str_offsets[orig_idx]);

            if (sym->is_defined) {
                w8(&sp, N_SECT | N_EXT);
                w8(&sp, sym->section);
            } else {
                w8(&sp, N_EXT);
                w8(&sp, 0);
            }

            w16(&sp, 0);
            if (sym->is_defined) {
                uint64_t value = (uint64_t)sym->offset;
                if (sym->section == 2)
                    value += data_vmaddr;
                w64(&sp, value);
            } else {
                w64(&sp, 0);
            }
        }
    }

    /* String table */
    memcpy(buf + strtab_off, strtab, strtab_size);

    size_t written = fwrite(buf, 1, total_size, out);

    free(buf);
    free(strtab);
    free(str_offsets);
    free(sym_remap);
    free(sym_order);

    return written == total_size ? 0 : -1;
}
