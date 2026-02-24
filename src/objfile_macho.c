#include "objfile_macho.h"
#include <stdlib.h>
#include <string.h>

#define MH_MAGIC_64     0xFEEDFACFu
#define MH_OBJECT       0x1u
#define MH_EXECUTE      0x2u
#define CPU_TYPE_ARM64  0x0100000Cu
#define CPU_SUBTYPE_ALL 0x00000000u
#define MH_SUBSECTIONS_VIA_SYMBOLS 0x00002000u
#define MH_NOUNDEFS     0x00000001u
#define MH_DYLDLINK     0x00000004u
#define MH_TWOLEVEL     0x00000080u
#define MH_PIE          0x00200000u

#define LC_SEGMENT_64   0x19u
#define LC_SYMTAB       0x02u
#define LC_DYSYMTAB     0x0Bu
#define LC_LOAD_DYLIB   0x0Cu
#define LC_LOAD_DYLINKER 0x0Eu
#define LC_UUID         0x1Bu
#define LC_FUNCTION_STARTS 0x26u
#define LC_DATA_IN_CODE 0x29u
#define LC_SOURCE_VERSION 0x2Au
#define LC_BUILD_VERSION 0x32u
#define LC_MAIN         0x80000028u
#define LC_DYLD_EXPORTS_TRIE 0x80000033u
#define LC_DYLD_CHAINED_FIXUPS 0x80000034u

#define S_REGULAR           0x0u
#define S_ATTR_PURE_INSTRUCTIONS 0x80000000u
#define S_ATTR_SOME_INSTRUCTIONS 0x00000400u

#define N_EXT   0x1u
#define N_SECT  0xEu

#define PLATFORM_MACOS 1u
#define TOOL_LD 3u

static size_t append_uleb128(uint8_t *buf, size_t cap, uint64_t value) {
    size_t n = 0;
    if (!buf || cap == 0)
        return 0;
    do {
        uint8_t byte = (uint8_t)(value & 0x7Fu);
        value >>= 7;
        if (value != 0)
            byte |= 0x80u;
        if (n >= cap)
            return 0;
        buf[n++] = byte;
    } while (value != 0);
    return n;
}

lr_reloc_mapped_t macho_reloc_arm64(uint8_t liric_type) {
    lr_reloc_mapped_t m = {0};
    switch (liric_type) {
    case LR_RELOC_ARM64_ABS64:
        m.native_type = 0; /* ARM64_RELOC_UNSIGNED */
        m.is_pcrel = false;
        break;
    case LR_RELOC_ARM64_BRANCH26:
    case LR_RELOC_ARM64_PAGE21:
    case LR_RELOC_ARM64_GOT_LOAD_PAGE21:
        m.native_type = liric_type;
        m.is_pcrel = true;
        break;
    default:
        m.native_type = liric_type;
        m.is_pcrel = false;
        break;
    }
    return m;
}

