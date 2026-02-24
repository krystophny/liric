#include "objfile.h"
#include "objfile_macho.h"
#include "objfile_elf.h"
#include "liric.h"
#include "compile_mode.h"
#include "platform/platform.h"
#include "platform/platform_os.h"
#include "arena.h"
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#define OBJ_CODE_BUF_SIZE (4 * 1024 * 1024)
#define OBJ_DATA_BUF_SIZE (1 * 1024 * 1024)
#define OBJ_INITIAL_RELOC_CAP 256
#define OBJ_INITIAL_SYMBOL_CAP 128

typedef struct lr_obj_build_result {
    uint8_t *code_buf;
    uint8_t *data_buf;
    size_t code_pos;
    size_t data_pos;
    bool has_data;
    lr_objfile_ctx_t ctx;
} lr_obj_build_result_t;

#if !defined(__linux__) && !defined(_WIN32)
#define MACHO_ARM64_EXEC_IMAGE_BASE 0x100000000ULL

static uint32_t read_u32_le(const uint8_t *buf, uint32_t off) {
    return (uint32_t)buf[off] |
           ((uint32_t)buf[off + 1] << 8) |
           ((uint32_t)buf[off + 2] << 16) |
           ((uint32_t)buf[off + 3] << 24);
}

static int write_u32_le(uint8_t *buf, size_t buflen, uint32_t off, uint32_t v) {
    if ((size_t)off + 4 > buflen)
        return -1;
    buf[off + 0] = (uint8_t)(v & 0xffu);
    buf[off + 1] = (uint8_t)((v >> 8) & 0xffu);
    buf[off + 2] = (uint8_t)((v >> 16) & 0xffu);
    buf[off + 3] = (uint8_t)((v >> 24) & 0xffu);
    return 0;
}

static int write_u64_le(uint8_t *buf, size_t buflen, uint32_t off, uint64_t v) {
    if ((size_t)off + 8 > buflen)
        return -1;
    for (int i = 0; i < 8; i++)
        buf[off + i] = (uint8_t)(v >> (i * 8));
    return 0;
}

static int patch_aarch64_page21_exec(uint8_t *buf, size_t buflen, uint32_t off,
                                     uint64_t place_addr, uint64_t target_addr) {
    if ((size_t)off + 4 > buflen)
        return -1;
    uint64_t target_page = target_addr & ~0xfffULL;
    uint64_t place_page = place_addr & ~0xfffULL;
    int64_t pages = ((int64_t)target_page - (int64_t)place_page) >> 12;
    if (pages < -(1LL << 20) || pages >= (1LL << 20))
        return -1;
    uint32_t insn = read_u32_le(buf, off);
    insn &= ~((0x3u << 29) | (0x7ffffu << 5));
    insn |= (((uint32_t)pages & 0x3u) << 29);
    insn |= ((((uint32_t)pages >> 2) & 0x7ffffu) << 5);
    return write_u32_le(buf, buflen, off, insn);
}

static int patch_aarch64_pageoff12_exec(uint8_t *buf, size_t buflen, uint32_t off,
                                        uint64_t target_addr, bool got_load) {
    if ((size_t)off + 4 > buflen)
        return -1;
    uint32_t imm = (uint32_t)(target_addr & 0xfffu);
    if (got_load) {
        if ((imm & 0x7u) != 0)
            return -1;
        imm >>= 3;
    }
    uint32_t insn = read_u32_le(buf, off);
    insn &= ~(0xfffu << 10);
    insn |= ((imm & 0xfffu) << 10);
    return write_u32_le(buf, buflen, off, insn);
}

static const char *normalize_external_lookup_name(const char *name) {
    if (!name)
        return NULL;
    while (*name == '\1')
        name++;
    return name;
}

static void *resolve_external_symbol_addr(const char *name, void *runtime_handle) {
    const char *lookup = normalize_external_lookup_name(name);
    void *addr = NULL;
    const int verbose = (getenv("LIRIC_VERBOSE_BLOB_LINK") != NULL);
    if (!lookup || !lookup[0])
        return NULL;
    addr = lr_platform_intrinsic_resolve_addr(lookup, runtime_handle);
    if (addr)
        return addr;
    if (verbose && name && name != lookup) {
        fprintf(stderr,
                "macho_exec_payload: normalize external '%s' -> '%s'\n",
                name, lookup);
    }
    addr = lr_platform_dlsym_default(lookup);
    if (verbose && !addr) {
        fprintf(stderr, "macho_exec_payload: dlsym default miss '%s'\n", lookup);
    }
    if (!addr && lookup[0] == '_')
        addr = lr_platform_dlsym_default(lookup + 1);
    if (verbose && !addr && lookup[0] == '_') {
        fprintf(stderr, "macho_exec_payload: dlsym default miss '%s'\n", lookup + 1);
    }
    if (!addr && runtime_handle) {
        addr = lr_platform_dlsym(runtime_handle, lookup);
        if (verbose && !addr) {
            fprintf(stderr, "macho_exec_payload: dlsym runtime miss '%s'\n", lookup);
        }
        if (!addr && lookup[0] == '_')
            addr = lr_platform_dlsym(runtime_handle, lookup + 1);
        if (verbose && !addr && lookup[0] == '_') {
            fprintf(stderr, "macho_exec_payload: dlsym runtime miss '%s'\n",
                    lookup + 1);
        }
    }
    return addr;
}

typedef struct lr_exec_stub {
    const char *name;
    const uint8_t *bytes;
    uint32_t size;
} lr_exec_stub_t;

static const lr_exec_stub_t *find_exec_stub(const char *name) {
    (void)name;
    return NULL;
}

