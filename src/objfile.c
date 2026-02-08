#include "objfile.h"
#include "arena.h"
#include <stdlib.h>
#include <string.h>

#define OBJ_CODE_BUF_SIZE (4 * 1024 * 1024)
#define OBJ_DATA_BUF_SIZE (1 * 1024 * 1024)
#define OBJ_INITIAL_RELOC_CAP 256
#define OBJ_INITIAL_SYMBOL_CAP 128

/* Mach-O constants (defined locally for portability) */
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

/* Remap LLVM intrinsic names to libc equivalents for linking */
static const char *remap_intrinsic(const char *name) {
    if (!name || strncmp(name, "llvm.", 5) != 0) return name;
    if (strcmp(name, "llvm.pow.f32") == 0) return "powf";
    if (strcmp(name, "llvm.pow.f64") == 0) return "pow";
    if (strcmp(name, "llvm.sqrt.f32") == 0) return "sqrtf";
    if (strcmp(name, "llvm.sqrt.f64") == 0) return "sqrt";
    if (strcmp(name, "llvm.copysign.f32") == 0) return "copysignf";
    if (strcmp(name, "llvm.copysign.f64") == 0) return "copysign";
    if (strcmp(name, "llvm.powi.f32.i32") == 0) return "powf";
    if (strcmp(name, "llvm.powi.f64.i32") == 0) return "pow";
    if (strcmp(name, "llvm.fabs.f32") == 0) return "fabsf";
    if (strcmp(name, "llvm.fabs.f64") == 0) return "fabs";
    if (strcmp(name, "llvm.sin.f32") == 0) return "sinf";
    if (strcmp(name, "llvm.sin.f64") == 0) return "sin";
    if (strcmp(name, "llvm.cos.f32") == 0) return "cosf";
    if (strcmp(name, "llvm.cos.f64") == 0) return "cos";
    if (strcmp(name, "llvm.exp.f32") == 0) return "expf";
    if (strcmp(name, "llvm.exp.f64") == 0) return "exp";
    if (strcmp(name, "llvm.exp2.f32") == 0) return "exp2f";
    if (strcmp(name, "llvm.exp2.f64") == 0) return "exp2";
    if (strcmp(name, "llvm.log.f32") == 0) return "logf";
    if (strcmp(name, "llvm.log.f64") == 0) return "log";
    if (strcmp(name, "llvm.log2.f32") == 0) return "log2f";
    if (strcmp(name, "llvm.log2.f64") == 0) return "log2";
    if (strcmp(name, "llvm.log10.f32") == 0) return "log10f";
    if (strcmp(name, "llvm.log10.f64") == 0) return "log10";
    if (strcmp(name, "llvm.floor.f32") == 0) return "floorf";
    if (strcmp(name, "llvm.floor.f64") == 0) return "floor";
    if (strcmp(name, "llvm.ceil.f32") == 0) return "ceilf";
    if (strcmp(name, "llvm.ceil.f64") == 0) return "ceil";
    if (strcmp(name, "llvm.trunc.f32") == 0) return "truncf";
    if (strcmp(name, "llvm.trunc.f64") == 0) return "trunc";
    if (strcmp(name, "llvm.round.f32") == 0) return "roundf";
    if (strcmp(name, "llvm.round.f64") == 0) return "round";
    if (strcmp(name, "llvm.fma.f32") == 0) return "fmaf";
    if (strcmp(name, "llvm.fma.f64") == 0) return "fma";
    if (strcmp(name, "llvm.minnum.f32") == 0) return "fminf";
    if (strcmp(name, "llvm.minnum.f64") == 0) return "fmin";
    if (strcmp(name, "llvm.maxnum.f32") == 0) return "fmaxf";
    if (strcmp(name, "llvm.maxnum.f64") == 0) return "fmax";
    if (strcmp(name, "llvm.rint.f32") == 0) return "rintf";
    if (strcmp(name, "llvm.rint.f64") == 0) return "rint";
    if (strcmp(name, "llvm.nearbyint.f32") == 0) return "nearbyintf";
    if (strcmp(name, "llvm.nearbyint.f64") == 0) return "nearbyint";
    return name;
}

