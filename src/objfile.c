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
#ifdef __APPLE__
#include <fcntl.h>
#include <limits.h>
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

static int patch_aarch64_branch26_exec(uint8_t *buf, size_t buflen, uint32_t off,
                                       uint64_t place_addr, uint64_t target_addr) {
    int64_t diff = (int64_t)target_addr - (int64_t)place_addr;
    int64_t imm26;
    uint32_t insn;
    if ((size_t)off + 4 > buflen)
        return -1;
    if ((diff & 0x3) != 0)
        return -1;
    imm26 = diff >> 2;
    if (imm26 < -(1LL << 25) || imm26 >= (1LL << 25))
        return -1;
    insn = read_u32_le(buf, off);
    insn &= ~0x03ffffffu;
    insn |= ((uint32_t)imm26 & 0x03ffffffu);
    return write_u32_le(buf, buflen, off, insn);
}

static int patch_aarch64_add_imm12_exec(uint8_t *buf, size_t buflen, uint32_t off,
                                        uint64_t target_addr) {
    uint32_t insn;
    uint32_t rn;
    uint32_t rd;
    uint32_t imm12;
    uint32_t add;
    if ((size_t)off + 4 > buflen)
        return -1;
    insn = read_u32_le(buf, off);
    rn = (insn >> 5) & 0x1fu;
    rd = insn & 0x1fu;
    imm12 = (uint32_t)(target_addr & 0xfffu);
    add = 0x91000000u | ((imm12 & 0xfffu) << 10) | (rn << 5) | rd;
    return write_u32_le(buf, buflen, off, add);
}

static uint32_t encode_aarch64_movz(uint32_t rd, uint16_t imm16,
                                    uint32_t shift_bits) {
    uint32_t hw = (shift_bits / 16u) & 0x3u;
    return 0xD2800000u | (hw << 21) | ((uint32_t)imm16 << 5) | (rd & 0x1fu);
}

static uint32_t encode_aarch64_movk(uint32_t rd, uint16_t imm16,
                                    uint32_t shift_bits) {
    uint32_t hw = (shift_bits / 16u) & 0x3u;
    return 0xF2800000u | (hw << 21) | ((uint32_t)imm16 << 5) | (rd & 0x1fu);
}