static int build_macho_exec_payload_aarch64(const lr_obj_build_result_t *build,
                                            uint8_t **out_code,
                                            size_t *out_code_size) {
    uint8_t *buf = NULL;
    uint64_t *sym_addr = NULL;
    uint32_t *got_slot_off = NULL;
    uint32_t *stub_off = NULL;
    uint8_t *sym_needed = NULL;
    const lr_exec_stub_t **stub_for_sym = NULL;
    size_t code_size = 0, data_off = 0, got_off = 0, total_size = 0;
    void *runtime_handle = NULL;
    const int verbose = (getenv("LIRIC_VERBOSE_BLOB_LINK") != NULL);
    const uint64_t text_base = MACHO_ARM64_EXEC_IMAGE_BASE +
        (uint64_t)lr_macho_executable_text_offset_arm64();
#define FAIL_BUILD(msg) do { \
    if (verbose) fprintf(stderr, "macho_exec_payload: %s\n", (msg)); \
    goto fail; \
} while (0)

    if (!build || !out_code || !out_code_size)
        return -1;
    *out_code = NULL;
    *out_code_size = 0;

    code_size = build->code_pos;
    data_off = obj_align_up(code_size, 8u);
    got_off = obj_align_up(data_off + build->data_pos, 8u);
    total_size = got_off;

    got_slot_off = (uint32_t *)calloc(build->ctx.num_symbols, sizeof(uint32_t));
    sym_addr = (uint64_t *)calloc(build->ctx.num_symbols, sizeof(uint64_t));
    stub_off = (uint32_t *)calloc(build->ctx.num_symbols, sizeof(uint32_t));
    sym_needed = (uint8_t *)calloc(build->ctx.num_symbols, sizeof(uint8_t));
    stub_for_sym = (const lr_exec_stub_t **)calloc(build->ctx.num_symbols,
                                                   sizeof(lr_exec_stub_t *));
    if (!got_slot_off || !sym_addr || !stub_off || !sym_needed || !stub_for_sym)
        FAIL_BUILD("allocation failed");
    for (uint32_t i = 0; i < build->ctx.num_symbols; i++) {
        got_slot_off[i] = UINT32_MAX;
        stub_off[i] = UINT32_MAX;
    }

    const char *runtime_lib = getenv("LIRIC_RUNTIME_LIB");
    if (runtime_lib && runtime_lib[0])
        runtime_handle = lr_platform_dlopen(runtime_lib);

    for (uint32_t ri = 0; ri < build->ctx.num_relocs; ri++) {
        const lr_obj_reloc_t *rel = &build->ctx.relocs[ri];
        if (rel->symbol_idx >= build->ctx.num_symbols)
            FAIL_BUILD("bad code reloc symbol index");
        sym_needed[rel->symbol_idx] = 1;
    }
    for (uint32_t ri = 0; ri < build->ctx.num_data_relocs; ri++) {
        const lr_obj_reloc_t *rel = &build->ctx.data_relocs[ri];
        if (rel->symbol_idx >= build->ctx.num_symbols)
            FAIL_BUILD("bad data reloc symbol index");
        sym_needed[rel->symbol_idx] = 1;
    }

    for (uint32_t ri = 0; ri < build->ctx.num_relocs; ri++) {
        const lr_obj_reloc_t *rel = &build->ctx.relocs[ri];
        if ((rel->type != LR_RELOC_ARM64_GOT_LOAD_PAGE21) &&
            (rel->type != LR_RELOC_ARM64_GOT_LOAD_PAGEOFF12))
            continue;
        if (got_slot_off[rel->symbol_idx] != UINT32_MAX)
            continue;
        got_slot_off[rel->symbol_idx] = (uint32_t)got_off;
        got_off += sizeof(uint64_t);
    }

    for (uint32_t si = 0; si < build->ctx.num_symbols; si++) {
        const lr_obj_symbol_t *sym = &build->ctx.symbols[si];
        const lr_exec_stub_t *stub = NULL;
        if (sym->is_defined)
            continue;
        stub = find_exec_stub(sym->name);
        if (!stub)
            continue;
        got_off = obj_align_up(got_off, 4u);
        stub_off[si] = (uint32_t)got_off;
        stub_for_sym[si] = stub;
        got_off += stub->size;
        if (verbose) {
            fprintf(stderr, "macho_exec_stub: symbol=%s off=%u size=%u\n",
                    sym->name, stub_off[si], stub->size);
        }
    }
    total_size = got_off;

    buf = (uint8_t *)calloc(1, total_size);
    if (!buf)
        FAIL_BUILD("buffer allocation failed");
    memcpy(buf, build->code_buf, build->code_pos);
    if (build->has_data && build->data_pos > 0)
        memcpy(buf + data_off, build->data_buf, build->data_pos);

    for (uint32_t si = 0; si < build->ctx.num_symbols; si++) {
        if (stub_off[si] == UINT32_MAX || !stub_for_sym[si])
            continue;
        memcpy(buf + stub_off[si], stub_for_sym[si]->bytes, stub_for_sym[si]->size);
    }

    for (uint32_t si = 0; si < build->ctx.num_symbols; si++) {
        const lr_obj_symbol_t *sym = &build->ctx.symbols[si];
        if (sym->is_defined) {
            if (sym->section == 1) {
                sym_addr[si] = text_base + sym->offset;
            } else if (sym->section == 2) {
                sym_addr[si] = text_base + data_off + sym->offset;
            } else {
                if (verbose) {
                    fprintf(stderr, "macho_exec_payload: bad defined section=%u symbol=%s\n",
                            (unsigned)sym->section, sym->name ? sym->name : "<null>");
                }
                FAIL_BUILD("bad defined section");
            }
            continue;
        }

        if (stub_off[si] != UINT32_MAX) {
            sym_addr[si] = text_base + stub_off[si];
            continue;
        }
        if (!sym_needed[si])
            continue;

        void *addr = resolve_external_symbol_addr(sym->name, runtime_handle);
        if (!addr) {
            if (verbose) {
                fprintf(stderr, "macho_exec_payload: unresolved external symbol=%s\n",
                        sym->name ? sym->name : "<null>");
            }
            FAIL_BUILD("unresolved external symbol");
        }
        sym_addr[si] = (uint64_t)(uintptr_t)addr;
    }

    for (uint32_t si = 0; si < build->ctx.num_symbols; si++) {
        if (got_slot_off[si] == UINT32_MAX)
            continue;
        if (write_u64_le(buf, total_size, got_slot_off[si], sym_addr[si]) != 0)
            FAIL_BUILD("writing GOT slot failed");
    }

    for (uint32_t ri = 0; ri < build->ctx.num_relocs; ri++) {
        const lr_obj_reloc_t *rel = &build->ctx.relocs[ri];
        uint64_t place_addr = text_base + rel->offset;
        uint64_t target_addr = 0;
        if (rel->symbol_idx >= build->ctx.num_symbols) {
            if (verbose) {
                fprintf(stderr, "macho_exec_payload: bad reloc symbol idx rel=%u type=%u off=%u\n",
                        ri, rel->type, rel->offset);
            }
            FAIL_BUILD("bad code reloc symbol index");
        }
        switch (rel->type) {
        case LR_RELOC_ARM64_PAGE21:
            target_addr = sym_addr[rel->symbol_idx];
            if (patch_aarch64_page21_exec(buf, total_size, rel->offset,
                                          place_addr, target_addr) != 0) {
                if (verbose) {
                    fprintf(stderr, "macho_exec_payload: PAGE21 patch failed rel=%u off=%u sym=%u target=0x%llx place=0x%llx\n",
                            ri, rel->offset, rel->symbol_idx,
                            (unsigned long long)target_addr,
                            (unsigned long long)place_addr);
                }
                FAIL_BUILD("code PAGE21 patch failed");
            }
            break;
        case LR_RELOC_ARM64_PAGEOFF12:
            target_addr = sym_addr[rel->symbol_idx];
            if (patch_aarch64_pageoff12_exec(buf, total_size, rel->offset,
                                             target_addr, false) != 0) {
                if (verbose) {
                    fprintf(stderr, "macho_exec_payload: PAGEOFF12 patch failed rel=%u off=%u sym=%u target=0x%llx\n",
                            ri, rel->offset, rel->symbol_idx,
                            (unsigned long long)target_addr);
                }
                FAIL_BUILD("code PAGEOFF12 patch failed");
            }
            break;
        case LR_RELOC_ARM64_GOT_LOAD_PAGE21:
            if (got_slot_off[rel->symbol_idx] == UINT32_MAX)
                FAIL_BUILD("missing GOT slot for PAGE21");
            target_addr = text_base + got_slot_off[rel->symbol_idx];
            if (patch_aarch64_page21_exec(buf, total_size, rel->offset,
                                          place_addr, target_addr) != 0) {
                if (verbose) {
                    fprintf(stderr, "macho_exec_payload: GOT PAGE21 patch failed rel=%u off=%u sym=%u got_off=%u\n",
                            ri, rel->offset, rel->symbol_idx,
                            got_slot_off[rel->symbol_idx]);
                }
                FAIL_BUILD("code GOT PAGE21 patch failed");
            }
            break;
        case LR_RELOC_ARM64_GOT_LOAD_PAGEOFF12:
            if (got_slot_off[rel->symbol_idx] == UINT32_MAX)
                FAIL_BUILD("missing GOT slot for PAGEOFF12");
            target_addr = text_base + got_slot_off[rel->symbol_idx];
            if (patch_aarch64_pageoff12_exec(buf, total_size, rel->offset,
                                             target_addr, true) != 0) {
                if (verbose) {
                    fprintf(stderr, "macho_exec_payload: GOT PAGEOFF12 patch failed rel=%u off=%u sym=%u got_off=%u\n",
                            ri, rel->offset, rel->symbol_idx,
                            got_slot_off[rel->symbol_idx]);
                }
                FAIL_BUILD("code GOT PAGEOFF12 patch failed");
            }
            break;
        default:
            if (verbose) {
                fprintf(stderr, "macho_exec_payload: unsupported code reloc type=%u rel=%u off=%u\n",
                        rel->type, ri, rel->offset);
            }
            FAIL_BUILD("unsupported code reloc");
        }
    }

    for (uint32_t ri = 0; ri < build->ctx.num_data_relocs; ri++) {
        const lr_obj_reloc_t *rel = &build->ctx.data_relocs[ri];
        uint32_t patch_off = (uint32_t)data_off + rel->offset;
        if (rel->symbol_idx >= build->ctx.num_symbols)
            FAIL_BUILD("bad data reloc symbol index");
        if (rel->type != LR_RELOC_ARM64_ABS64)
            FAIL_BUILD("unsupported data reloc type");
        if (write_u64_le(buf, total_size, patch_off, sym_addr[rel->symbol_idx]) != 0)
            FAIL_BUILD("data reloc write failed");
    }

    if (runtime_handle)
        (void)lr_platform_dlclose(runtime_handle);
    free(sym_addr);
    free(got_slot_off);
    free(stub_off);
    free(sym_needed);
    free(stub_for_sym);
    *out_code = buf;
    *out_code_size = total_size;
    return 0;