uint32_t lr_obj_ensure_symbol(lr_objfile_ctx_t *oc, const char *name,
                               bool is_defined, uint8_t section,
                               uint32_t offset) {
    if (!name) return UINT32_MAX;
    name = remap_intrinsic(name);
    for (uint32_t i = 0; i < oc->num_symbols; i++) {
        if (strcmp(oc->symbols[i].name, name) == 0) {
            if (is_defined && !oc->symbols[i].is_defined) {
                oc->symbols[i].is_defined = true;
                oc->symbols[i].section = section;
                oc->symbols[i].offset = offset;
            }
            return i;
        }
    }

    if (oc->num_symbols == oc->symbol_cap) {
        uint32_t new_cap = oc->symbol_cap == 0
            ? OBJ_INITIAL_SYMBOL_CAP
            : oc->symbol_cap * 2;
        lr_obj_symbol_t *ns = realloc(oc->symbols,
                                       new_cap * sizeof(lr_obj_symbol_t));
        if (!ns) return UINT32_MAX;
        oc->symbols = ns;
        oc->symbol_cap = new_cap;
    }

    uint32_t idx = oc->num_symbols++;
    oc->symbols[idx].name = name;
    oc->symbols[idx].offset = offset;
    oc->symbols[idx].section = section;
    oc->symbols[idx].is_defined = is_defined;
    return idx;
}

void lr_obj_add_reloc(lr_objfile_ctx_t *oc, uint32_t offset,
                       uint32_t symbol_idx, uint8_t type) {
    if (oc->num_relocs == oc->reloc_cap) {
        uint32_t new_cap = oc->reloc_cap == 0
            ? OBJ_INITIAL_RELOC_CAP
            : oc->reloc_cap * 2;
        lr_obj_reloc_t *nr = realloc(oc->relocs,
                                      new_cap * sizeof(lr_obj_reloc_t));
        if (!nr) return;
        oc->relocs = nr;
        oc->reloc_cap = new_cap;
    }

    uint32_t i = oc->num_relocs++;
    oc->relocs[i].offset = offset;
    oc->relocs[i].symbol_idx = symbol_idx;
    oc->relocs[i].type = type;
}

/* Byte-level write helpers for Mach-O structures */

static void w8(uint8_t **p, uint8_t v) {
    *(*p)++ = v;
}

static void w16(uint8_t **p, uint16_t v) {
    (*p)[0] = (uint8_t)(v);
    (*p)[1] = (uint8_t)(v >> 8);
    *p += 2;
}

static void w32(uint8_t **p, uint32_t v) {
    (*p)[0] = (uint8_t)(v);
    (*p)[1] = (uint8_t)(v >> 8);
    (*p)[2] = (uint8_t)(v >> 16);
    (*p)[3] = (uint8_t)(v >> 24);
    *p += 4;
}

static void w64(uint8_t **p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        (*p)[i] = (uint8_t)(v >> (i * 8));
    }
    *p += 8;
}

static void wbytes(uint8_t **p, const void *data, size_t n) {
    memcpy(*p, data, n);
    *p += n;
}

static void wpad(uint8_t **p, size_t n) {
    memset(*p, 0, n);
    *p += n;
}

static size_t align_up(size_t val, size_t alignment) {
    return (val + alignment - 1) & ~(alignment - 1);
}

static int write_macho_arm64(FILE *out,
                              const uint8_t *code, size_t code_size,
                              const uint8_t *data, size_t data_size,
                              const lr_objfile_ctx_t *oc) {
    /* Count defined vs undefined symbols for nlist ordering.
       Mach-O requires: local defined, external defined, then undefined. */
    uint32_t n_defined = 0;
    for (uint32_t i = 0; i < oc->num_symbols; i++) {
        if (oc->symbols[i].is_defined)
            n_defined++;
    }

    /* Build symbol index remapping: defined first, then undefined */
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

    /* Build string table: NUL byte, then "_name\0" for each symbol */
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

    /* Determine sections: __text always present, __data if data_size > 0 */
    bool has_data = data_size > 0;
    uint32_t num_sections = has_data ? 2 : 1;

    /* Calculate layout offsets */
    /* mach_header_64: 32 bytes */
    size_t header_size = 32;

    /* LC_SEGMENT_64: 72 + 80*nsections */
    size_t segment_cmd_size = 72 + 80 * num_sections;

    /* LC_SYMTAB: 24 bytes */
    size_t symtab_cmd_size = 24;

    /* LC_BUILD_VERSION: 24 bytes */
    size_t build_version_cmd_size = 24;

    uint32_t ncmds = 3;
    size_t sizeofcmds = segment_cmd_size + symtab_cmd_size
                      + build_version_cmd_size;

    size_t section_data_off = header_size + sizeofcmds;
    size_t text_file_off = section_data_off;
    size_t text_size = code_size;

    size_t data_align = 8;
    size_t data_file_off = align_up(text_file_off + text_size, data_align);
    size_t data_pad = data_file_off - (text_file_off + text_size);
    size_t data_vmaddr = has_data ? align_up(text_size, data_align) : 0;

    /* Relocations follow section data */
    size_t reloc_off = data_file_off + (has_data ? data_size : 0);
    /* Mach-O relocation entries: 8 bytes each */
    size_t reloc_size = oc->num_relocs * 8;

    /* Symbol table follows relocations */
    size_t symtab_off = reloc_off + reloc_size;
    /* nlist_64: 16 bytes each */
    size_t symtab_entries_size = oc->num_symbols * 16;

    /* String table follows symbol table */
    size_t strtab_off = symtab_off + symtab_entries_size;

    size_t total_size = strtab_off + strtab_size;

    /* Allocate output buffer */
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
    w32(&p, CPU_TYPE_ARM64);
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

            uint32_t r_pcrel = 0;
            uint32_t r_type = r->type;
            if (r_type == LR_RELOC_ARM64_BRANCH26 ||
                r_type == LR_RELOC_ARM64_PAGE21 ||
                r_type == LR_RELOC_ARM64_GOT_LOAD_PAGE21) {
                r_pcrel = 1;
            }

            uint32_t packed = (mapped_sym & 0x00FFFFFFu)
                            | (r_pcrel << 24)
                            | (2u << 25)
                            | (1u << 27)
                            | ((r_type & 0xFu) << 28);

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

    /* Write to file */
    size_t written = fwrite(buf, 1, total_size, out);

    free(buf);
    free(strtab);
    free(str_offsets);
    free(sym_remap);
    free(sym_order);

    return written == total_size ? 0 : -1;
}