/* Compute r_length for a Mach-O relocation from the native type. */
static uint32_t macho_reloc_length(uint8_t liric_type) {
    if (liric_type == LR_RELOC_ARM64_ABS64)
        return 3; /* 8-byte */
    return 2; /* 4-byte (default for code relocs) */
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

    size_t text_reloc_off = data_file_off + (has_data ? data_size : 0);
    size_t text_reloc_size = oc->num_relocs * 8;

    size_t data_reloc_off = text_reloc_off + text_reloc_size;
    size_t data_reloc_size = oc->num_data_relocs * 8;

    size_t symtab_off = data_reloc_off + data_reloc_size;
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
        w32(&p, (uint32_t)text_reloc_off);
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
        w32(&p, oc->num_data_relocs > 0
                ? (uint32_t)data_reloc_off : 0);
        w32(&p, oc->num_data_relocs);
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

    /* Text relocation entries (Mach-O relocation_info: 8 bytes) */
    {
        uint8_t *rp = buf + text_reloc_off;
        for (uint32_t i = 0; i < oc->num_relocs; i++) {
            const lr_obj_reloc_t *r = &oc->relocs[i];
            uint32_t r_address = r->offset;
            uint32_t mapped_sym = sym_remap[r->symbol_idx];

            lr_reloc_mapped_t mapped = reloc_mapper(r->type);
            uint32_t r_length = macho_reloc_length(r->type);

            uint32_t packed = (mapped_sym & 0x00FFFFFFu)
                            | ((mapped.is_pcrel ? 1u : 0u) << 24)
                            | (r_length << 25)
                            | (1u << 27)
                            | ((mapped.native_type & 0xFu) << 28);

            w32(&rp, r_address);
            w32(&rp, packed);
        }
    }

    /* Data relocation entries */
    {
        uint8_t *rp = buf + data_reloc_off;
        for (uint32_t i = 0; i < oc->num_data_relocs; i++) {
            const lr_obj_reloc_t *r = &oc->data_relocs[i];
            uint32_t r_address = r->offset;
            uint32_t mapped_sym = sym_remap[r->symbol_idx];

            lr_reloc_mapped_t mapped = reloc_mapper(r->type);
            uint32_t r_length = macho_reloc_length(r->type);

            uint32_t packed = (mapped_sym & 0x00FFFFFFu)
                            | ((mapped.is_pcrel ? 1u : 0u) << 24)
                            | (r_length << 25)
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
                w8(&sp, sym->is_local ? N_SECT : (N_SECT | N_EXT));
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

int write_macho_executable_arm64(FILE *out,
                                 const uint8_t *code, size_t code_size,
                                 const uint8_t *data, size_t data_size,
                                 const lr_objfile_ctx_t *oc,
                                 const char *entry_symbol) {
    static const uint8_t fixups_blob[56] = {
        0x00,0x00,0x00,0x00, 0x20,0x00,0x00,0x00, 0x30,0x00,0x00,0x00,
        0x30,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x01,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x03,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00
    };
    static const char dyld_path[] = "/usr/lib/dyld";
    static const char libsystem_path[] = "/usr/lib/libSystem.B.dylib";
    const uint64_t image_base = 0x100000000ULL;
    const size_t page = 0x4000u;
    const uint32_t ncmds = 15;
    const uint32_t sizeofcmds = 648;
    const size_t code_sig_cmd_slack = 16u;
    const size_t header_and_cmds = 32u + (size_t)sizeofcmds + code_sig_cmd_slack;
    const size_t text_off = obj_align_up(header_and_cmds, 8);
    const size_t text_file_size = obj_align_up(text_off + code_size, page);
    const size_t linkedit_off = text_file_size;
    size_t fixups_off = linkedit_off;
    size_t exports_off = 0;
    size_t func_starts_off = 0;
    size_t symtab_off = 0;
    size_t strtab_off = 0;
    size_t linkedit_size = 0;
    size_t total_size = 0;
    const lr_obj_symbol_t *entry = NULL;
    uint8_t exports_blob[64];
    size_t exports_size = 0;
    uint8_t func_starts_blob[16];
    size_t func_starts_size = 0;
    uint8_t symtab_blob[32];
    uint8_t strtab_blob[32];
    uint8_t *buf = NULL;
    uint8_t *p = NULL;
    size_t written;
    uint64_t entry_off = 0;
    uint64_t entry_addr = 0;

    if (!out || !code || !oc || !entry_symbol || !entry_symbol[0])
        return -1;
    if (data != NULL || data_size != 0)
        return -1;

    for (uint32_t i = 0; i < oc->num_symbols; i++) {
        const lr_obj_symbol_t *sym = &oc->symbols[i];
        if (sym->is_defined && sym->section == 1 &&
            strcmp(sym->name, entry_symbol) == 0) {
            entry = sym;
            break;
        }
    }
    if (!entry || (size_t)entry->offset >= code_size)
        return -1;

    entry_off = text_off + (uint64_t)entry->offset;
    entry_addr = image_base + entry_off;

    memset(exports_blob, 0, sizeof(exports_blob));
    {
        size_t ep = 0;
        size_t n = 0;
        exports_blob[ep++] = 0x00;
        exports_blob[ep++] = 0x01;
        exports_blob[ep++] = '_';
        exports_blob[ep++] = 0x00;
        exports_blob[ep++] = 0x12;
        exports_blob[ep++] = 0x00;
        exports_blob[ep++] = 0x00;
        exports_blob[ep++] = 0x00;
        exports_blob[ep++] = 0x00;
        exports_blob[ep++] = 0x02;
        exports_blob[ep++] = 0x00;
        exports_blob[ep++] = 0x00;
        exports_blob[ep++] = 0x00;
        exports_blob[ep++] = 0x03;
        exports_blob[ep++] = 0x00;
        n = append_uleb128(exports_blob + ep, sizeof(exports_blob) - ep,
                           entry_off);
        if (n == 0)
            return -1;
        ep += n;
        exports_blob[ep++] = 0x00;
        exports_blob[ep++] = 0x00;
        exports_blob[ep++] = 0x02;
        memcpy(exports_blob + ep, "_mh_execute_header", 18);
        ep += 18;
        exports_blob[ep++] = 0x09;
        memcpy(exports_blob + ep, "main", 5);
        ep += 5;
        exports_blob[ep++] = 0x0D;
        exports_blob[ep++] = 0x00;
        exports_blob[ep++] = 0x00;
        exports_size = ep;
    }

    memset(func_starts_blob, 0, sizeof(func_starts_blob));
    {
        size_t n = append_uleb128(func_starts_blob, sizeof(func_starts_blob),
                                  entry_off);
        if (n == 0 || n + 1 > sizeof(func_starts_blob))
            return -1;
        func_starts_blob[n] = 0x00;
        func_starts_size = 8;
    }

    memset(symtab_blob, 0, sizeof(symtab_blob));
    memset(strtab_blob, 0, sizeof(strtab_blob));
    strtab_blob[0] = 0x20;
    strtab_blob[1] = 0x00;
    memcpy(strtab_blob + 2, "__mh_execute_header", 18);
    memcpy(strtab_blob + 22, "_main", 6);
    {
        uint8_t *s = symtab_blob;
        /* __mh_execute_header */
        w32(&s, 2u);
        w8(&s, N_SECT | N_EXT);
        w8(&s, 1u);
        w16(&s, 0x0010u);
        w64(&s, image_base);
        /* _main */
        w32(&s, 22u);
        w8(&s, N_SECT | N_EXT);
        w8(&s, 1u);
        w16(&s, 0u);
        w64(&s, entry_addr);
    }

    exports_off = fixups_off + sizeof(fixups_blob);
    func_starts_off = exports_off + exports_size;
    symtab_off = func_starts_off + func_starts_size;
    strtab_off = symtab_off + sizeof(symtab_blob);
    linkedit_size = strtab_off + sizeof(strtab_blob) - linkedit_off;
    total_size = strtab_off + sizeof(strtab_blob);

    buf = (uint8_t *)calloc(1, total_size);
    if (!buf)
        return -1;
    p = buf;

    w32(&p, MH_MAGIC_64);
    w32(&p, CPU_TYPE_ARM64);
    w32(&p, CPU_SUBTYPE_ALL);
    w32(&p, MH_EXECUTE);
    w32(&p, ncmds);
    w32(&p, sizeofcmds);
    /*
     * No-link payload executables carry pre-resolved absolute pointers in
     * synthesized GOT slots. Keep a fixed image base for this format.
     */
    w32(&p, MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL);
    w32(&p, 0u);

    /* __PAGEZERO */
    w32(&p, LC_SEGMENT_64);
    w32(&p, 72u);
    {
        char segname[16] = {0};
        memcpy(segname, "__PAGEZERO", 10);
        wbytes(&p, segname, sizeof(segname));
    }
    w64(&p, 0u);
    w64(&p, image_base);
    w64(&p, 0u);
    w64(&p, 0u);
    w32(&p, 0u);
    w32(&p, 0u);
    w32(&p, 0u);
    w32(&p, 0u);

    /* __TEXT */
    w32(&p, LC_SEGMENT_64);
    w32(&p, 152u);
    {
        char segname[16] = {0};
        char sectname[16] = {0};
        memcpy(segname, "__TEXT", 6);
        wbytes(&p, segname, sizeof(segname));
        w64(&p, image_base);
        w64(&p, text_file_size);
        w64(&p, 0u);
        w64(&p, text_file_size);
        w32(&p, 5u);
        w32(&p, 5u);
        w32(&p, 1u);
        w32(&p, 0u);

        memcpy(sectname, "__text", 6);
        wbytes(&p, sectname, sizeof(sectname));
        memset(segname, 0, sizeof(segname));
        memcpy(segname, "__TEXT", 6);
        wbytes(&p, segname, sizeof(segname));
        w64(&p, image_base + text_off);
        w64(&p, code_size);
        w32(&p, (uint32_t)text_off);
        w32(&p, 2u);
        w32(&p, 0u);
        w32(&p, 0u);
        w32(&p, S_REGULAR | S_ATTR_PURE_INSTRUCTIONS |
                 S_ATTR_SOME_INSTRUCTIONS);
        w32(&p, 0u);
        w32(&p, 0u);
        w32(&p, 0u);
    }

    /* __LINKEDIT */
    w32(&p, LC_SEGMENT_64);
    w32(&p, 72u);
    {
        char segname[16] = {0};
        memcpy(segname, "__LINKEDIT", 10);
        wbytes(&p, segname, sizeof(segname));
    }
    w64(&p, image_base + linkedit_off);
    w64(&p, obj_align_up(linkedit_size, page));
    w64(&p, linkedit_off);
    w64(&p, linkedit_size);
    w32(&p, 1u);
    w32(&p, 1u);
    w32(&p, 0u);
    w32(&p, 0u);

    w32(&p, LC_DYLD_CHAINED_FIXUPS);
    w32(&p, 16u);
    w32(&p, (uint32_t)fixups_off);
    w32(&p, (uint32_t)sizeof(fixups_blob));

    w32(&p, LC_DYLD_EXPORTS_TRIE);
    w32(&p, 16u);
    w32(&p, (uint32_t)exports_off);
    w32(&p, (uint32_t)exports_size);

    w32(&p, LC_SYMTAB);
    w32(&p, 24u);
    w32(&p, (uint32_t)symtab_off);
    w32(&p, 2u);
    w32(&p, (uint32_t)strtab_off);
    w32(&p, (uint32_t)sizeof(strtab_blob));

    w32(&p, LC_DYSYMTAB);
    w32(&p, 80u);
    w32(&p, 0u); w32(&p, 0u); /* ilocalsym, nlocalsym */
    w32(&p, 0u); w32(&p, 2u); /* iextdefsym, nextdefsym */
    w32(&p, 2u); w32(&p, 0u); /* iundefsym, nundefsym */
    w32(&p, 0u); w32(&p, 0u); /* tocoff, ntoc */
    w32(&p, 0u); w32(&p, 0u); /* modtaboff, nmodtab */
    w32(&p, 0u); w32(&p, 0u); /* extrefsymoff, nextrefsyms */
    w32(&p, 0u); w32(&p, 0u); /* indirectsymoff, nindirectsyms */
    w32(&p, 0u); w32(&p, 0u); /* extreloff, nextrel */
    w32(&p, 0u); w32(&p, 0u); /* locreloff, nlocrel */

    w32(&p, LC_LOAD_DYLINKER);
    w32(&p, 32u);
    w32(&p, 12u);
    wbytes(&p, dyld_path, strlen(dyld_path) + 1u);
    wpad(&p, 32u - 12u - (strlen(dyld_path) + 1u));

    w32(&p, LC_UUID);
    w32(&p, 24u);
    wpad(&p, 16u);

    w32(&p, LC_BUILD_VERSION);
    w32(&p, 32u);
    w32(&p, PLATFORM_MACOS);
    w32(&p, (14u << 16));
    w32(&p, (14u << 16));
    w32(&p, 1u);
    w32(&p, TOOL_LD);
    w32(&p, 0x04ce0100u);

    w32(&p, LC_SOURCE_VERSION);
    w32(&p, 16u);
    w64(&p, 0u);

    w32(&p, LC_MAIN);
    w32(&p, 24u);
    w64(&p, entry_off);
    w64(&p, 0u);

    w32(&p, LC_LOAD_DYLIB);
    w32(&p, 56u);
    w32(&p, 24u);
    w32(&p, 2u);
    w32(&p, 0x054c0000u);
    w32(&p, 0x00010000u);
    wbytes(&p, libsystem_path, strlen(libsystem_path) + 1u);
    wpad(&p, 56u - 24u - (strlen(libsystem_path) + 1u));

    w32(&p, LC_FUNCTION_STARTS);
    w32(&p, 16u);
    w32(&p, (uint32_t)func_starts_off);
    w32(&p, (uint32_t)func_starts_size);

    w32(&p, LC_DATA_IN_CODE);
    w32(&p, 16u);
    w32(&p, (uint32_t)symtab_off);
    w32(&p, 0u);

    if ((size_t)(p - buf) > text_off) {
        free(buf);
        return -1;
    }

    memcpy(buf + text_off, code, code_size);
    memcpy(buf + fixups_off, fixups_blob, sizeof(fixups_blob));
    memcpy(buf + exports_off, exports_blob, exports_size);
    memcpy(buf + func_starts_off, func_starts_blob, func_starts_size);
    memcpy(buf + symtab_off, symtab_blob, sizeof(symtab_blob));
    memcpy(buf + strtab_off, strtab_blob, sizeof(strtab_blob));

    written = fwrite(buf, 1, total_size, out);
    free(buf);
    return written == total_size ? 0 : -1;
}