fail:
    if (runtime_handle)
        (void)lr_platform_dlclose(runtime_handle);
    free(buf);
    free(sym_addr);
    free(got_slot_off);
    free(stub_off);
    free(sym_needed);
    free(stub_for_sym);
    return -1;
#undef FAIL_BUILD
}
#endif

static uint32_t obj_symbol_hash(const char *name) {
    uint32_t h = 2166136261u;
    while (*name) {
        h ^= (uint8_t)*name++;
        h *= 16777619u;
    }
    return h;
}

static int obj_symbol_index_rebuild(lr_objfile_ctx_t *oc, uint32_t min_symbols) {
    uint32_t cap = 1;
    while (cap < (min_symbols << 1))
        cap <<= 1;

    uint32_t *new_index = (uint32_t *)calloc(cap, sizeof(uint32_t));
    if (!new_index)
        return -1;

    for (uint32_t i = 0; i < oc->num_symbols; i++) {
        uint32_t slot = oc->symbols[i].hash & (cap - 1u);
        while (new_index[slot] != 0)
            slot = (slot + 1u) & (cap - 1u);
        new_index[slot] = i + 1u;
    }

    free(oc->symbol_index);
    oc->symbol_index = new_index;
    oc->symbol_index_cap = cap;
    return 0;
}

int lr_obj_build_symbol_cache(lr_objfile_ctx_t *oc, lr_module_t *m) {
    if (!oc || !m)
        return -1;

    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (f->name && f->name[0])
            lr_module_intern_symbol(m, f->name);
    }
    for (lr_global_t *g = m->first_global; g; g = g->next) {
        if (g->name && g->name[0])
            lr_module_intern_symbol(m, g->name);
    }

    oc->module_sym_count = m->num_symbols;
    if (oc->module_sym_count == 0)
        return 0;

    oc->module_sym_defined = (uint8_t *)calloc(oc->module_sym_count, sizeof(uint8_t));
    oc->module_sym_funcs = (lr_func_t **)calloc(oc->module_sym_count,
                                                sizeof(lr_func_t *));
    if (!oc->module_sym_defined || !oc->module_sym_funcs) {
        free(oc->module_sym_defined);
        free(oc->module_sym_funcs);
        oc->module_sym_defined = NULL;
        oc->module_sym_funcs = NULL;
        oc->module_sym_count = 0;
        return -1;
    }

    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (!f->name || !f->name[0])
            continue;
        uint32_t sym_id = lr_module_intern_symbol(m, f->name);
        if (sym_id >= oc->module_sym_count)
            continue;
        oc->module_sym_funcs[sym_id] = f;
        if (f->first_block)
            oc->module_sym_defined[sym_id] = 1;
    }
    for (lr_global_t *g = m->first_global; g; g = g->next) {
        if (!g->name || !g->name[0] || g->is_external)
            continue;
        uint32_t sym_id = lr_module_intern_symbol(m, g->name);
        if (sym_id < oc->module_sym_count)
            oc->module_sym_defined[sym_id] = 1;
    }
    return 0;
}

static const char *remap_intrinsic(const char *name) {
    return lr_platform_intrinsic_libc_name(name);
}