int lr_emit_object(lr_module_t *m, const lr_target_t *target, FILE *out) {
    if (!m || !target || !out)
        return -1;

    uint8_t *code_buf = calloc(1, OBJ_CODE_BUF_SIZE);
    uint8_t *data_buf = calloc(1, OBJ_DATA_BUF_SIZE);
    if (!code_buf || !data_buf) {
        free(code_buf);
        free(data_buf);
        return -1;
    }

    lr_arena_t *arena = lr_arena_create(0);
    if (!arena) {
        free(code_buf);
        free(data_buf);
        return -1;
    }

    lr_objfile_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    m->obj_ctx = &ctx;

    size_t code_pos = 0;
    size_t data_pos = 0;
    bool has_data = false;

    /* Compile each defined function (has blocks = is a definition) */
    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (f->is_decl || !f->first_block)
            continue;

        uint32_t sym_idx = lr_obj_ensure_symbol(&ctx, f->name, true, 1,
                                                 (uint32_t)code_pos);
        (void)sym_idx;

        uint32_t reloc_base = ctx.num_relocs;

        size_t func_len = 0;
        int rc = target->compile_func(f, m,
                                       code_buf + code_pos,
                                       OBJ_CODE_BUF_SIZE - code_pos,
                                       &func_len, arena);
        if (rc != 0) {
            m->obj_ctx = NULL;
            lr_arena_destroy(arena);
            free(code_buf);
            free(data_buf);
            return -1;
        }

        for (uint32_t ri = reloc_base; ri < ctx.num_relocs; ri++)
            ctx.relocs[ri].offset += (uint32_t)code_pos;

        code_pos += func_len;
    }

    /* Add declared (external) functions as undefined symbols */
    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (!f->is_decl && f->first_block)
            continue;
        lr_obj_ensure_symbol(&ctx, f->name, false, 0, 0);
    }

    /* Copy global data into data section */
    for (lr_global_t *g = m->first_global; g; g = g->next) {
        if (g->is_external) {
            lr_obj_ensure_symbol(&ctx, g->name, false, 0, 0);
            continue;
        }

        size_t gsize = lr_type_size(g->type);
        if (gsize == 0)
            gsize = 8;

        size_t galign = lr_type_align(g->type);
        if (galign == 0)
            galign = 8;

        data_pos = align_up(data_pos, galign);

        if (data_pos + gsize > OBJ_DATA_BUF_SIZE) {
            m->obj_ctx = NULL;
            lr_arena_destroy(arena);
            free(code_buf);
            free(data_buf);
            return -1;
        }

        if (g->init_data && g->init_size > 0) {
            size_t copy_n = g->init_size < gsize ? g->init_size : gsize;
            memcpy(data_buf + data_pos, g->init_data, copy_n);
        }

        lr_obj_ensure_symbol(&ctx, g->name, true, 2,
                              (uint32_t)data_pos);

        data_pos += gsize;
        has_data = true;
    }

    m->obj_ctx = NULL;

    int result = write_macho_arm64(out,
                                    code_buf, code_pos,
                                    has_data ? data_buf : NULL,
                                    has_data ? data_pos : 0,
                                    &ctx);

    free(ctx.relocs);
    free(ctx.symbols);
    lr_arena_destroy(arena);
    free(code_buf);
    free(data_buf);

    return result;
}