static int add_unique_u32(uint32_t *vals, size_t *count, size_t cap,
                          uint32_t v) {
    size_t i;
    if (!vals || !count)
        return -1;
    for (i = 0; i < *count; i++) {
        if (vals[i] == v)
            return 0;
    }
    if (*count >= cap)
        return -1;
    vals[*count] = v;
    (*count)++;
    return 0;
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
    const char *intrinsic_lookup = NULL;
    void *addr = NULL;
    const int verbose = (getenv("LIRIC_VERBOSE_BLOB_LINK") != NULL);
    if (!lookup || !lookup[0])
        return NULL;

    /* AOT emission must only embed process-stable symbol addresses.
       Do not use builtin intrinsic fallbacks here: those are pointers to
       functions in the liric compiler process and are invalid in emitted
       executables. */
    intrinsic_lookup = lr_platform_intrinsic_libc_name(lookup);
    if (intrinsic_lookup && intrinsic_lookup[0] &&
        strcmp(intrinsic_lookup, lookup) != 0) {
        addr = lr_platform_dlsym_default(intrinsic_lookup);
        if (!addr && intrinsic_lookup[0] == '_')
            addr = lr_platform_dlsym_default(intrinsic_lookup + 1);
        if (!addr && runtime_handle) {
            addr = lr_platform_dlsym(runtime_handle, intrinsic_lookup);
            if (!addr && intrinsic_lookup[0] == '_')
                addr = lr_platform_dlsym(runtime_handle, intrinsic_lookup + 1);
        }
        if (verbose && !addr) {
            fprintf(stderr,
                    "macho_exec_payload: intrinsic dlsym miss '%s' (from '%s')\n",
                    intrinsic_lookup, lookup);
        }
        if (addr)
            return addr;
    }

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

static const char *normalize_exec_symbol_name(const char *name) {
    if (!name)
        return NULL;
    while (*name == '\1' || *name == '_')
        name++;
    return name;
}

static bool exec_symbol_name_matches(const char *lhs, const char *rhs) {
    lhs = normalize_exec_symbol_name(lhs);
    rhs = normalize_exec_symbol_name(rhs);
    if (!lhs || !rhs)
        return false;
    return strcmp(lhs, rhs) == 0;
}

static int build_macho_exec_payload_aarch64(lr_obj_build_result_t *build,
                                            const char *entry_symbol,
                                            uint8_t **out_code,
                                            size_t *out_code_size,
                                            uint8_t **out_data,
                                            size_t *out_data_size) {
    uint8_t *code_buf = NULL;
    uint8_t *data_buf = NULL;
    uint64_t *sym_addr = NULL;
    uint32_t *got_slot_off = NULL;
    uint32_t *stub_off = NULL;
    uint8_t *sym_needed = NULL;
    const lr_exec_stub_t **stub_for_sym = NULL;
    uint32_t *slide_slot_off = NULL;
    size_t slide_slot_count = 0;
    size_t slide_slot_cap = 0;
    uint32_t init_fn_sym_idx = UINT32_MAX;
    uint32_t entry_sym_idx = UINT32_MAX;
    uint64_t init_fn_real_addr = 0;
    bool reroute_entry_symbol = false;
    size_t code_size = 0;
    size_t data_runtime_size = 0;
    size_t got_off = 0;
    void *runtime_handle = NULL;
    const int verbose = (getenv("LIRIC_VERBOSE_BLOB_LINK") != NULL);
    const size_t page = 0x4000u;
    const size_t text_off = lr_macho_executable_text_offset_arm64();
    /* PIE slide fixups may append a text stub after code emission. Reserve
       enough text-file space up front so data_base stays stable. */
    const size_t max_slide_slots = (size_t)build->ctx.num_data_relocs;
    const size_t max_stub_insns = max_slide_slots > 0 ? (13u + (8u * max_slide_slots)) : 0u;
    const size_t max_stub_size = max_stub_insns * 4u;
    const size_t text_file_size =
        obj_align_up(text_off + build->code_pos + max_stub_size, page);
    const uint64_t text_base = MACHO_ARM64_EXEC_IMAGE_BASE + (uint64_t)text_off;
    const uint64_t data_base = MACHO_ARM64_EXEC_IMAGE_BASE + (uint64_t)text_file_size;
    const char *entry_name = (entry_symbol && entry_symbol[0])
                                 ? entry_symbol
                                 : "main";
#define FAIL_BUILD(msg) do { \
    if (verbose) fprintf(stderr, "macho_exec_payload: %s\n", (msg)); \
    goto fail; \
} while (0)

    if (!build || !out_code || !out_code_size || !out_data || !out_data_size)
        return -1;
    *out_code = NULL;
    *out_code_size = 0;
    *out_data = NULL;
    *out_data_size = 0;

    code_size = build->code_pos;
    data_runtime_size = obj_align_up(build->data_pos, 8u);
    got_off = data_runtime_size;

    got_slot_off = (uint32_t *)calloc(build->ctx.num_symbols, sizeof(uint32_t));
    sym_addr = (uint64_t *)calloc(build->ctx.num_symbols, sizeof(uint64_t));
    stub_off = (uint32_t *)calloc(build->ctx.num_symbols, sizeof(uint32_t));
    sym_needed = (uint8_t *)calloc(build->ctx.num_symbols, sizeof(uint8_t));
    stub_for_sym = (const lr_exec_stub_t **)calloc(build->ctx.num_symbols,
                                                   sizeof(lr_exec_stub_t *));
    slide_slot_cap = (size_t)build->ctx.num_data_relocs + (size_t)build->ctx.num_symbols + 4u;
    slide_slot_off = (uint32_t *)calloc(slide_slot_cap, sizeof(uint32_t));
    if (!got_slot_off || !sym_addr || !stub_off || !sym_needed || !stub_for_sym ||
        !slide_slot_off)
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
    data_runtime_size = got_off;

    code_buf = (uint8_t *)calloc(1, code_size > 0 ? code_size : 1u);
    data_buf = (uint8_t *)calloc(1, data_runtime_size > 0 ? data_runtime_size : 1u);
    if (!code_buf || !data_buf)
        FAIL_BUILD("buffer allocation failed");
    memcpy(code_buf, build->code_buf, build->code_pos);
    if (build->has_data && build->data_pos > 0)
        memcpy(data_buf, build->data_buf, build->data_pos);

    for (uint32_t si = 0; si < build->ctx.num_symbols; si++) {
        if (stub_off[si] == UINT32_MAX || !stub_for_sym[si])
            continue;
        memcpy(data_buf + stub_off[si], stub_for_sym[si]->bytes,
               stub_for_sym[si]->size);
    }

    for (uint32_t si = 0; si < build->ctx.num_symbols; si++) {
        const lr_obj_symbol_t *sym = &build->ctx.symbols[si];
        if (sym->is_defined) {
            if (sym->section == 1) {
                sym_addr[si] = text_base + sym->offset;
                if (entry_sym_idx == UINT32_MAX &&
                    exec_symbol_name_matches(sym->name, entry_name)) {
                    entry_sym_idx = si;
                }
                if (init_fn_sym_idx == UINT32_MAX &&
                    (strcmp(sym->name, "_lpython_call_initial_functions") == 0 ||
                     strcmp(sym->name, "lpython_call_initial_functions") == 0)) {
                    init_fn_sym_idx = si;
                }
            } else if (sym->section == 2) {
                if ((size_t)sym->offset >= data_runtime_size)
                    FAIL_BUILD("data symbol offset out of range");
                sym_addr[si] = data_base + sym->offset;
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
            sym_addr[si] = data_base + stub_off[si];
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
        if (verbose) {
            fprintf(stderr,
                    "macho_exec_payload: resolved external symbol=%s addr=0x%llx\n",
                    sym->name ? sym->name : "<null>",
                    (unsigned long long)sym_addr[si]);
        }
    }

    for (uint32_t si = 0; si < build->ctx.num_symbols; si++) {
        if (got_slot_off[si] == UINT32_MAX)
            continue;
        if (write_u64_le(data_buf, data_runtime_size,
                         got_slot_off[si], sym_addr[si]) != 0)
            FAIL_BUILD("writing GOT slot failed");
    }

    for (uint32_t ri = 0; ri < build->ctx.num_relocs; ri++) {
        const lr_obj_reloc_t *rel = &build->ctx.relocs[ri];
        const lr_obj_symbol_t *sym = NULL;
        uint64_t place_addr = text_base + rel->offset;
        uint64_t target_addr = 0;
        if (rel->symbol_idx >= build->ctx.num_symbols) {
            if (verbose) {
                fprintf(stderr, "macho_exec_payload: bad reloc symbol idx rel=%u type=%u off=%u\n",
                        ri, rel->type, rel->offset);
            }
            FAIL_BUILD("bad code reloc symbol index");
        }
        sym = &build->ctx.symbols[rel->symbol_idx];
        switch (rel->type) {
        case LR_RELOC_ARM64_PAGE21:
            target_addr = sym_addr[rel->symbol_idx];
            if (patch_aarch64_page21_exec(code_buf, code_size, rel->offset,
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
            if (patch_aarch64_pageoff12_exec(code_buf, code_size, rel->offset,
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
            if (sym->is_defined) {
                target_addr = sym_addr[rel->symbol_idx];
            } else {
                if (got_slot_off[rel->symbol_idx] == UINT32_MAX)
                    FAIL_BUILD("missing GOT slot for PAGE21");
                target_addr = data_base + got_slot_off[rel->symbol_idx];
            }
            if (patch_aarch64_page21_exec(code_buf, code_size, rel->offset,
                                          place_addr, target_addr) != 0) {
                if (verbose) {
                    if (sym->is_defined) {
                        fprintf(stderr, "macho_exec_payload: local PAGE21 patch failed rel=%u off=%u sym=%u target=0x%llx\n",
                                ri, rel->offset, rel->symbol_idx,
                                (unsigned long long)target_addr);
                    } else {
                        fprintf(stderr, "macho_exec_payload: GOT PAGE21 patch failed rel=%u off=%u sym=%u got_off=%u\n",
                                ri, rel->offset, rel->symbol_idx,
                                got_slot_off[rel->symbol_idx]);
                    }
                }
                FAIL_BUILD(sym->is_defined
                               ? "code local PAGE21 patch failed"
                               : "code GOT PAGE21 patch failed");
            }
            break;
        case LR_RELOC_ARM64_GOT_LOAD_PAGEOFF12:
            if (sym->is_defined) {
                target_addr = sym_addr[rel->symbol_idx];
                if (patch_aarch64_add_imm12_exec(code_buf, code_size, rel->offset,
                                                 target_addr) != 0) {
                    if (verbose) {
                        fprintf(stderr, "macho_exec_payload: local ADD patch failed rel=%u off=%u sym=%u target=0x%llx\n",
                                ri, rel->offset, rel->symbol_idx,
                                (unsigned long long)target_addr);
                    }
                    FAIL_BUILD("code local ADD patch failed");
                }
                break;
            }
            if (got_slot_off[rel->symbol_idx] == UINT32_MAX)
                FAIL_BUILD("missing GOT slot for PAGEOFF12");
            target_addr = data_base + got_slot_off[rel->symbol_idx];
            if (patch_aarch64_pageoff12_exec(code_buf, code_size, rel->offset,
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
        const lr_obj_symbol_t *sym = NULL;
        uint32_t patch_off = rel->offset;
        uint64_t addend = 0;
        if (rel->symbol_idx >= build->ctx.num_symbols)
            FAIL_BUILD("bad data reloc symbol index");
        if (rel->type != LR_RELOC_ARM64_ABS64)
            FAIL_BUILD("unsupported data reloc type");
        if ((size_t)patch_off + sizeof(uint64_t) > data_runtime_size)
            FAIL_BUILD("data reloc patch offset out of range");
        sym = &build->ctx.symbols[rel->symbol_idx];
        memcpy(&addend, data_buf + patch_off, sizeof(addend));
        if (write_u64_le(data_buf, data_runtime_size, patch_off,
                         sym_addr[rel->symbol_idx] + addend) != 0)
            FAIL_BUILD("data reloc write failed");
        if (sym->is_defined &&
            add_unique_u32(slide_slot_off, &slide_slot_count, slide_slot_cap,
                           patch_off) != 0) {
            FAIL_BUILD("tracking local data reloc failed");
        }
    }

    if (slide_slot_count > 0) {
        size_t stub_insns = 13u + (8u * slide_slot_count);
        size_t stub_size = stub_insns * 4u;
        size_t old_code_size = code_size;
        size_t stub_off = old_code_size;
        uint64_t init_stub_addr = 0;
        uint8_t *new_code = NULL;
        uint32_t cur = 0;
        uint32_t call_adrp_off = 0;

        if (init_fn_sym_idx == UINT32_MAX) {
            if (entry_sym_idx != UINT32_MAX) {
                init_fn_sym_idx = entry_sym_idx;
                reroute_entry_symbol = true;
                if (verbose) {
                    fprintf(stderr,
                            "macho_exec_payload: using entry symbol '%s' for slide fixups\n",
                            build->ctx.symbols[entry_sym_idx].name
                                ? build->ctx.symbols[entry_sym_idx].name
                                : "<null>");
                }
            } else {
                FAIL_BUILD("missing init symbol for PIE slide fixups");
            }
        }
        init_fn_real_addr = sym_addr[init_fn_sym_idx];

        code_size = old_code_size + stub_size;
        if (obj_align_up(text_off + code_size, page) != text_file_size)
            FAIL_BUILD("PIE slide stub crosses text page boundary");
        new_code = (uint8_t *)realloc(code_buf, code_size);
        if (!new_code)
            FAIL_BUILD("expanding code buffer failed");
        code_buf = new_code;
        memset(code_buf + old_code_size, 0, stub_size);

        cur = (uint32_t)stub_off;
        if (write_u32_le(code_buf, code_size, cur, 0x90000009u) != 0) /* adrp x9, _mh_execute_header */
            FAIL_BUILD("emit slide stub failed");
        cur += 4u;
        if (write_u32_le(code_buf, code_size, cur, 0x91000129u) != 0) /* add x9, x9, #0 */
            FAIL_BUILD("emit slide stub failed");
        cur += 4u;
        if (write_u32_le(code_buf, code_size, cur, encode_aarch64_movz(10u, 0u, 0u)) != 0)
            FAIL_BUILD("emit slide stub failed");
        cur += 4u;
        if (write_u32_le(code_buf, code_size, cur, encode_aarch64_movk(10u, 1u, 32u)) != 0)
            FAIL_BUILD("emit slide stub failed");
        cur += 4u;
        if (write_u32_le(code_buf, code_size, cur, 0xCB0A0129u) != 0) /* sub x9, x9, x10 */
            FAIL_BUILD("emit slide stub failed");
        cur += 4u;
        if (write_u32_le(code_buf, code_size, cur, 0xD10043FFu) != 0) /* sub sp, sp, #16 */
            FAIL_BUILD("emit slide stub failed");
        cur += 4u;
        if (write_u32_le(code_buf, code_size, cur, 0xF90007FEu) != 0) /* str x30, [sp, #8] */
            FAIL_BUILD("emit slide stub failed");
        cur += 4u;

        for (size_t i = 0; i < slide_slot_count; i++) {
            uint64_t slot_addr = data_base + (uint64_t)slide_slot_off[i];
            if (write_u32_le(code_buf, code_size, cur,
                             encode_aarch64_movz(10u, (uint16_t)(slot_addr & 0xffffu), 0u)) != 0)
                FAIL_BUILD("emit slide stub failed");
            cur += 4u;
            if (write_u32_le(code_buf, code_size, cur,
                             encode_aarch64_movk(10u, (uint16_t)((slot_addr >> 16) & 0xffffu), 16u)) != 0)
                FAIL_BUILD("emit slide stub failed");
            cur += 4u;
            if (write_u32_le(code_buf, code_size, cur,
                             encode_aarch64_movk(10u, (uint16_t)((slot_addr >> 32) & 0xffffu), 32u)) != 0)
                FAIL_BUILD("emit slide stub failed");
            cur += 4u;
            if (write_u32_le(code_buf, code_size, cur,
                             encode_aarch64_movk(10u, (uint16_t)((slot_addr >> 48) & 0xffffu), 48u)) != 0)
                FAIL_BUILD("emit slide stub failed");
            cur += 4u;
            if (write_u32_le(code_buf, code_size, cur, 0x8B09014Au) != 0) /* add x10, x10, x9 */
                FAIL_BUILD("emit slide stub failed");
            cur += 4u;
            if (write_u32_le(code_buf, code_size, cur, 0xF940014Bu) != 0) /* ldr x11, [x10] */
                FAIL_BUILD("emit slide stub failed");
            cur += 4u;
            if (write_u32_le(code_buf, code_size, cur, 0x8B09016Bu) != 0) /* add x11, x11, x9 */
                FAIL_BUILD("emit slide stub failed");
            cur += 4u;
            if (write_u32_le(code_buf, code_size, cur, 0xF900014Bu) != 0) /* str x11, [x10] */
                FAIL_BUILD("emit slide stub failed");
            cur += 4u;
        }

        call_adrp_off = cur;
        if (write_u32_le(code_buf, code_size, cur, 0x90000010u) != 0) /* adrp x16, init */
            FAIL_BUILD("emit slide stub failed");
        cur += 4u;
        if (write_u32_le(code_buf, code_size, cur, 0x91000210u) != 0) /* add x16, x16, #0 */
            FAIL_BUILD("emit slide stub failed");
        cur += 4u;
        if (write_u32_le(code_buf, code_size, cur, 0xD63F0200u) != 0) /* blr x16 */
            FAIL_BUILD("emit slide stub failed");
        cur += 4u;
        if (write_u32_le(code_buf, code_size, cur, 0xF94007FEu) != 0) /* ldr x30, [sp, #8] */
            FAIL_BUILD("emit slide stub failed");
        cur += 4u;
        if (write_u32_le(code_buf, code_size, cur, 0x910043FFu) != 0) /* add sp, sp, #16 */
            FAIL_BUILD("emit slide stub failed");
        cur += 4u;
        if (write_u32_le(code_buf, code_size, cur, 0xD65F03C0u) != 0) /* ret */
            FAIL_BUILD("emit slide stub failed");
        cur += 4u;
        if ((size_t)(cur - (uint32_t)stub_off) != stub_size)
            FAIL_BUILD("slide stub size mismatch");

        if (patch_aarch64_page21_exec(code_buf, code_size, (uint32_t)stub_off,
                                      text_base + stub_off, MACHO_ARM64_EXEC_IMAGE_BASE) != 0)
            FAIL_BUILD("slide stub header page patch failed");
        if (patch_aarch64_pageoff12_exec(code_buf, code_size,
                                         (uint32_t)stub_off + 4u,
                                         MACHO_ARM64_EXEC_IMAGE_BASE, false) != 0)
            FAIL_BUILD("slide stub header add patch failed");

        if (patch_aarch64_page21_exec(code_buf, code_size, call_adrp_off,
                                      text_base + call_adrp_off,
                                      init_fn_real_addr) != 0) {
            FAIL_BUILD("slide stub init page patch failed");
        }
        if (patch_aarch64_pageoff12_exec(code_buf, code_size,
                                         call_adrp_off + 4u,
                                         init_fn_real_addr, false) != 0) {
            FAIL_BUILD("slide stub init add patch failed");
        }

        init_stub_addr = text_base + stub_off;
        if (reroute_entry_symbol) {
            build->ctx.symbols[init_fn_sym_idx].offset = (uint32_t)stub_off;
            sym_addr[init_fn_sym_idx] = init_stub_addr;
        } else {
            for (uint32_t ri = 0; ri < build->ctx.num_relocs; ri++) {
                const lr_obj_reloc_t *rel = &build->ctx.relocs[ri];
                uint64_t place_addr;
                if (rel->symbol_idx != init_fn_sym_idx)
                    continue;
                place_addr = text_base + rel->offset;
                switch (rel->type) {
                case LR_RELOC_ARM64_BRANCH26:
                    if (patch_aarch64_branch26_exec(code_buf, code_size, rel->offset,
                                                    place_addr, init_stub_addr) != 0)
                        FAIL_BUILD("reroute init BRANCH26 failed");
                    break;
                case LR_RELOC_ARM64_PAGE21:
                    if (patch_aarch64_page21_exec(code_buf, code_size, rel->offset,
                                                  place_addr, init_stub_addr) != 0)
                        FAIL_BUILD("reroute init PAGE21 failed");
                    break;
                case LR_RELOC_ARM64_PAGEOFF12:
                    if (patch_aarch64_pageoff12_exec(code_buf, code_size, rel->offset,
                                                     init_stub_addr, false) != 0)
                        FAIL_BUILD("reroute init PAGEOFF12 failed");
                    break;
                case LR_RELOC_ARM64_GOT_LOAD_PAGE21:
                    if (patch_aarch64_page21_exec(code_buf, code_size, rel->offset,
                                                  place_addr, init_stub_addr) != 0)
                        FAIL_BUILD("reroute init GOT_PAGE21 failed");
                    break;
                case LR_RELOC_ARM64_GOT_LOAD_PAGEOFF12:
                    if (patch_aarch64_add_imm12_exec(code_buf, code_size, rel->offset,
                                                     init_stub_addr) != 0)
                        FAIL_BUILD("reroute init GOT_PAGEOFF12 failed");
                    break;
                default:
                    FAIL_BUILD("unsupported reloc type for init reroute");
                }
            }
        }
    }

    if (runtime_handle)
        (void)lr_platform_dlclose(runtime_handle);
    free(sym_addr);
    free(got_slot_off);
    free(stub_off);
    free(sym_needed);
    free(stub_for_sym);
    free(slide_slot_off);
    *out_code = code_buf;
    *out_code_size = code_size;
    *out_data = data_buf;
    *out_data_size = data_runtime_size;
    return 0;

fail:
    if (runtime_handle)
        (void)lr_platform_dlclose(runtime_handle);
    free(code_buf);
    free(data_buf);
    free(sym_addr);
    free(got_slot_off);
    free(stub_off);
    free(sym_needed);
    free(stub_for_sym);
    free(slide_slot_off);
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

static int stream_get_path(FILE *out, char *path_buf, size_t path_cap) {
    int fd;
    if (!out || !path_buf || path_cap == 0)
        return -1;
    fd = fileno(out);
    if (fd < 0)
        return -1;
    path_buf[0] = '\0';
    if (fcntl(fd, F_GETPATH, path_buf) != 0)
        return -1;
    if (path_buf[0] == '\0')
        return -1;
    return 0;
}

static int write_signed_macho_exec_aarch64(FILE *out,
                                           const uint8_t *code,
                                           size_t code_size,
                                           const uint8_t *data,
                                           size_t data_size,
                                           lr_objfile_ctx_t *ctx,
                                           const char *entry_symbol) {
    char out_path[PATH_MAX];
    char exe_tpl[] = "/tmp/liric_exe_XXXXXX";
    int exe_fd = -1;
    FILE *exe_out = NULL;
    int rc = -1;

    if (!out || !ctx || !entry_symbol || !entry_symbol[0])
        return -1;

    if (stream_get_path(out, out_path, sizeof(out_path)) == 0) {
        if (write_macho_executable_arm64(out, code, code_size, data, data_size,
                                         ctx, entry_symbol) != 0)
            return -1;
        if (fflush(out) != 0)
            return -1;
        if (run_codesign_adhoc(out_path) != 0)
            return -1;
        return 0;
    }

    exe_fd = mkstemp(exe_tpl);
    if (exe_fd < 0)
        return -1;
    exe_out = fdopen(exe_fd, "wb");
    if (!exe_out) {
        close(exe_fd);
        unlink(exe_tpl);
        return -1;
    }
    exe_fd = -1;

    if (write_macho_executable_arm64(exe_out, code, code_size, data, data_size,
                                     ctx, entry_symbol) != 0)
        goto done;
    if (fclose(exe_out) != 0)
        goto done;
    exe_out = NULL;

    if (run_codesign_adhoc(exe_tpl) != 0)
        goto done;
    if (copy_file_to_stream(exe_tpl, out) != 0)
        goto done;

    rc = 0;

done:
    if (exe_out)
        fclose(exe_out);
    if (exe_fd >= 0)
        close(exe_fd);
    unlink(exe_tpl);
    return rc;
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
            if (rel->offset + sizeof(uint64_t) <= gsize) {
                uint64_t addend_raw = (uint64_t)rel->addend;
                memcpy(out->data_buf + out->data_pos + rel->offset,
                       &addend_raw, sizeof(addend_raw));
            }
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
            if (rel->offset + sizeof(uint64_t) <= gsize) {
                uint64_t addend_raw = (uint64_t)rel->addend;
                memcpy(out->data_buf + out->data_pos + rel->offset,
                       &addend_raw, sizeof(addend_raw));
            }
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
            uint64_t addend = 0;
            if ((size_t)rel->offset + sizeof(uint64_t) <= out->data_pos) {
                memcpy(&addend, out->data_buf + rel->offset, sizeof(addend));
            }
            fprintf(stderr, "  data[%u]: off=%u type=%u sym=%u (%s) addend=%lld\n",
                    ri, rel->offset, rel->type, rel->symbol_idx, name,
                    (long long)addend);
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
        uint8_t *exec_code = NULL;
        size_t exec_code_size = 0;
        uint8_t *exec_data = NULL;
        size_t exec_data_size = 0;
#if !defined(_WIN32)
        if (build_macho_exec_payload_aarch64(&build, entry_symbol,
                                             &exec_code,
                                             &exec_code_size,
                                             &exec_data,
                                             &exec_data_size) != 0)
            goto done;
#else
        goto done;
#endif
        result = write_signed_macho_exec_aarch64(
            out, exec_code, exec_code_size,
            exec_data, exec_data_size,
            &build.ctx, entry_symbol);
        free(exec_code);
        free(exec_data);
        exec_code = NULL;
        exec_data = NULL;
done:
        free(exec_code);
        free(exec_data);
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
        uint8_t *exec_code = NULL;
        size_t exec_code_size = 0;
        uint8_t *exec_data = NULL;
        size_t exec_data_size = 0;

#if !defined(_WIN32)
        if (build_macho_exec_payload_aarch64(&build, entry_symbol,
                                             &exec_code,
                                             &exec_code_size,
                                             &exec_data,
                                             &exec_data_size) != 0)
            goto done;
#else
        goto done;
#endif
        result = write_signed_macho_exec_aarch64(
            out, exec_code, exec_code_size,
            exec_data, exec_data_size,
            &build.ctx, entry_symbol);
        free(exec_code);
        free(exec_data);
        exec_code = NULL;
        exec_data = NULL;

done:
        free(exec_code);
        free(exec_data);
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