static bool obj_symbol_should_be_weak(const char *name) {
    static const char *k_weak_prefixes[] = {
        "__lfortran_module_init_",
        "_copy_",
        "_deepcopy_",
        "_allocate_struct_",
        "_deallocate_struct_",
        "_Type_Info_",
        "_VTable_",
        "__module_file_common_block_",
    };
    if (!name || !name[0])
        return false;
    for (size_t i = 0; i < sizeof(k_weak_prefixes) / sizeof(k_weak_prefixes[0]); i++) {
        const char *prefix = k_weak_prefixes[i];
        size_t prefix_len = strlen(prefix);
        if (strncmp(name, prefix, prefix_len) == 0)
            return true;
    }
    return false;
}

uint32_t lr_obj_ensure_symbol(lr_objfile_ctx_t *oc, const char *name,
                               bool is_defined, uint8_t section,
                               uint32_t offset) {
    if (!name) return UINT32_MAX;
    if (!oc->preserve_symbol_names)
        name = remap_intrinsic(name);
    if (!name) return UINT32_MAX;
    uint32_t hash = obj_symbol_hash(name);

    if (oc->symbol_index_cap == 0) {
        if (obj_symbol_index_rebuild(oc, 1) != 0)
            return UINT32_MAX;
    }

    uint32_t slot = hash & (oc->symbol_index_cap - 1u);
    while (1) {
        uint32_t stored = oc->symbol_index[slot];
        if (stored == 0)
            break;
        uint32_t i = stored - 1u;
        if (oc->symbols[i].hash == hash &&
            strcmp(oc->symbols[i].name, name) == 0) {
            if (is_defined && obj_symbol_should_be_weak(name))
                oc->symbols[i].is_weak = true;
            if (is_defined && !oc->symbols[i].is_defined) {
                oc->symbols[i].is_defined = true;
                oc->symbols[i].section = section;
                oc->symbols[i].offset = offset;
            }
            return i;
        }
        slot = (slot + 1u) & (oc->symbol_index_cap - 1u);
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

    if ((oc->num_symbols + 1u) * 2u > oc->symbol_index_cap) {
        if (obj_symbol_index_rebuild(oc, oc->num_symbols + 1u) != 0)
            return UINT32_MAX;
    }

    uint32_t idx = oc->num_symbols++;
    oc->symbols[idx].name = name;
    oc->symbols[idx].hash = hash;
    oc->symbols[idx].offset = offset;
    oc->symbols[idx].section = section;
    oc->symbols[idx].is_defined = is_defined;
    oc->symbols[idx].is_local = false;
    oc->symbols[idx].is_weak = is_defined && obj_symbol_should_be_weak(name);

    slot = hash & (oc->symbol_index_cap - 1u);
    while (oc->symbol_index[slot] != 0)
        slot = (slot + 1u) & (oc->symbol_index_cap - 1u);
    oc->symbol_index[slot] = idx + 1u;
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

void lr_obj_add_data_reloc(lr_objfile_ctx_t *oc, uint32_t offset,
                            uint32_t symbol_idx, uint8_t type) {
    if (oc->num_data_relocs == oc->data_reloc_cap) {
        uint32_t new_cap = oc->data_reloc_cap == 0
            ? OBJ_INITIAL_RELOC_CAP
            : oc->data_reloc_cap * 2;
        lr_obj_reloc_t *nr = realloc(oc->data_relocs,
                                      new_cap * sizeof(lr_obj_reloc_t));
        if (!nr) return;
        oc->data_relocs = nr;
        oc->data_reloc_cap = new_cap;
    }

    uint32_t i = oc->num_data_relocs++;
    oc->data_relocs[i].offset = offset;
    oc->data_relocs[i].symbol_idx = symbol_idx;
    oc->data_relocs[i].type = type;
}

static int obj_define_intrinsic_stubs(lr_obj_build_result_t *out,
                                      const lr_target_t *target) {
    if (!out || !target)
        return -1;

    for (uint32_t i = 0; i < out->ctx.num_symbols; i++) {
        lr_obj_symbol_t *sym = &out->ctx.symbols[i];
        if (sym->is_defined || !sym->name || !sym->name[0])
            continue;
        if (!lr_platform_intrinsic_supported(sym->name))
            continue;

        const uint8_t *blob_begin = NULL;
        const uint8_t *blob_end = NULL;
        if (!lr_platform_intrinsic_blob_lookup(sym->name, &blob_begin, &blob_end))
            return -1;
        if (!blob_begin || !blob_end || blob_end <= blob_begin)
            return -1;

        out->code_pos = obj_align_up(out->code_pos, 16);
        size_t blob_n = (size_t)(blob_end - blob_begin);
        if (out->code_pos + blob_n > OBJ_CODE_BUF_SIZE)
            return -1;

        memcpy(out->code_buf + out->code_pos, blob_begin, blob_n);
        sym->is_defined = true;
        sym->section = 1;
        sym->offset = (uint32_t)out->code_pos;
        out->code_pos += blob_n;
    }
    return 0;
}

void lr_objfile_ctx_destroy(lr_objfile_ctx_t *ctx) {
    if (!ctx)
        return;
    free(ctx->relocs);
    free(ctx->data_relocs);
    free(ctx->symbols);
    free(ctx->symbol_index);
    free(ctx->module_sym_defined);
    free(ctx->module_sym_funcs);
    memset(ctx, 0, sizeof(*ctx));
}

static void obj_build_result_destroy(lr_obj_build_result_t *build) {
    if (!build)
        return;
    lr_objfile_ctx_destroy(&build->ctx);
    free(build->code_buf);
    free(build->data_buf);
    memset(build, 0, sizeof(*build));
}

static int write_object_payload(FILE *out, const lr_target_t *target,
                                const lr_obj_build_result_t *build) {
    if (!out || !target || !build)
        return -1;
#ifdef __APPLE__
    if (strcmp(target->name, "aarch64") == 0) {
        return write_macho(out, build->code_buf, build->code_pos,
                           build->has_data ? build->data_buf : NULL,
                           build->has_data ? build->data_pos : 0,
                           (lr_objfile_ctx_t *)&build->ctx, 0x0100000Cu,
                           macho_reloc_arm64);
    }
    return -1;
#else
    if (strcmp(target->name, "x86_64") == 0) {
        return write_elf(out, build->code_buf, build->code_pos,
                         build->has_data ? build->data_buf : NULL,
                         build->has_data ? build->data_pos : 0,
                         (lr_objfile_ctx_t *)&build->ctx, 62,
                         elf_reloc_x86_64);
    }
    if (strcmp(target->name, "aarch64") == 0) {
        return write_elf(out, build->code_buf, build->code_pos,
                         build->has_data ? build->data_buf : NULL,
                         build->has_data ? build->data_pos : 0,
                         (lr_objfile_ctx_t *)&build->ctx, 183,
                         elf_reloc_aarch64);
    }
    return -1;
#endif
}

#ifdef __APPLE__
static int run_codesign_adhoc(const char *path) {
    char *const argv[] = {
        "/usr/bin/codesign", "--force", "--sign", "-", (char *)path, NULL
    };
    int status = -1;
    if (!path || !path[0])
        return -1;
    if (lr_platform_run_process(argv, true, &status) != 0)
        return -1;
    return status;
}

static int copy_file_to_stream(const char *path, FILE *out) {
    FILE *in = NULL;
    uint8_t buf[4096];
    size_t nread;
    if (!path || !out)
        return -1;
    in = fopen(path, "rb");
    if (!in)
        return -1;
    while ((nread = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, nread, out) != nread) {
            fclose(in);
            return -1;
        }
    }
    if (ferror(in)) {
        fclose(in);
        return -1;
    }
    fclose(in);
    return fflush(out) == 0 ? 0 : -1;
}
#endif

static int obj_build_module(lr_module_t *m, const lr_target_t *target,
                            bool preserve_symbol_names,
                            lr_obj_build_result_t *out) {
    if (!m || !target || !out)
        return -1;

    memset(out, 0, sizeof(*out));
    out->code_buf = (uint8_t *)calloc(1, OBJ_CODE_BUF_SIZE);
    out->data_buf = (uint8_t *)calloc(1, OBJ_DATA_BUF_SIZE);
    if (!out->code_buf || !out->data_buf) {
        obj_build_result_destroy(out);
        return -1;
    }

    lr_arena_t *arena = lr_arena_create(0);
    if (!arena) {
        obj_build_result_destroy(out);
        return -1;
    }

    out->ctx.preserve_symbol_names = preserve_symbol_names;
    m->obj_ctx = &out->ctx;
    if (lr_obj_build_symbol_cache(&out->ctx, m) != 0) {
        m->obj_ctx = NULL;
        lr_arena_destroy(arena);
        obj_build_result_destroy(out);
        return -1;
    }

    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (f->is_decl || !f->first_block)
            continue;

        uint32_t sym_idx = lr_obj_ensure_symbol(&out->ctx, f->name, true, 1,
                                                (uint32_t)out->code_pos);
        if (sym_idx == UINT32_MAX) {
            m->obj_ctx = NULL;
            lr_arena_destroy(arena);
            obj_build_result_destroy(out);
            return -1;
        }

        uint32_t reloc_base = out->ctx.num_relocs;

        size_t func_len = 0;
        int rc = lr_target_compile(target, LR_COMPILE_ISEL, f, m,
                                   out->code_buf + out->code_pos,
                                   OBJ_CODE_BUF_SIZE - out->code_pos,
                                   &func_len, arena);
        if (rc != 0) {
            m->obj_ctx = NULL;
            lr_arena_destroy(arena);
            obj_build_result_destroy(out);
            return -1;
        }

        for (uint32_t ri = reloc_base; ri < out->ctx.num_relocs; ri++)
            out->ctx.relocs[ri].offset += (uint32_t)out->code_pos;

        out->code_pos += func_len;
    }

    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (!f->is_decl && f->first_block)
            continue;
        if (lr_obj_ensure_symbol(&out->ctx, f->name, false, 0, 0) == UINT32_MAX) {
            m->obj_ctx = NULL;
            lr_arena_destroy(arena);
            obj_build_result_destroy(out);
            return -1;
        }
    }

    if (obj_define_intrinsic_stubs(out, target) != 0) {
        m->obj_ctx = NULL;
        lr_arena_destroy(arena);
        obj_build_result_destroy(out);
        return -1;
    }

    for (lr_global_t *g = m->first_global; g; g = g->next) {
        if (g->is_external) {
            if (lr_obj_ensure_symbol(&out->ctx, g->name, false, 0, 0) == UINT32_MAX) {
                m->obj_ctx = NULL;
                lr_arena_destroy(arena);
                obj_build_result_destroy(out);
                return -1;
            }
            continue;
        }

        size_t gsize = lr_type_size(g->type);
        if (gsize == 0)
            gsize = 8;

        size_t galign = lr_type_align(g->type);
        if (galign == 0)
            galign = 8;
        /* Globals with pointer relocations must be at least
           pointer-aligned so the linker can patch 8-byte addresses. */
        if (g->relocs && galign < 8)
            galign = 8;

        out->data_pos = obj_align_up(out->data_pos, galign);

        if (out->data_pos + gsize > OBJ_DATA_BUF_SIZE) {
            m->obj_ctx = NULL;
            lr_arena_destroy(arena);
            obj_build_result_destroy(out);
            return -1;
        }

        if (g->init_data && g->init_size > 0) {
            size_t copy_n = g->init_size < gsize ? g->init_size : gsize;
            memcpy(out->data_buf + out->data_pos, g->init_data, copy_n);
        }

        uint32_t gsym = lr_obj_ensure_symbol(&out->ctx, g->name, true, 2,
                                             (uint32_t)out->data_pos);
        if (gsym == UINT32_MAX) {
            m->obj_ctx = NULL;
            lr_arena_destroy(arena);
            obj_build_result_destroy(out);
            return -1;
        }
        out->ctx.symbols[gsym].is_local = g->is_local;

        for (lr_reloc_t *rel = g->relocs; rel; rel = rel->next) {
            uint32_t sym_idx = lr_obj_ensure_symbol(
                &out->ctx, rel->symbol_name, false, 0, 0);
            if (sym_idx == UINT32_MAX) {
                m->obj_ctx = NULL;
                lr_arena_destroy(arena);
                obj_build_result_destroy(out);
                return -1;
            }
            uint8_t abs64_reloc = LR_RELOC_X86_64_64;
            if (strcmp(target->name, "aarch64") == 0)
                abs64_reloc = LR_RELOC_ARM64_ABS64;
            lr_obj_add_data_reloc(&out->ctx,
                                  (uint32_t)(out->data_pos + rel->offset),
                                  sym_idx, abs64_reloc);
        }

        out->data_pos += gsize;
        out->has_data = true;
    }

    m->obj_ctx = NULL;
    lr_arena_destroy(arena);
    return 0;
}

