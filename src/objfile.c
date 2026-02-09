#include "objfile.h"
#include "objfile_macho.h"
#include "objfile_elf.h"
#include "arena.h"
#include <stdlib.h>
#include <string.h>

#define OBJ_CODE_BUF_SIZE (4 * 1024 * 1024)
#define OBJ_DATA_BUF_SIZE (1 * 1024 * 1024)
#define OBJ_INITIAL_RELOC_CAP 256
#define OBJ_INITIAL_SYMBOL_CAP 128

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

static int obj_build_module_symbol_cache(lr_objfile_ctx_t *oc, lr_module_t *m) {
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
    if (obj_build_module_symbol_cache(&ctx, m) != 0) {
        m->obj_ctx = NULL;
        free(ctx.module_sym_defined);
        free(ctx.module_sym_funcs);
        lr_arena_destroy(arena);
        free(code_buf);
        free(data_buf);
        return -1;
    }

    size_t code_pos = 0;
    size_t data_pos = 0;
    bool has_data = false;

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
            free(ctx.module_sym_defined);
            free(ctx.module_sym_funcs);
            lr_arena_destroy(arena);
            free(code_buf);
            free(data_buf);
            return -1;
        }

        for (uint32_t ri = reloc_base; ri < ctx.num_relocs; ri++)
            ctx.relocs[ri].offset += (uint32_t)code_pos;

        code_pos += func_len;
    }

    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (!f->is_decl && f->first_block)
            continue;
        lr_obj_ensure_symbol(&ctx, f->name, false, 0, 0);
    }

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

        data_pos = obj_align_up(data_pos, galign);

        if (data_pos + gsize > OBJ_DATA_BUF_SIZE) {
            m->obj_ctx = NULL;
            free(ctx.module_sym_defined);
            free(ctx.module_sym_funcs);
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

    int result;
#ifdef __APPLE__
    if (strcmp(target->name, "aarch64") == 0)
        result = write_macho(out, code_buf, code_pos,
                              has_data ? data_buf : NULL,
                              has_data ? data_pos : 0,
                              &ctx, 0x0100000Cu, macho_reloc_arm64);
    else
        result = -1;
#else
    if (strcmp(target->name, "x86_64") == 0)
        result = write_elf(out, code_buf, code_pos,
                            has_data ? data_buf : NULL,
                            has_data ? data_pos : 0,
                            &ctx, 62, elf_reloc_x86_64);
    else if (strcmp(target->name, "aarch64") == 0)
        result = write_elf(out, code_buf, code_pos,
                            has_data ? data_buf : NULL,
                            has_data ? data_pos : 0,
                            &ctx, 183, elf_reloc_x86_64);
    else
        result = -1;
#endif

    free(ctx.relocs);
    free(ctx.symbols);
    free(ctx.symbol_index);
    free(ctx.module_sym_defined);
    free(ctx.module_sym_funcs);
    lr_arena_destroy(arena);
    free(code_buf);
    free(data_buf);

    return result;
}