static int obj_build_from_blobs(const lr_func_blob_t *blobs,
                               uint32_t num_blobs,
                               lr_module_t *m,
                               const lr_target_t *target,
                               bool preserve_symbol_names,
                               lr_obj_build_result_t *out) {
    lr_arena_t *compile_arena = NULL;
    lr_compile_mode_t extra_mode = LR_COMPILE_ISEL;
    if (!blobs || num_blobs == 0 || !m || !target || !out)
        return -1;

    memset(out, 0, sizeof(*out));
    out->code_buf = (uint8_t *)calloc(1, OBJ_CODE_BUF_SIZE);
    out->data_buf = (uint8_t *)calloc(1, OBJ_DATA_BUF_SIZE);
    if (!out->code_buf || !out->data_buf) {
        obj_build_result_destroy(out);
        return -1;
    }

    out->ctx.preserve_symbol_names = preserve_symbol_names;
    const int verbose_blob = (getenv("LIRIC_VERBOSE_BLOB_LINK") != NULL);
    if (lr_obj_build_symbol_cache(&out->ctx, m) != 0) {
        obj_build_result_destroy(out);
        return -1;
    }
    m->obj_ctx = &out->ctx;
    extra_mode = lr_compile_mode_from_env();
    if (extra_mode == LR_COMPILE_LLVM ||
        !lr_target_can_compile(target, extra_mode)) {
        extra_mode = LR_COMPILE_ISEL;
    }

    for (uint32_t bi = 0; bi < num_blobs; bi++) {
        const lr_func_blob_t *blob = &blobs[bi];
        if (!blob->name || !blob->code || blob->code_len == 0)
            continue;

        out->code_pos = obj_align_up(out->code_pos, 16);
        if (out->code_pos + blob->code_len > OBJ_CODE_BUF_SIZE) {
            obj_build_result_destroy(out);
            return -1;
        }

        uint32_t sym_idx = lr_obj_ensure_symbol(&out->ctx, blob->name,
                                                true, 1,
                                                (uint32_t)out->code_pos);
        if (sym_idx == UINT32_MAX) {
            obj_build_result_destroy(out);
            return -1;
        }

        memcpy(out->code_buf + out->code_pos, blob->code, blob->code_len);

        for (uint32_t ri = 0; ri < blob->num_relocs; ri++) {
            const lr_cached_reloc_t *rel = &blob->relocs[ri];
            if (!rel->symbol_name || !rel->symbol_name[0])
                continue;
            uint32_t reloc_sym = lr_obj_ensure_symbol(&out->ctx,
                                                      rel->symbol_name,
                                                      false, 0, 0);
            if (reloc_sym == UINT32_MAX) {
                obj_build_result_destroy(out);
                return -1;
            }
            lr_obj_add_reloc(&out->ctx,
                             (uint32_t)out->code_pos + rel->offset,
                             reloc_sym, rel->type);
        }

        out->code_pos += blob->code_len;
    }

    /* Compile module-defined functions that are missing from imported blobs.
       This is needed for mixed no-link merges (e.g. C sources merged as LL)
       where no sidecar blob package exists for those definitions. */
    for (lr_func_t *f = m->first_func; f; f = f->next) {
        uint32_t sym_idx;
        uint32_t reloc_base;
        size_t func_len = 0;
        if (!f->name || !f->name[0] || f->is_decl || !f->first_block)
            continue;

        sym_idx = lr_obj_ensure_symbol(&out->ctx, f->name, false, 0, 0);
        if (sym_idx == UINT32_MAX) {
            m->obj_ctx = NULL;
            if (compile_arena) lr_arena_destroy(compile_arena);
            obj_build_result_destroy(out);
            return -1;
        }
        if (out->ctx.symbols[sym_idx].is_defined)
            continue;

        if (!compile_arena) {
            compile_arena = lr_arena_create(0);
            if (!compile_arena) {
                m->obj_ctx = NULL;
                obj_build_result_destroy(out);
                return -1;
            }
        }

        out->code_pos = obj_align_up(out->code_pos, 16);
        sym_idx = lr_obj_ensure_symbol(&out->ctx, f->name, true, 1,
                                       (uint32_t)out->code_pos);
        if (sym_idx == UINT32_MAX) {
            m->obj_ctx = NULL;
            lr_arena_destroy(compile_arena);
            obj_build_result_destroy(out);
            return -1;
        }

        reloc_base = out->ctx.num_relocs;
        if (lr_target_compile(target, extra_mode, f, m,
                              out->code_buf + out->code_pos,
                              OBJ_CODE_BUF_SIZE - out->code_pos,
                              &func_len, compile_arena) != 0) {
            m->obj_ctx = NULL;
            lr_arena_destroy(compile_arena);
            obj_build_result_destroy(out);
            return -1;
        }
        for (uint32_t ri = reloc_base; ri < out->ctx.num_relocs; ri++)
            out->ctx.relocs[ri].offset += (uint32_t)out->code_pos;
        out->code_pos += func_len;
    }

    /* Declare external functions that aren't in the blob list */
    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (!f->name || !f->name[0])
            continue;
        if (f->is_decl || !f->first_block) {
            if (lr_obj_ensure_symbol(&out->ctx, f->name, false, 0, 0) ==
                UINT32_MAX) {
                m->obj_ctx = NULL;
                if (compile_arena) lr_arena_destroy(compile_arena);
                obj_build_result_destroy(out);
                return -1;
            }
        }
    }

    if (obj_define_intrinsic_stubs(out, target) != 0) {
        m->obj_ctx = NULL;
        if (compile_arena) lr_arena_destroy(compile_arena);
        obj_build_result_destroy(out);
        return -1;
    }

    /* Globals (same as obj_build_module) */
    for (lr_global_t *g = m->first_global; g; g = g->next) {
        if (verbose_blob) {
            uint32_t nrel = 0;
            for (lr_reloc_t *r = g->relocs; r; r = r->next)
                nrel++;
            fprintf(stderr,
                    "obj_build_from_blobs: global name=%s external=%d local=%d const=%d init_size=%zu relocs=%u\n",
                    g->name ? g->name : "<null>",
                    g->is_external ? 1 : 0,
                    g->is_local ? 1 : 0,
                    g->is_const ? 1 : 0,
                    g->init_size,
                    nrel);
        }
        if (g->is_external) {
            if (lr_obj_ensure_symbol(&out->ctx, g->name, false, 0, 0) ==
                UINT32_MAX) {
                m->obj_ctx = NULL;
                if (compile_arena) lr_arena_destroy(compile_arena);
                obj_build_result_destroy(out);
                return -1;
            }
            continue;
        }

        size_t gsize = lr_type_size(g->type);
        if (gsize == 0)
            gsize = 8;
        size_t galign = lr_type_align(g->type);
        if (galign == 0)
            galign = 8;
        if (g->relocs && galign < 8)
            galign = 8;
        out->data_pos = obj_align_up(out->data_pos, galign);
        if (out->data_pos + gsize > OBJ_DATA_BUF_SIZE) {
            m->obj_ctx = NULL;
            if (compile_arena) lr_arena_destroy(compile_arena);
            obj_build_result_destroy(out);
            return -1;
        }

        if (g->init_data && g->init_size > 0) {
            size_t copy_n = g->init_size < gsize ? g->init_size : gsize;
            memcpy(out->data_buf + out->data_pos, g->init_data, copy_n);
        }

        uint32_t gsym = lr_obj_ensure_symbol(&out->ctx, g->name, true, 2,
                                             (uint32_t)out->data_pos);
        if (gsym == UINT32_MAX) {
            m->obj_ctx = NULL;
            if (compile_arena) lr_arena_destroy(compile_arena);
            obj_build_result_destroy(out);
            return -1;
        }
        out->ctx.symbols[gsym].is_local = g->is_local;

        for (lr_reloc_t *rel = g->relocs; rel; rel = rel->next) {
            uint32_t rel_sym = lr_obj_ensure_symbol(
                &out->ctx, rel->symbol_name, false, 0, 0);
            if (rel_sym == UINT32_MAX) {
                m->obj_ctx = NULL;
                if (compile_arena) lr_arena_destroy(compile_arena);
                obj_build_result_destroy(out);
                return -1;
            }
            uint8_t abs64_reloc = LR_RELOC_X86_64_64;
            if (strcmp(target->name, "aarch64") == 0)
                abs64_reloc = LR_RELOC_ARM64_ABS64;
            lr_obj_add_data_reloc(&out->ctx,
                                  (uint32_t)(out->data_pos + rel->offset),
                                  rel_sym, abs64_reloc);
        }

        out->data_pos += gsize;
        out->has_data = true;
    }

    if (verbose_blob) {
        fprintf(stderr, "obj_build_from_blobs: symbols after merge (%u total)\n",
                out->ctx.num_symbols);
        for (uint32_t si = 0; si < out->ctx.num_symbols; si++) {
            const lr_obj_symbol_t *sym = &out->ctx.symbols[si];
            fprintf(stderr, "  [%u] %s defined=%d section=%u off=%u local=%d weak=%d\n",
                    si,
                    sym->name ? sym->name : "<null>",
                    sym->is_defined ? 1 : 0,
                    sym->section,
                    sym->offset,
                    sym->is_local ? 1 : 0,
                    sym->is_weak ? 1 : 0);
        }
        fprintf(stderr, "obj_build_from_blobs: code relocs (%u total)\n",
                out->ctx.num_relocs);
        for (uint32_t ri = 0; ri < out->ctx.num_relocs; ri++) {
            const lr_obj_reloc_t *rel = &out->ctx.relocs[ri];
            const char *name = (rel->symbol_idx < out->ctx.num_symbols &&
                                out->ctx.symbols[rel->symbol_idx].name)
                                   ? out->ctx.symbols[rel->symbol_idx].name
                                   : "<invalid>";
            fprintf(stderr, "  code[%u]: off=%u type=%u sym=%u (%s)\n",
                    ri, rel->offset, rel->type, rel->symbol_idx, name);
        }
        fprintf(stderr, "obj_build_from_blobs: data relocs (%u total)\n",
                out->ctx.num_data_relocs);
        for (uint32_t ri = 0; ri < out->ctx.num_data_relocs; ri++) {
            const lr_obj_reloc_t *rel = &out->ctx.data_relocs[ri];
            const char *name = (rel->symbol_idx < out->ctx.num_symbols &&
                                out->ctx.symbols[rel->symbol_idx].name)
                                   ? out->ctx.symbols[rel->symbol_idx].name
                                   : "<invalid>";
            fprintf(stderr, "  data[%u]: off=%u type=%u sym=%u (%s)\n",
                    ri, rel->offset, rel->type, rel->symbol_idx, name);
        }
    }

    m->obj_ctx = NULL;
    if (compile_arena)
        lr_arena_destroy(compile_arena);
    return 0;
}

int lr_emit_object_from_blobs(const lr_func_blob_t *blobs,
                              uint32_t num_blobs,
                              lr_module_t *m,
                              const lr_target_t *target,
                              FILE *out) {
    if (!blobs || !m || !target || !out)
        return -1;

    lr_obj_build_result_t build;
    if (obj_build_from_blobs(blobs, num_blobs, m, target, false, &build) != 0)
        return -1;

    int result = write_object_payload(out, target, &build);
    obj_build_result_destroy(&build);
    return result;
}

int lr_emit_executable_from_blobs(const lr_func_blob_t *blobs,
                                  uint32_t num_blobs,
                                  lr_module_t *m,
                                  const lr_target_t *target,
                                  FILE *out,
                                  const char *entry_symbol) {
    if (!blobs || !m || !target || !out)
        return -1;
    if (!entry_symbol || !entry_symbol[0])
        entry_symbol = "main";

    lr_obj_build_result_t build;
    if (obj_build_from_blobs(blobs, num_blobs, m, target, true, &build) != 0)
        return -1;

    for (uint32_t i = 0; i < build.ctx.num_symbols; i++) {
        if (build.ctx.symbols[i].is_defined)
            continue;
        const char *orig = build.ctx.symbols[i].name;
        const char *mapped = remap_intrinsic(orig);
        if (mapped != orig)
            build.ctx.symbols[i].name = mapped;
    }

    int result = -1;
#if defined(__linux__)
    if (strcmp(target->name, "x86_64") == 0) {
        bool has_undef = false;
        for (uint32_t i = 0; i < build.ctx.num_symbols; i++) {
            if (!build.ctx.symbols[i].is_defined) {
                has_undef = true;
                break;
            }
        }
        if (has_undef) {
            result = write_elf_dynamic_executable_x86_64(
                out, build.code_buf, build.code_pos,
                build.has_data ? build.data_buf : NULL,
                build.has_data ? build.data_pos : 0,
                &build.ctx, entry_symbol);
        } else {
            result = write_elf_executable_x86_64(
                out, build.code_buf, build.code_pos,
                build.has_data ? build.data_buf : NULL,
                build.has_data ? build.data_pos : 0,
                &build.ctx, entry_symbol);
        }
    } else if (strcmp(target->name, "aarch64") == 0) {
        result = write_elf_executable_aarch64(
            out, build.code_buf, build.code_pos,
            build.has_data ? build.data_buf : NULL,
            build.has_data ? build.data_pos : 0,
            &build.ctx, entry_symbol);
    } else if (strncmp(target->name, "riscv64", 7) == 0) {
        result = write_elf_executable_riscv64(
            out, build.code_buf, build.code_pos,
            build.has_data ? build.data_buf : NULL,
            build.has_data ? build.data_pos : 0,
            &build.ctx, entry_symbol);
    }
#else
    if (strcmp(target->name, "aarch64") == 0) {
        char exe_tpl[] = "/tmp/liric_exe_XXXXXX";
        int exe_fd = mkstemp(exe_tpl);
        FILE *exe_out = NULL;
        uint8_t *exec_code = NULL;
        size_t exec_code_size = 0;
        if (exe_fd < 0) goto blob_done;
        exe_out = fdopen(exe_fd, "wb");
        if (!exe_out) { close(exe_fd); goto blob_done; }
        exe_fd = -1;
#if !defined(_WIN32)
        if (build_macho_exec_payload_aarch64(&build, &exec_code,
                                             &exec_code_size) != 0)
            goto blob_done;
#else
        goto blob_done;
#endif
        result = write_macho_executable_arm64(
            exe_out, exec_code, exec_code_size,
            NULL, 0,
            &build.ctx, entry_symbol);
        free(exec_code);
        exec_code = NULL;
        if (fclose(exe_out) != 0) result = -1;
        exe_out = NULL;
        if (result != 0) goto blob_done;
        if (run_codesign_adhoc(exe_tpl) != 0) { result = -1; goto blob_done; }
        if (copy_file_to_stream(exe_tpl, out) != 0) { result = -1; goto blob_done; }
        result = 0;
blob_done:
        free(exec_code);
        if (exe_out) fclose(exe_out);
        if (exe_fd >= 0) close(exe_fd);
        unlink(exe_tpl);
    }
#endif

    obj_build_result_destroy(&build);
    return result;
}

int lr_emit_object(lr_module_t *m, const lr_target_t *target, FILE *out) {
    if (!m || !target || !out)
        return -1;

    lr_obj_build_result_t build;
    if (obj_build_module(m, target, false, &build) != 0)
        return -1;

    int result = write_object_payload(out, target, &build);

    obj_build_result_destroy(&build);
    return result;
}

int lr_emit_executable(lr_module_t *m, const lr_target_t *target, FILE *out,
                       const char *entry_symbol) {
    if (!m || !target || !out)
        return -1;
    if (!entry_symbol || !entry_symbol[0])
        entry_symbol = "main";

    lr_obj_build_result_t build;
    if (obj_build_module(m, target, true, &build) != 0)
        return -1;

    /* Remap remaining undefined llvm.* intrinsics to libc equivalents.
       This runs after intrinsic stub embedding, so only truly unresolved
       intrinsics (like llvm.memcpy) get remapped. */
    for (uint32_t i = 0; i < build.ctx.num_symbols; i++) {
        if (build.ctx.symbols[i].is_defined)
            continue;
        const char *orig = build.ctx.symbols[i].name;
        const char *mapped = remap_intrinsic(orig);
        if (mapped != orig)
            build.ctx.symbols[i].name = mapped;
    }

    int result = -1;
#if defined(__linux__)
    if (strcmp(target->name, "x86_64") == 0) {
        bool has_undef = false;
        for (uint32_t i = 0; i < build.ctx.num_symbols; i++) {
            if (!build.ctx.symbols[i].is_defined) {
                has_undef = true;
                break;
            }
        }
        if (has_undef) {
            result = write_elf_dynamic_executable_x86_64(
                out, build.code_buf, build.code_pos,
                build.has_data ? build.data_buf : NULL,
                build.has_data ? build.data_pos : 0,
                &build.ctx, entry_symbol);
        } else {
            result = write_elf_executable_x86_64(
                out, build.code_buf, build.code_pos,
                build.has_data ? build.data_buf : NULL,
                build.has_data ? build.data_pos : 0,
                &build.ctx, entry_symbol);
        }
    } else if (strcmp(target->name, "aarch64") == 0) {
        result = write_elf_executable_aarch64(
            out, build.code_buf, build.code_pos,
            build.has_data ? build.data_buf : NULL,
            build.has_data ? build.data_pos : 0,
            &build.ctx, entry_symbol);
    } else if (strncmp(target->name, "riscv64", 7) == 0) {
        result = write_elf_executable_riscv64(
            out, build.code_buf, build.code_pos,
            build.has_data ? build.data_buf : NULL,
            build.has_data ? build.data_pos : 0,
            &build.ctx, entry_symbol);
    }
#else
    if (strcmp(target->name, "aarch64") == 0) {
        char exe_tpl[] = "/tmp/liric_exe_XXXXXX";
        int exe_fd = -1;
        FILE *exe_out = NULL;
        int sign_rc;
        int copy_rc;
        uint8_t *exec_code = NULL;
        size_t exec_code_size = 0;

        exe_fd = mkstemp(exe_tpl);
        if (exe_fd < 0)
            goto done;
        exe_out = fdopen(exe_fd, "wb");
        if (!exe_out) {
            close(exe_fd);
            exe_fd = -1;
            goto done;
        }
        exe_fd = -1;

#if !defined(_WIN32)
        if (build_macho_exec_payload_aarch64(&build, &exec_code,
                                             &exec_code_size) != 0)
            goto done;
#else
        goto done;
#endif
        result = write_macho_executable_arm64(
            exe_out, exec_code, exec_code_size,
            NULL, 0,
            &build.ctx, entry_symbol
        );
        free(exec_code);
        exec_code = NULL;
        if (fclose(exe_out) != 0)
            result = -1;
        exe_out = NULL;
        if (result != 0)
            goto done;

        sign_rc = run_codesign_adhoc(exe_tpl);
        if (sign_rc != 0) {
            result = -1;
            goto done;
        }

        copy_rc = copy_file_to_stream(exe_tpl, out);
        if (copy_rc != 0) {
            result = -1;
            goto done;
        }
        result = 0;

done:
        free(exec_code);
        if (exe_out)
            fclose(exe_out);
        if (exe_fd >= 0)
            close(exe_fd);
        unlink(exe_tpl);
    }
#endif

    obj_build_result_destroy(&build);
    return result;
}

int lr_emit_executable_with_runtime(lr_module_t *m, const char *runtime_ll,
                                     size_t runtime_len,
                                     const lr_target_t *target, FILE *out,
                                     const char *entry_symbol) {
    if (!m || !runtime_ll || runtime_len == 0 || !target || !out)
        return -1;

    char parse_err[256] = {0};
    lr_module_t *rt = lr_parse_ll(runtime_ll, runtime_len, parse_err,
                                   sizeof(parse_err));
    if (!rt)
        return -1;

    if (lr_module_merge(m, rt) != 0) {
        lr_module_free(rt);
        return -1;
    }
    lr_module_free(rt);

    return lr_emit_executable(m, target, out, entry_symbol);
}
