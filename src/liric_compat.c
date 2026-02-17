#include "ir.h"
#include "arena.h"
#include "jit.h"
#include "liric.h"
#include "llvm_backend.h"
#include "module_emit.h"
#include <liric/liric_session.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <limits.h>

static int compat_policy_from_env(lr_session_mode_t *out_mode) {
    const char *p;
    if (!out_mode)
        return -1;
    p = getenv("LIRIC_POLICY");
    if (!p || !p[0] || strcmp(p, "direct") == 0) {
        *out_mode = LR_MODE_DIRECT;
        return 0;
    }
    if (strcmp(p, "ir") == 0) {
        *out_mode = LR_MODE_IR;
        return 0;
    }
    return -1;
}

/* ---- Compat types (must match the public header exactly) ---- */

typedef enum lc_value_kind {
    LC_VAL_VREG,
    LC_VAL_CONST_INT,
    LC_VAL_CONST_FP,
    LC_VAL_CONST_NULL,
    LC_VAL_CONST_UNDEF,
    LC_VAL_GLOBAL,
    LC_VAL_ARGUMENT,
    LC_VAL_BLOCK,
    LC_VAL_CONST_AGGREGATE,
} lc_value_kind_t;

enum {
    LC_BACKEND_DEFAULT = 0,
    LC_BACKEND_ISEL = 1,
    LC_BACKEND_COPY_PATCH = 2,
    LC_BACKEND_LLVM = 3,
};

typedef struct lc_value {
    lc_value_kind_t kind;
    lr_type_t *type;
    union {
        struct { uint32_t id; lr_func_t *func; void *phi_node; lr_type_t *alloc_type; } vreg;
        struct { int64_t val; unsigned width; } const_int;
        struct { double val; bool is_double; } const_fp;
        struct { uint32_t id; const char *name; lr_func_t *func; int64_t offset; } global;
        struct { uint32_t param_idx; lr_func_t *func; } argument;
        struct { lr_block_t *block; lr_func_t *func; } block;
        struct { const void *data; size_t size; } aggregate;
    };
} lc_value_t;

typedef struct lc_context {
    lr_module_t *mod;
    lr_type_t *type_void;
    lr_type_t *type_i1;
    lr_type_t *type_i8;
    lr_type_t *type_i16;
    lr_type_t *type_i32;
    lr_type_t *type_i64;
    lr_type_t *type_float;
    lr_type_t *type_double;
    lr_type_t *type_ptr;
    lr_arena_t *type_arena;
    int backend;
} lc_context_t;

typedef struct lc_phi_node lc_phi_node_t;
typedef struct lc_const_reloc_meta lc_const_reloc_meta_t;
typedef struct lc_const_value_meta lc_const_value_meta_t;

typedef struct lc_module_compat {
    lr_module_t *mod;
    lc_context_t *ctx;
    const char *name;
    lc_value_t *value_pool;
    uint32_t value_count;
    uint32_t value_cap;
    lc_value_t **func_values;
    uint32_t func_value_count;
    uint32_t func_value_cap;
    lr_session_t *session;
    lr_block_t **detached_blocks;
    uint32_t detached_count;
    uint32_t detached_cap;
    lr_module_t *cache_owner_mod;
    lr_func_t **func_by_sym;
    lr_global_t **global_by_sym;
    uint32_t sym_cache_cap;
    lc_const_value_meta_t *const_value_meta;
} lc_module_compat_t;

struct lc_const_reloc_meta {
    size_t offset;
    int64_t addend;
    const char *symbol_name;
    lc_const_reloc_meta_t *next;
};

struct lc_const_value_meta {
    lc_value_t *value;
    lc_const_reloc_meta_t *relocs;
    lc_const_value_meta_t *next;
};

struct lc_phi_node {
    lc_value_t *result;
    lr_type_t *type;
    lr_block_t *block;
    lr_func_t *func;
    lr_module_t *mod;
    lc_value_t **incoming_vals;
    uint32_t *incoming_block_ids;
    uint32_t num_incoming;
    uint32_t cap_incoming;
    bool finalized;
    lr_inst_t *inst;
    lr_session_t *session;
};

typedef struct lc_alloca_inst {
    lc_value_t *result;
    lr_type_t *alloc_type;
} lc_alloca_inst_t;

/* ---- Internal desc_to_op (same as session.c) ---- */

/* Forward declaration for safe_undef */
lc_value_t *lc_value_undef(lc_module_compat_t *mod, lr_type_t *type);
void lc_create_memcpy(lc_module_compat_t *mod, lr_block_t *b, lr_func_t *f,
                      lc_value_t *dst, lc_value_t *src, lc_value_t *size);

/* ---- Value pool (slab-based to avoid pointer invalidation) ---- */

#define LC_VALUE_SLAB_SIZE 256
#define LC_INITIAL_FUNC_CAP 32
#define LC_INITIAL_PHI_CAP 4

typedef struct lc_value_slab {
    lc_value_t values[LC_VALUE_SLAB_SIZE];
    struct lc_value_slab *next;
} lc_value_slab_t;

static lc_value_t *value_pool_alloc(lc_module_compat_t *mod) {
    uint32_t offset = mod->value_count % LC_VALUE_SLAB_SIZE;
    if (mod->value_count == 0 || offset == 0) {
        lc_value_slab_t *slab = (lc_value_slab_t *)calloc(
            1, sizeof(lc_value_slab_t));
        if (!slab) return NULL;
        slab->next = (lc_value_slab_t *)mod->value_pool;
        mod->value_pool = (lc_value_t *)slab;
    }
    lc_value_slab_t *head = (lc_value_slab_t *)mod->value_pool;
    lc_value_t *v = &head->values[offset];
    mod->value_count++;
    return v;
}

static void value_pool_free(lc_module_compat_t *mod) {
    lc_value_slab_t *slab = (lc_value_slab_t *)mod->value_pool;
    while (slab) {
        lc_value_slab_t *next = slab->next;
        free(slab);
        slab = next;
    }
    mod->value_pool = NULL;
}

/* ---- Safe fallback value (avoids NULL cascading into C++ crashes) ---- */

static lr_type_t s_poison_type = { LR_TYPE_PTR, {{0}} };

static lc_value_t *safe_undef(lc_module_compat_t *mod) {
    if (!mod) {
        static lc_value_t s_undef;
        s_undef.kind = LC_VAL_CONST_UNDEF;
        s_undef.type = &s_poison_type;
        return &s_undef;
    }
    return lc_value_undef(mod, mod->mod->type_ptr);
}

static void func_cache_add(lc_module_compat_t *mod, lc_value_t *fv) {
    if (mod->func_value_count == mod->func_value_cap) {
        uint32_t new_cap = mod->func_value_cap == 0
            ? LC_INITIAL_FUNC_CAP : mod->func_value_cap * 2;
        lc_value_t **new_arr = (lc_value_t **)realloc(
            mod->func_values, sizeof(lc_value_t *) * new_cap);
        if (!new_arr) return;
        mod->func_values = new_arr;
        mod->func_value_cap = new_cap;
    }
    mod->func_values[mod->func_value_count++] = fv;
}

static void clear_symbol_caches(lc_module_compat_t *mod) {
    if (!mod) return;
    free(mod->func_by_sym);
    free(mod->global_by_sym);
    mod->func_by_sym = NULL;
    mod->global_by_sym = NULL;
    mod->sym_cache_cap = 0;
    mod->cache_owner_mod = mod->mod;
}

static void ensure_cache_owner(lc_module_compat_t *mod) {
    if (!mod) return;
    if (mod->cache_owner_mod != mod->mod)
        clear_symbol_caches(mod);
}

static int ensure_symbol_cache_cap(lc_module_compat_t *mod, uint32_t min_cap) {
    if (!mod) return -1;
    ensure_cache_owner(mod);
    if (mod->sym_cache_cap >= min_cap)
        return 0;

    uint32_t new_cap = mod->sym_cache_cap ? mod->sym_cache_cap : 64u;
    while (new_cap < min_cap)
        new_cap *= 2u;

    lr_func_t **new_func = (lr_func_t **)calloc(new_cap, sizeof(*new_func));
    lr_global_t **new_global = (lr_global_t **)calloc(new_cap, sizeof(*new_global));
    if (!new_func || !new_global) {
        free(new_func);
        free(new_global);
        return -1;
    }
    if (mod->sym_cache_cap > 0) {
        memcpy(new_func, mod->func_by_sym,
               sizeof(*new_func) * mod->sym_cache_cap);
        memcpy(new_global, mod->global_by_sym,
               sizeof(*new_global) * mod->sym_cache_cap);
    }
    free(mod->func_by_sym);
    free(mod->global_by_sym);
    mod->func_by_sym = new_func;
    mod->global_by_sym = new_global;
    mod->sym_cache_cap = new_cap;
    return 0;
}

static uint32_t compat_symbol_id(lc_module_compat_t *mod, const char *name) {
    if (!mod || !mod->mod || !name || !name[0])
        return UINT32_MAX;
    uint32_t sym_id = lr_module_intern_symbol(mod->mod, name);
    if (sym_id == UINT32_MAX)
        return UINT32_MAX;
    if (ensure_symbol_cache_cap(mod, sym_id + 1u) != 0)
        return UINT32_MAX;
    return sym_id;
}

static lr_func_t *find_func_linear(lr_module_t *m, const char *name) {
    if (!m || !name) return NULL;
    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (f->name && strcmp(f->name, name) == 0)
            return f;
    }
    return NULL;
}

static lr_global_t *find_global_linear(lr_module_t *m, const char *name) {
    if (!m || !name) return NULL;
    for (lr_global_t *g = m->first_global; g; g = g->next) {
        if (g->name && strcmp(g->name, name) == 0)
            return g;
    }
    return NULL;
}

static char *dup_cstr(const char *s) {
    size_t n;
    char *out;
    if (!s)
        return NULL;
    n = strlen(s);
    out = (char *)malloc(n + 1);
    if (!out)
        return NULL;
    memcpy(out, s, n + 1);
    return out;
}

static int symbol_name_in_use(lc_module_compat_t *mod, const char *name) {
    if (!mod || !mod->mod || !name || !name[0])
        return 0;
    return find_func_linear(mod->mod, name) != NULL ||
           find_global_linear(mod->mod, name) != NULL;
}

static char *make_unique_symbol_name(lc_module_compat_t *mod,
                                     const char *name) {
    size_t base_len;
    if (!mod || !name || !name[0])
        return NULL;
    if (!symbol_name_in_use(mod, name))
        return dup_cstr(name);

    base_len = strlen(name);
    for (uint32_t suffix = 1; suffix < UINT32_MAX; suffix++) {
        char suffix_buf[32];
        int ns = snprintf(suffix_buf, sizeof(suffix_buf), ".%u", suffix);
        char *candidate;
        if (ns <= 0 || (size_t)ns >= sizeof(suffix_buf))
            return NULL;
        candidate = (char *)malloc(base_len + (size_t)ns + 1);
        if (!candidate)
            return NULL;
        memcpy(candidate, name, base_len);
        memcpy(candidate + base_len, suffix_buf, (size_t)ns + 1);
        if (!symbol_name_in_use(mod, candidate))
            return candidate;
        free(candidate);
    }
    return NULL;
}

static void cache_func_by_symbol(lc_module_compat_t *mod, uint32_t sym_id,
                                 lr_func_t *func) {
    if (!mod || !func || sym_id == UINT32_MAX)
        return;
    if (ensure_symbol_cache_cap(mod, sym_id + 1u) != 0)
        return;
    mod->func_by_sym[sym_id] = func;
}

static void cache_global_by_symbol(lc_module_compat_t *mod, uint32_t sym_id,
                                   lr_global_t *global) {
    if (!mod || !global || sym_id == UINT32_MAX)
        return;
    if (ensure_symbol_cache_cap(mod, sym_id + 1u) != 0)
        return;
    mod->global_by_sym[sym_id] = global;
}

static lr_func_t *lookup_func_cached(lc_module_compat_t *mod, const char *name,
                                     uint32_t *sym_id_out) {
    uint32_t sym_id = compat_symbol_id(mod, name);
    if (sym_id_out)
        *sym_id_out = sym_id;
    if (sym_id == UINT32_MAX)
        return NULL;
    lr_func_t *f = mod->func_by_sym[sym_id];
    if (f)
        return f;
    f = find_func_linear(mod->mod, name);
    if (f)
        mod->func_by_sym[sym_id] = f;
    return f;
}

static lr_global_t *lookup_global_cached(lc_module_compat_t *mod, const char *name,
                                         uint32_t *sym_id_out) {
    uint32_t sym_id = compat_symbol_id(mod, name);
    if (sym_id_out)
        *sym_id_out = sym_id;
    if (sym_id == UINT32_MAX)
        return NULL;
    lr_global_t *g = mod->global_by_sym[sym_id];
    if (g)
        return g;
    g = find_global_linear(mod->mod, name);
    if (g)
        mod->global_by_sym[sym_id] = g;
    return g;
}

static lc_const_value_meta_t *lookup_const_value_meta(lc_module_compat_t *mod,
                                                      lc_value_t *value) {
    if (!mod || !value)
        return NULL;
    for (lc_const_value_meta_t *it = mod->const_value_meta; it; it = it->next) {
        if (it->value == value)
            return it;
    }
    return NULL;
}

static lc_const_value_meta_t *ensure_const_value_meta(lc_module_compat_t *mod,
                                                      lc_value_t *value) {
    lc_const_value_meta_t *meta;
    if (!mod || !value)
        return NULL;
    meta = lookup_const_value_meta(mod, value);
    if (meta)
        return meta;
    meta = lr_arena_new(mod->mod->arena, lc_const_value_meta_t);
    if (!meta)
        return NULL;
    memset(meta, 0, sizeof(*meta));
    meta->value = value;
    meta->next = mod->const_value_meta;
    mod->const_value_meta = meta;
    return meta;
}

/* ---- lc_value_to_desc ---- */

lr_operand_desc_t lc_value_to_desc(lc_value_t *val) {
    lr_operand_desc_t d;
    memset(&d, 0, sizeof(d));
    d.kind = LR_OP_KIND_NULL;
    if (!val) {
        return d;
    }
    d.type = val->type;
    switch (val->kind) {
    case LC_VAL_VREG:
        d.kind = LR_OP_KIND_VREG;
        d.vreg = val->vreg.id;
        break;
    case LC_VAL_CONST_INT:
        d.kind = LR_OP_KIND_IMM_I64;
        d.imm_i64 = val->const_int.val;
        break;
    case LC_VAL_CONST_FP:
        d.kind = LR_OP_KIND_IMM_F64;
        d.imm_f64 = val->const_fp.val;
        break;
    case LC_VAL_CONST_NULL:
        d.kind = LR_OP_KIND_NULL;
        break;
    case LC_VAL_CONST_UNDEF:
        d.kind = LR_OP_KIND_UNDEF;
        break;
    case LC_VAL_GLOBAL:
        d.kind = LR_OP_KIND_GLOBAL;
        d.global_id = val->global.id;
        d.global_offset = val->global.offset;
        break;
    case LC_VAL_ARGUMENT:
        if (val->argument.func && val->argument.func->param_vregs
            && val->argument.param_idx < val->argument.func->num_params) {
            d.kind = LR_OP_KIND_VREG;
            d.vreg = val->argument.func->param_vregs[val->argument.param_idx];
        } else {
            d.kind = LR_OP_KIND_IMM_I64;
            d.imm_i64 = 0;
        }
        break;
    case LC_VAL_BLOCK:
        if (val->block.block) {
            d.kind = LR_OP_KIND_BLOCK;
            d.block_id = val->block.block->id;
        } else {
            d.kind = LR_OP_KIND_IMM_I64;
            d.imm_i64 = 0;
        }
        break;
    case LC_VAL_CONST_AGGREGATE:
        d.kind = LR_OP_KIND_IMM_I64;
        d.imm_i64 = 0;
        break;
    }
    return d;
}

static lr_operand_t compat_desc_to_operand(const lr_operand_desc_t *d) {
    lr_operand_t op;
    memset(&op, 0, sizeof(op));
    if (!d) {
        op.kind = LR_VAL_UNDEF;
        return op;
    }
    op.kind = (lr_operand_kind_t)d->kind;
    op.type = d->type;
    op.global_offset = d->global_offset;
    switch (d->kind) {
    case LR_OP_KIND_VREG:
        op.vreg = d->vreg;
        break;
    case LR_OP_KIND_IMM_I64:
        op.imm_i64 = d->imm_i64;
        break;
    case LR_OP_KIND_IMM_F64:
        op.imm_f64 = d->imm_f64;
        break;
    case LR_OP_KIND_BLOCK:
        op.block_id = d->block_id;
        break;
    case LR_OP_KIND_GLOBAL:
        op.global_id = d->global_id;
        break;
    default:
        break;
    }
    return op;
}

static int compat_finish_active_func(lc_module_compat_t *mod) {
    lr_error_t err;
    int rc;
    if (!mod || !mod->session)
        return -1;
    if (!lr_session_cur_func(mod->session))
        return 0;
    memset(&err, 0, sizeof(err));
    rc = lr_session_func_end(mod->session, NULL, &err);
    if (rc != 0 && err.msg[0]) {
        fprintf(stderr, "liric compat finalize failed: %s\n", err.msg);
    }
    return rc;
}

static int compat_bind_emit(lc_module_compat_t *mod, lr_block_t *b, lr_func_t *f,
                            lr_error_t *err) {
    lr_session_t *s;
    lr_func_t *active;
    if (!mod || !mod->session || !b)
        return -1;
    if (!f)
        f = b->func;
    if (!f || b->func != f)
        return -1;
    s = mod->session;

    if (!lr_session_is_direct(s))
        return lr_session_bind_ir(s, mod->mod, f, b, err);

    active = lr_session_cur_func(s);
    if (active != f) {
        if (active && compat_finish_active_func(mod) != 0)
            return -1;
        if (lr_session_func_begin_existing(s, mod->mod, f, err) != 0)
            return -1;
    }
    if (lr_session_cur_block(s) == b)
        return 0;
    return lr_session_adopt_block(s, b->id, b, err);
}

static uint32_t compat_emit(lc_module_compat_t *mod, lr_block_t *b, lr_func_t *f,
                            const lr_inst_desc_t *inst) {
    lr_error_t err;
    memset(&err, 0, sizeof(err));
    if (compat_bind_emit(mod, b, f, &err) != 0)
        return 0;
    return lr_session_emit(mod->session, inst, &err);
}

static lr_operand_desc_t block_operand_desc(lr_block_t *block) {
    lr_operand_desc_t d;
    memset(&d, 0, sizeof(d));
    d.kind = LR_OP_KIND_BLOCK;
    d.block_id = block ? block->id : 0;
    return d;
}

static lr_operand_desc_t global_operand_desc(uint32_t id, lr_type_t *type) {
    lr_operand_desc_t d;
    memset(&d, 0, sizeof(d));
    d.kind = LR_OP_KIND_GLOBAL;
    d.global_id = id;
    d.type = type;
    return d;
}

/* ---- Context ---- */

lc_context_t *lc_context_create(void) {
    lc_context_t *ctx = (lc_context_t *)calloc(1, sizeof(lc_context_t));
    if (!ctx) return NULL;
    ctx->type_arena = lr_arena_create(0);
    if (!ctx->type_arena) { free(ctx); return NULL; }
    ctx->type_void   = lr_arena_new(ctx->type_arena, lr_type_t);
    ctx->type_void->kind = LR_TYPE_VOID;
    ctx->type_i1     = lr_arena_new(ctx->type_arena, lr_type_t);
    ctx->type_i1->kind = LR_TYPE_I1;
    ctx->type_i8     = lr_arena_new(ctx->type_arena, lr_type_t);
    ctx->type_i8->kind = LR_TYPE_I8;
    ctx->type_i16    = lr_arena_new(ctx->type_arena, lr_type_t);
    ctx->type_i16->kind = LR_TYPE_I16;
    ctx->type_i32    = lr_arena_new(ctx->type_arena, lr_type_t);
    ctx->type_i32->kind = LR_TYPE_I32;
    ctx->type_i64    = lr_arena_new(ctx->type_arena, lr_type_t);
    ctx->type_i64->kind = LR_TYPE_I64;
    ctx->type_float  = lr_arena_new(ctx->type_arena, lr_type_t);
    ctx->type_float->kind = LR_TYPE_FLOAT;
    ctx->type_double = lr_arena_new(ctx->type_arena, lr_type_t);
    ctx->type_double->kind = LR_TYPE_DOUBLE;
    ctx->type_ptr    = lr_arena_new(ctx->type_arena, lr_type_t);
    ctx->type_ptr->kind = LR_TYPE_PTR;
    ctx->backend = LR_SESSION_BACKEND_ISEL;
    return ctx;
}

void lc_context_destroy(lc_context_t *ctx) {
    if (!ctx) return;
    if (ctx->type_arena) lr_arena_destroy(ctx->type_arena);
    free(ctx);
}

void lc_context_set_backend(lc_context_t *ctx, int backend) {
    if (!ctx)
        return;
    switch (backend) {
    case LC_BACKEND_DEFAULT:
    case LC_BACKEND_ISEL:
    case LC_BACKEND_COPY_PATCH:
    case LC_BACKEND_LLVM:
        ctx->backend = backend;
        break;
    default:
        ctx->backend = LC_BACKEND_ISEL;
        break;
    }
}

int lc_context_get_backend(const lc_context_t *ctx) {
    if (!ctx)
        return LC_BACKEND_ISEL;
    return ctx->backend;
}

/* ---- Module ---- */

lc_module_compat_t *lc_module_create(lc_context_t *ctx, const char *name) {
    lc_module_compat_t *cm = (lc_module_compat_t *)calloc(
        1, sizeof(lc_module_compat_t));
    lr_session_config_t cfg;
    lr_arena_t *arena;
    if (!cm) return NULL;

    arena = lr_arena_create(0);
    if (!arena) { free(cm); return NULL; }

    cm->mod = lr_module_create(arena);
    if (!cm->mod) { lr_arena_destroy(arena); free(cm); return NULL; }
    memset(&cfg, 0, sizeof(cfg));
    /* Unified policy: DIRECT default, explicit IR via LIRIC_POLICY=ir. */
    if (compat_policy_from_env(&cfg.mode) != 0) {
        lr_arena_destroy(arena);
        free(cm);
        return NULL;
    }
    switch (ctx ? ctx->backend : LC_BACKEND_ISEL) {
    case LC_BACKEND_COPY_PATCH:
        cfg.backend = LR_SESSION_BACKEND_COPY_PATCH;
        break;
    case LC_BACKEND_LLVM:
        cfg.backend = LR_SESSION_BACKEND_LLVM;
        break;
    case LC_BACKEND_DEFAULT:
    case LC_BACKEND_ISEL:
    default:
        cfg.backend = LR_SESSION_BACKEND_ISEL;
        break;
    }
    cm->session = lr_session_create(&cfg, NULL);
    if (!cm->session) {
        lr_arena_destroy(arena);
        free(cm);
        return NULL;
    }
    cm->cache_owner_mod = cm->mod;

    cm->ctx = ctx;
    ctx->mod = cm->mod;

    cm->mod->type_void   = ctx->type_void;
    cm->mod->type_i1     = ctx->type_i1;
    cm->mod->type_i8     = ctx->type_i8;
    cm->mod->type_i16    = ctx->type_i16;
    cm->mod->type_i32    = ctx->type_i32;
    cm->mod->type_i64    = ctx->type_i64;
    cm->mod->type_float  = ctx->type_float;
    cm->mod->type_double = ctx->type_double;
    cm->mod->type_ptr    = ctx->type_ptr;

    size_t nlen = strlen(name);
    char *dup = (char *)malloc(nlen + 1);
    memcpy(dup, name, nlen + 1);
    cm->name = dup;

    return cm;
}

void lc_module_destroy(lc_module_compat_t *mod) {
    if (!mod) return;
    lr_arena_destroy(mod->mod->arena);
    lr_session_destroy(mod->session);
    value_pool_free(mod);
    free(mod->func_values);
    free(mod->detached_blocks);
    free(mod->func_by_sym);
    free(mod->global_by_sym);
    free((void *)mod->name);
    free(mod);
}

lr_module_t *lc_module_get_ir(lc_module_compat_t *mod) {
    return mod->mod;
}

static void compat_dump_module(lr_module_t *m, FILE *out) {
    if (!m || !out)
        return;
    lr_module_dump(m, out);
}

void lc_module_dump(lc_module_compat_t *mod) {
    compat_dump_module(mod->mod, stderr);
}

void lc_module_print(lc_module_compat_t *mod, FILE *out) {
    compat_dump_module(mod->mod, out ? out : stdout);
}

char *lc_module_sprint(lc_module_compat_t *mod, size_t *out_len) {
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f) return NULL;
    compat_dump_module(mod->mod, f);
    fclose(f);
    if (out_len) *out_len = len;
    return buf;
}

/* ---- Value allocation ---- */

lc_value_t *lc_value_alloc(lc_module_compat_t *mod) {
    return value_pool_alloc(mod);
}

lc_value_t *lc_value_vreg(lc_module_compat_t *mod, uint32_t id,
                           lr_type_t *type, lr_func_t *func) {
    lc_value_t *v = value_pool_alloc(mod);
    if (!v) return NULL;
    v->kind = LC_VAL_VREG;
    v->type = type;
    v->vreg.id = id;
    v->vreg.func = func;
    v->vreg.phi_node = NULL;
    return v;
}

lc_value_t *lc_value_const_int(lc_module_compat_t *mod, lr_type_t *type,
                                int64_t val, unsigned width) {
    lc_value_t *v = value_pool_alloc(mod);
    if (!v) return NULL;
    v->kind = LC_VAL_CONST_INT;
    v->type = type;
    v->const_int.val = val;
    v->const_int.width = width;
    return v;
}

lc_value_t *lc_value_const_fp(lc_module_compat_t *mod, lr_type_t *type,
                               double val, bool is_double) {
    lc_value_t *v = value_pool_alloc(mod);
    if (!v) return NULL;
    v->kind = LC_VAL_CONST_FP;
    v->type = type;
    v->const_fp.val = val;
    v->const_fp.is_double = is_double;
    return v;
}

lc_value_t *lc_value_const_null(lc_module_compat_t *mod, lr_type_t *type) {
    lc_value_t *v = value_pool_alloc(mod);
    if (!v) return NULL;
    v->kind = LC_VAL_CONST_NULL;
    v->type = type;
    return v;
}

lc_value_t *lc_value_undef(lc_module_compat_t *mod, lr_type_t *type) {
    lc_value_t *v = value_pool_alloc(mod);
    if (!v) return NULL;
    v->kind = LC_VAL_CONST_UNDEF;
    v->type = type;
    return v;
}

lc_value_t *lc_value_global(lc_module_compat_t *mod, uint32_t id,
                             lr_type_t *type, const char *name) {
    lc_value_t *v = value_pool_alloc(mod);
    if (!v) return NULL;
    v->kind = LC_VAL_GLOBAL;
    v->type = type;
    v->global.id = id;
    v->global.name = name;
    v->global.func = NULL;
    v->global.offset = 0;
    return v;
}

lc_value_t *lc_value_global_with_addend(lc_module_compat_t *mod,
                                         lc_value_t *base,
                                         int64_t addend) {
    lc_value_t *v;
    int64_t combined;
    if (!mod || !base || base->kind != LC_VAL_GLOBAL)
        return safe_undef(mod);
    if ((addend > 0 && base->global.offset > INT64_MAX - addend) ||
        (addend < 0 && base->global.offset < INT64_MIN - addend))
        return safe_undef(mod);
    combined = base->global.offset + addend;
    v = lc_value_global(mod, base->global.id, base->type, base->global.name);
    if (!v)
        return safe_undef(mod);
    v->global.func = base->global.func;
    v->global.offset = combined;
    return v;
}

lc_value_t *lc_value_argument(lc_module_compat_t *mod, uint32_t param_idx,
                               lr_type_t *type, lr_func_t *func) {
    lc_value_t *v = value_pool_alloc(mod);
    if (!v) return NULL;
    v->kind = LC_VAL_ARGUMENT;
    v->type = type;
    v->argument.param_idx = param_idx;
    v->argument.func = func;
    return v;
}

lc_value_t *lc_value_block_ref(lc_module_compat_t *mod, lr_block_t *block) {
    lc_value_t *v = value_pool_alloc(mod);
    if (!v) return NULL;
    v->kind = LC_VAL_BLOCK;
    v->type = NULL;
    v->block.block = block;
    v->block.func = block ? block->func : NULL;
    return v;
}

lc_value_t *lc_value_const_aggregate(lc_module_compat_t *mod, lr_type_t *type,
                                      const void *data, size_t size) {
    lc_value_t *v = value_pool_alloc(mod);
    if (!v) return NULL;
    v->kind = LC_VAL_CONST_AGGREGATE;
    v->type = type;
    if (data && size > 0 && mod && mod->mod && mod->mod->arena) {
        void *copy = lr_arena_alloc_uninit(mod->mod->arena, size, 1);
        if (!copy) return NULL;
        memcpy(copy, data, size);
        v->aggregate.data = copy;
        v->aggregate.size = size;
    } else {
        v->aggregate.data = NULL;
        v->aggregate.size = 0;
    }
    return v;
}

int lc_value_const_aggregate_add_reloc(lc_module_compat_t *mod,
                                        lc_value_t *aggregate,
                                        size_t offset,
                                        const char *symbol_name,
                                        int64_t addend) {
    lc_const_value_meta_t *meta;
    lc_const_reloc_meta_t *reloc;
    if (!mod || !aggregate || aggregate->kind != LC_VAL_CONST_AGGREGATE)
        return -1;
    if (!symbol_name || !symbol_name[0])
        return -1;
    meta = ensure_const_value_meta(mod, aggregate);
    if (!meta)
        return -1;
    reloc = lr_arena_new(mod->mod->arena, lc_const_reloc_meta_t);
    if (!reloc)
        return -1;
    reloc->offset = offset;
    reloc->addend = addend;
    reloc->symbol_name = lr_arena_strdup(mod->mod->arena, symbol_name,
                                         strlen(symbol_name));
    reloc->next = meta->relocs;
    meta->relocs = reloc;
    return 0;
}

/* ---- Type queries ---- */

lr_type_t *lc_get_int_type(lc_module_compat_t *mod, unsigned width) {
    lr_module_t *m = mod->mod;
    switch (width) {
    case 1:  return m->type_i1;
    case 8:  return m->type_i8;
    case 16: return m->type_i16;
    case 32: return m->type_i32;
    case 64: return m->type_i64;
    default: return m->type_i64;
    }
}

lr_type_t *lc_get_void_type(lc_module_compat_t *mod) {
    return mod->mod->type_void;
}

lr_type_t *lc_get_float_type(lc_module_compat_t *mod) {
    return mod->mod->type_float;
}

lr_type_t *lc_get_double_type(lc_module_compat_t *mod) {
    return mod->mod->type_double;
}

lr_type_t *lc_get_ptr_type(lc_module_compat_t *mod) {
    return mod->mod->type_ptr;
}

bool lc_type_is_integer(lr_type_t *ty) {
    if (!ty) return false;
    return ty->kind >= LR_TYPE_I1 && ty->kind <= LR_TYPE_I64;
}

bool lc_type_is_floating(lr_type_t *ty) {
    if (!ty) return false;
    return ty->kind == LR_TYPE_FLOAT || ty->kind == LR_TYPE_DOUBLE;
}

bool lc_type_is_pointer(lr_type_t *ty) {
    if (!ty) return false;
    return ty->kind == LR_TYPE_PTR;
}

unsigned lc_type_int_width(lr_type_t *ty) {
    if (!ty) return 0;
    switch (ty->kind) {
    case LR_TYPE_I1:  return 1;
    case LR_TYPE_I8:  return 8;
    case LR_TYPE_I16: return 16;
    case LR_TYPE_I32: return 32;
    case LR_TYPE_I64: return 64;
    default:          return 0;
    }
}

size_t lc_type_size_bits(lr_type_t *ty) {
    return lr_type_size(ty) * 8;
}

size_t lc_type_store_size(lr_type_t *ty) {
    return lr_type_size(ty);
}

size_t lc_type_alloc_size(lr_type_t *ty) {
    size_t sz = lr_type_size(ty);
    size_t al = lr_type_align(ty);
    return (sz + al - 1) & ~(al - 1);
}

unsigned lc_type_primitive_size_bits(lr_type_t *ty) {
    if (!ty) return 0;
    switch (ty->kind) {
    case LR_TYPE_I1:     return 1;
    case LR_TYPE_I8:     return 8;
    case LR_TYPE_I16:    return 16;
    case LR_TYPE_I32:    return 32;
    case LR_TYPE_I64:    return 64;
    case LR_TYPE_FLOAT:  return 32;
    case LR_TYPE_DOUBLE: return 64;
    case LR_TYPE_PTR:    return 64;
    default:             return 0;
    }
}

lr_type_t *lc_type_struct_field(lr_type_t *ty, unsigned idx) {
    if (!ty || ty->kind != LR_TYPE_STRUCT) return NULL;
    if (idx >= ty->struc.num_fields) return NULL;
    return ty->struc.fields[idx];
}

lr_type_t *lc_type_contained(lr_type_t *ty, unsigned idx) {
    if (!ty) return NULL;
    switch (ty->kind) {
    case LR_TYPE_STRUCT:
        if (idx < ty->struc.num_fields) return ty->struc.fields[idx];
        return NULL;
    case LR_TYPE_FUNC:
        if (idx < ty->func.num_params) return ty->func.params[idx];
        return NULL;
    case LR_TYPE_ARRAY:
    case LR_TYPE_VECTOR:
        return ty->array.elem;
    default:
        return NULL;
    }
}

unsigned lc_type_struct_num_fields(lr_type_t *ty) {
    if (!ty || ty->kind != LR_TYPE_STRUCT) return 0;
    return ty->struc.num_fields;
}

bool lc_type_struct_has_name(lr_type_t *ty) {
    if (!ty || ty->kind != LR_TYPE_STRUCT) return false;
    return ty->struc.name != NULL;
}

static lc_value_t *create_func_value(lc_module_compat_t *mod,
                                      lr_func_t *func);

lc_value_t *lc_global_lookup_or_create(lc_module_compat_t *mod,
                                        const char *name, lr_type_t *type) {
    if (!mod || !name) return safe_undef(mod);
    lr_module_t *m = mod->mod;
    uint32_t sym_id = UINT32_MAX;

    if (type && type->kind == LR_TYPE_FUNC) {
        lr_func_t *f = lookup_func_cached(mod, name, &sym_id);
        if (!f) {
            lr_type_t *ret = type->func.ret ? type->func.ret : m->type_void;
            f = lr_func_declare(m, name, ret, type->func.params,
                                type->func.num_params, type->func.vararg);
            if (!f)
                return safe_undef(mod);
            if (sym_id == UINT32_MAX)
                sym_id = compat_symbol_id(mod, name);
            if (sym_id == UINT32_MAX)
                return safe_undef(mod);
            cache_func_by_symbol(mod, sym_id, f);
        }
        return create_func_value(mod, f);
    }

    lr_global_t *g = lookup_global_cached(mod, name, &sym_id);
    if (!g) {
        if (sym_id == UINT32_MAX)
            return safe_undef(mod);
        g = lr_global_create(m, name, type, false);
        if (!g)
            return safe_undef(mod);
        cache_global_by_symbol(mod, sym_id, g);
    }
    return lc_value_global(mod, sym_id, m->type_ptr, g->name);
}

/* ---- Compat utility helpers ---- */

const char *lc_intrinsic_name(unsigned intrinsic_id) {
    switch (intrinsic_id) {
    case 1:  return "abs";
    case 2:  return "copysign";
    case 3:  return "cos";
    case 4:  return "ctlz";
    case 5:  return "ctpop";
    case 6:  return "cttz";
    case 7:  return "exp";
    case 8:  return "exp2";
    case 9:  return "fabs";
    case 10: return "floor";
    case 11: return "ceil";
    case 12: return "round";
    case 13: return "trunc";
    case 14: return "fma";
    case 15: return "fma";
    case 16: return "log";
    case 17: return "log2";
    case 18: return "log10";
    case 19: return "fmax";
    case 20: return "fmax";
    case 21: return "fmin";
    case 22: return "fmin";
    case 23: return "memcpy";
    case 24: return "memmove";
    case 25: return "memset";
    case 26: return "pow";
    case 27: return "powi";
    case 28: return "sin";
    case 29: return "sqrt";
    case 35: return "abort";
    default:
        return NULL;
    }
}

static bool text_contains(const char *haystack, size_t hay_len,
                          const char *needle) {
    size_t needle_len = 0;
    unsigned char first = 0;
    const char *p = NULL;
    const char *end = NULL;
    if (!haystack || !needle)
        return false;
    needle_len = strlen(needle);
    if (needle_len == 0 || hay_len < needle_len)
        return false;
    first = (unsigned char)needle[0];
    p = haystack;
    end = haystack + hay_len - needle_len + 1;
    while (p < end) {
        if ((unsigned char)*p == first &&
            memcmp(p, needle, needle_len) == 0)
            return true;
        p++;
    }
    return false;
}

bool lc_is_lfortran_jit_wrapper_ir(const char *asm_text, size_t len) {
    if (!asm_text || len == 0)
        return false;
    /* Wrapper IR is tiny; avoid O(n) scans on full modules. */
    if (len > 4096)
        return false;
    return text_contains(asm_text, len, "declare i32 @main(i32, i8**)\n") &&
           text_contains(asm_text, len,
                         "define i32 @__lfortran_jit_entry(i32 %argc, i8** %argv)") &&
           text_contains(asm_text, len,
                         "call i32 @main(i32 %argc, i8** %argv)") &&
           text_contains(asm_text, len, "ret i32 %ret");
}

lr_module_t *lc_build_lfortran_jit_wrapper_module(char *err, size_t errlen) {
    static const char wrapper_ir[] =
        "declare i32 @main(i32, i8**)\n"
        "define i32 @__lfortran_jit_entry(i32 %argc, i8** %argv) {\n"
        "entry:\n"
        "  %ret = call i32 @main(i32 %argc, i8** %argv)\n"
        "  ret i32 %ret\n"
        "}\n";
    return lr_parse_ll(wrapper_ir, strlen(wrapper_ir), err, errlen);
}

static size_t format_snprintf(char *buf, size_t buf_size,
                              const char *fmt, ...) {
    va_list ap;
    int n = 0;
    if (!buf || buf_size == 0 || !fmt)
        return 0;
    va_start(ap, fmt);
    n = vsnprintf(buf, buf_size, fmt, ap);
    va_end(ap);
    if (n < 0)
        return 0;
    if ((size_t)n >= buf_size)
        return buf_size - 1;
    return (size_t)n;
}

size_t lc_format_i64(char *buf, size_t buf_size, int64_t value) {
    return format_snprintf(buf, buf_size, "%lld", (long long)value);
}

size_t lc_format_u64(char *buf, size_t buf_size, uint64_t value) {
    return format_snprintf(buf, buf_size, "%llu",
                           (unsigned long long)value);
}

size_t lc_format_f64(char *buf, size_t buf_size, double value) {
    return format_snprintf(buf, buf_size, "%.6f", value);
}

size_t lc_format_ptr(char *buf, size_t buf_size, const void *ptr) {
    return format_snprintf(buf, buf_size, "%p", ptr);
}

static bool pack_constant_bytes_raw(const lc_value_t *value, const lr_type_t *ty,
                                    uint8_t *out, size_t out_size) {
    size_t need = 0;
    if (!ty || !out)
        return false;
    need = lr_type_size(ty);
    if (out_size < need)
        return false;
    if (need > 0)
        memset(out, 0, need);
    if (!value)
        return false;

    switch (value->kind) {
    case LC_VAL_CONST_NULL:
    case LC_VAL_CONST_UNDEF:
        return true;
    case LC_VAL_CONST_INT: {
        if (need == 0)
            return true;
        if (ty->kind == LR_TYPE_I1) {
            out[0] = value->const_int.val ? 1u : 0u;
            return true;
        }
        uint64_t raw = (uint64_t)value->const_int.val;
        size_t n = need < sizeof(raw) ? need : sizeof(raw);
        for (size_t i = 0; i < n; i++)
            out[i] = (uint8_t)((raw >> (8u * i)) & 0xffu);
        return true;
    }
    case LC_VAL_CONST_FP:
        if (ty->kind == LR_TYPE_FLOAT) {
            float f = (float)value->const_fp.val;
            size_t n = need < sizeof(f) ? need : sizeof(f);
            memcpy(out, &f, n);
        } else {
            double d = value->const_fp.val;
            size_t n = need < sizeof(d) ? need : sizeof(d);
            memcpy(out, &d, n);
        }
        return true;
    case LC_VAL_CONST_AGGREGATE:
        if (value->aggregate.data && value->aggregate.size > 0) {
            size_t n = need < value->aggregate.size ? need
                                                    : value->aggregate.size;
            memcpy(out, value->aggregate.data, n);
        }
        return true;
    case LC_VAL_GLOBAL:
        return true;
    default:
        return false;
    }
}

static uint64_t read_le_u64_partial(const uint8_t *bytes, size_t n) {
    uint64_t raw = 0;
    size_t lim = n < sizeof(raw) ? n : sizeof(raw);
    for (size_t i = 0; i < lim; i++)
        raw |= ((uint64_t)bytes[i]) << (8u * i);
    return raw;
}

static bool aggregate_extract_offset_and_type(lr_type_t *base_type,
                                              unsigned *indices,
                                              unsigned num_indices,
                                              size_t *out_offset,
                                              lr_type_t **out_type) {
    lr_type_t *cur = base_type;
    size_t off = 0;
    unsigned i;
    if (!base_type || !out_offset || !out_type)
        return false;
    for (i = 0; i < num_indices; i++) {
        unsigned idx = indices ? indices[i] : 0u;
        if (!cur)
            return false;
        if (cur->kind == LR_TYPE_STRUCT) {
            if (idx >= cur->struc.num_fields)
                return false;
            off += lr_struct_field_offset(cur, idx);
            cur = cur->struc.fields[idx];
            continue;
        }
        if (cur->kind == LR_TYPE_ARRAY || cur->kind == LR_TYPE_VECTOR) {
            size_t elem_sz;
            if ((uint64_t)idx >= cur->array.count)
                return false;
            elem_sz = lr_type_size(cur->array.elem);
            off += (size_t)idx * elem_sz;
            cur = cur->array.elem;
            continue;
        }
        return false;
    }
    *out_offset = off;
    *out_type = cur;
    return true;
}

static lc_value_t *const_extractvalue_fold(lc_module_compat_t *mod,
                                           lc_value_t *agg,
                                           unsigned *indices,
                                           unsigned num_indices) {
    lr_type_t *result_ty = NULL;
    size_t off = 0;
    size_t need = 0;
    const uint8_t *src = NULL;
    size_t avail = 0;
    if (!mod || !agg || agg->kind != LC_VAL_CONST_AGGREGATE || !agg->type)
        return NULL;
    if (!aggregate_extract_offset_and_type(agg->type, indices, num_indices,
                                           &off, &result_ty)) {
        return NULL;
    }
    if (!result_ty)
        return NULL;

    need = lr_type_size(result_ty);
    if (agg->aggregate.data && off < agg->aggregate.size) {
        src = (const uint8_t *)agg->aggregate.data + off;
        avail = agg->aggregate.size - off;
    }

    if (lc_type_is_integer(result_ty)) {
        unsigned width = lc_type_int_width(result_ty);
        uint64_t raw = read_le_u64_partial(src ? src : (const uint8_t *)"", avail);
        if (width > 0 && width < 64) {
            uint64_t mask = (UINT64_C(1) << width) - 1u;
            raw &= mask;
            if (raw & (UINT64_C(1) << (width - 1u)))
                raw |= ~mask;
        }
        return lc_value_const_int(mod, result_ty, (int64_t)raw,
                                  width ? width : 64u);
    }

    if (result_ty->kind == LR_TYPE_FLOAT) {
        float f = 0.0f;
        size_t n = avail < sizeof(f) ? avail : sizeof(f);
        if (src && n > 0)
            memcpy(&f, src, n);
        return lc_value_const_fp(mod, result_ty, (double)f, false);
    }

    if (result_ty->kind == LR_TYPE_DOUBLE) {
        double d = 0.0;
        size_t n = avail < sizeof(d) ? avail : sizeof(d);
        if (src && n > 0)
            memcpy(&d, src, n);
        return lc_value_const_fp(mod, result_ty, d, true);
    }

    if (result_ty->kind == LR_TYPE_PTR) {
        lc_const_value_meta_t *meta = lookup_const_value_meta(mod, agg);
        if (meta) {
            for (lc_const_reloc_meta_t *cr = meta->relocs; cr; cr = cr->next) {
                if (cr->offset == off && cr->symbol_name && cr->symbol_name[0]) {
                    uint32_t sym_id = compat_symbol_id(mod, cr->symbol_name);
                    if (sym_id != UINT32_MAX) {
                        lc_value_t *gv =
                            lc_value_global(mod, sym_id, result_ty, cr->symbol_name);
                        if (gv)
                            gv->global.offset = cr->addend;
                        return gv;
                    }
                }
            }
        }
        if (!src || avail == 0 || read_le_u64_partial(src, avail) == 0)
            return lc_value_const_null(mod, result_ty);
        return safe_undef(mod);
    }

    if (result_ty->kind == LR_TYPE_STRUCT ||
        result_ty->kind == LR_TYPE_ARRAY ||
        result_ty->kind == LR_TYPE_VECTOR) {
        lc_value_t *nested;
        uint8_t *bytes = NULL;
        if (need > 0) {
            size_t n;
            bytes = (uint8_t *)calloc(1, need);
            if (!bytes)
                return safe_undef(mod);
            n = avail < need ? avail : need;
            if (src && n > 0)
                memcpy(bytes, src, n);
        }
        nested = lc_value_const_aggregate(mod, result_ty, bytes, need);
        free(bytes);
        if (!nested)
            return safe_undef(mod);
        if (need > 0) {
            lc_const_value_meta_t *meta = lookup_const_value_meta(mod, agg);
            if (meta) {
                for (lc_const_reloc_meta_t *cr = meta->relocs; cr; cr = cr->next) {
                    if (!cr->symbol_name)
                        continue;
                    if (cr->offset >= off && cr->offset < off + need) {
                        (void)lc_value_const_aggregate_add_reloc(
                            mod, nested, cr->offset - off, cr->symbol_name, cr->addend
                        );
                    }
                }
            }
        }
        return nested;
    }

    return NULL;
}

static lc_value_t *materialize_const_aggregate_global(lc_module_compat_t *mod,
                                                      lc_value_t *agg) {
    static uint32_t constagg_seq = 0;
    char name[64];
    size_t size;
    lr_global_t *g;
    uint32_t sym_id;
    if (!mod || !agg || agg->kind != LC_VAL_CONST_AGGREGATE || !agg->type)
        return safe_undef(mod);

    size = lr_type_size(agg->type);
    if (size == 0)
        return safe_undef(mod);

    (void)snprintf(name, sizeof(name), ".lc.constagg.%u", constagg_seq++);
    g = lr_global_create(mod->mod, name, agg->type, true);
    if (!g)
        return safe_undef(mod);

    g->init_data = (uint8_t *)lr_arena_alloc(mod->mod->arena, size, 1);
    if (!g->init_data)
        return safe_undef(mod);
    memset(g->init_data, 0, size);
    if (agg->aggregate.data && agg->aggregate.size > 0) {
        size_t n = agg->aggregate.size < size ? agg->aggregate.size : size;
        memcpy(g->init_data, agg->aggregate.data, n);
    }
    g->init_size = size;

    {
        lc_const_value_meta_t *meta = lookup_const_value_meta(mod, agg);
        if (meta) {
            for (lc_const_reloc_meta_t *cr = meta->relocs; cr; cr = cr->next) {
                lr_reloc_t *r;
                if (!cr->symbol_name || cr->offset >= size)
                    continue;
                r = lr_arena_new(mod->mod->arena, lr_reloc_t);
                if (!r)
                    return safe_undef(mod);
                r->offset = cr->offset;
                r->addend = cr->addend;
                r->symbol_name = lr_arena_strdup(mod->mod->arena,
                                                 cr->symbol_name,
                                                 strlen(cr->symbol_name));
                r->next = g->relocs;
                g->relocs = r;
            }
        }
    }

    sym_id = compat_symbol_id(mod, name);
    if (sym_id != UINT32_MAX)
        cache_global_by_symbol(mod, sym_id, g);
    return lc_value_global(mod, sym_id, mod->mod->type_ptr, g->name);
}

bool lc_pack_constant_bytes(lc_value_t *value, lr_type_t *ty,
                             uint8_t *out, size_t out_size) {
    return pack_constant_bytes_raw(value, ty, out, out_size);
}

lc_value_t *lc_const_struct_from_values(lc_module_compat_t *mod,
                                         lr_type_t *struct_ty,
                                         lc_value_t **values,
                                         uint32_t num_values) {
    lc_value_t *agg = NULL;
    uint8_t *bytes = NULL;
    size_t total = 0;
    uint32_t n = 0;
    if (!mod || !struct_ty || struct_ty->kind != LR_TYPE_STRUCT)
        return NULL;

    total = lr_type_size(struct_ty);
    if (total > 0) {
        bytes = (uint8_t *)calloc(1, total);
        if (!bytes)
            return NULL;
    }

    n = struct_ty->struc.num_fields;
    if (num_values < n)
        n = num_values;

    for (uint32_t i = 0; i < n; i++) {
        lr_type_t *field_ty = struct_ty->struc.fields[i];
        size_t off = lr_struct_field_offset(struct_ty, i);
        size_t field_sz = lr_type_size(field_ty);
        if (!field_ty || !bytes || field_sz == 0)
            continue;
        if (off > total || field_sz > total - off)
            continue;
        (void)pack_constant_bytes_raw(values ? values[i] : NULL,
                                      field_ty, bytes + off, field_sz);
    }

    agg = lc_value_const_aggregate(mod, struct_ty, bytes, total);
    free(bytes);
    if (!agg)
        return NULL;

    for (uint32_t i = 0; i < n; i++) {
        lr_type_t *field_ty = struct_ty->struc.fields[i];
        lc_value_t *elem = values ? values[i] : NULL;
        size_t off = lr_struct_field_offset(struct_ty, i);
        if (!elem || !field_ty)
            continue;

        if (field_ty->kind == LR_TYPE_PTR &&
            elem->kind == LC_VAL_GLOBAL && elem->global.name) {
            if (lc_value_const_aggregate_add_reloc(mod, agg, off,
                                                   elem->global.name,
                                                   elem->global.offset) != 0)
                return NULL;
        }

        if (elem->kind == LC_VAL_CONST_AGGREGATE) {
            lc_const_value_meta_t *meta = lookup_const_value_meta(mod, elem);
            if (!meta)
                continue;
            for (lc_const_reloc_meta_t *cr = meta->relocs; cr; cr = cr->next) {
                if (!cr->symbol_name)
                    continue;
                if (lc_value_const_aggregate_add_reloc(mod, agg,
                        off + cr->offset, cr->symbol_name, cr->addend) != 0)
                    return NULL;
            }
        }
    }
    return agg;
}

lc_value_t *lc_const_array_from_values(lc_module_compat_t *mod,
                                        lr_type_t *array_ty,
                                        lc_value_t **values,
                                        uint32_t num_values) {
    lc_value_t *agg = NULL;
    lr_type_t *elem_ty = NULL;
    uint8_t *bytes = NULL;
    size_t total = 0;
    size_t elem_sz = 0;
    uint64_t n = 0;
    if (!mod || !array_ty ||
        (array_ty->kind != LR_TYPE_ARRAY && array_ty->kind != LR_TYPE_VECTOR))
        return NULL;

    elem_ty = array_ty->array.elem;
    total = lr_type_size(array_ty);
    elem_sz = lr_type_size(elem_ty);

    if (getenv("LIRIC_COMPAT_DEBUG_ARRAY_CONST")) {
        fprintf(stderr,
                "[lc_const_array_from_values] count=%llu num_values=%u elem_kind=%d elem_sz=%zu total=%zu\n",
                (unsigned long long)array_ty->array.count, num_values,
                elem_ty ? (int)elem_ty->kind : -1, elem_sz, total);
        for (uint32_t i = 0; i < num_values && i < 8; i++) {
            lc_value_t *v = values ? values[i] : NULL;
            fprintf(stderr, "  val[%u]: %s kind=%d ty_kind=%d agg_size=%zu int=%lld\n",
                    i, v ? "set" : "null", v ? (int)v->kind : -1,
                    (v && v->type) ? (int)v->type->kind : -1,
                    (v && v->kind == LC_VAL_CONST_AGGREGATE) ? v->aggregate.size : 0u,
                    (long long)((v && v->kind == LC_VAL_CONST_INT) ? v->const_int.val : 0));
        }
    }

    /* Some producers pass array constructors as a single aggregate value of
     * the full array type. Preserve that payload instead of treating it as
     * element[0], which would zero-fill the remaining elements. */
    if (num_values == 1 && values && values[0] &&
        values[0]->kind == LC_VAL_CONST_AGGREGATE &&
        values[0]->type &&
        (values[0]->type->kind == LR_TYPE_ARRAY ||
         values[0]->type->kind == LR_TYPE_VECTOR) &&
        lr_type_size(values[0]->type) == total) {
        lc_value_t *src_agg = values[0];
        lc_value_t *dst_agg = lc_value_const_aggregate(
            mod, array_ty, src_agg->aggregate.data, src_agg->aggregate.size);
        if (!dst_agg)
            return NULL;
        {
            lc_const_value_meta_t *meta = lookup_const_value_meta(mod, src_agg);
            if (meta) {
                for (lc_const_reloc_meta_t *cr = meta->relocs; cr; cr = cr->next) {
                    if (!cr->symbol_name)
                        continue;
                    if (lc_value_const_aggregate_add_reloc(mod, dst_agg,
                            cr->offset, cr->symbol_name, cr->addend) != 0)
                        return NULL;
                }
            }
        }
        return dst_agg;
    }

    if (total > 0) {
        bytes = (uint8_t *)calloc(1, total);
        if (!bytes)
            return NULL;
    }

    n = array_ty->array.count;
    if ((uint64_t)num_values < n)
        n = num_values;

    for (uint64_t i = 0; i < n; i++) {
        size_t off = (size_t)i * elem_sz;
        if (!elem_ty || !bytes || elem_sz == 0)
            continue;
        if (off > total || elem_sz > total - off)
            continue;
        (void)pack_constant_bytes_raw(values ? values[i] : NULL,
                                      elem_ty, bytes + off, elem_sz);
    }

    agg = lc_value_const_aggregate(mod, array_ty, bytes, total);
    free(bytes);
    if (!agg)
        return NULL;

    for (uint64_t i = 0; i < n; i++) {
        lc_value_t *elem = values ? values[i] : NULL;
        size_t off = (size_t)i * elem_sz;
        if (!elem)
            continue;
        if (elem->kind == LC_VAL_CONST_AGGREGATE) {
            lc_const_value_meta_t *meta = lookup_const_value_meta(mod, elem);
            if (!meta)
                continue;
            for (lc_const_reloc_meta_t *cr = meta->relocs; cr; cr = cr->next) {
                if (!cr->symbol_name)
                    continue;
                if (lc_value_const_aggregate_add_reloc(mod, agg,
                        off + cr->offset, cr->symbol_name, cr->addend) != 0)
                    return NULL;
            }
        }
    }

    if (elem_ty && elem_ty->kind == LR_TYPE_PTR) {
        for (uint64_t i = 0; i < n; i++) {
            lc_value_t *elem = values ? values[i] : NULL;
            if (!elem || elem->kind != LC_VAL_GLOBAL || !elem->global.name)
                continue;
            size_t off = (size_t)i * elem_sz;
            if (lc_value_const_aggregate_add_reloc(mod, agg, off,
                                                   elem->global.name,
                                                   elem->global.offset) != 0)
                return NULL;
        }
    }

    return agg;
}

int lc_const_gep_compute_offset(lr_type_t *base_type,
                                 lc_value_t **indices,
                                 uint32_t num_indices,
                                 int64_t *out_offset) {
    int64_t total = 0;
    const lr_type_t *cur_ty = base_type;
    if (!base_type || !out_offset)
        return -1;

    for (uint32_t i = 0; i < num_indices; i++) {
        lc_value_t *idx = indices ? indices[i] : NULL;
        lr_operand_desc_t d = lc_value_to_desc(idx);
        lr_operand_t op;
        lr_gep_step_t step;

        if (d.kind != LR_OP_KIND_IMM_I64)
            return -1;
        op = compat_desc_to_operand(&d);
        if (!lr_gep_analyze_step(cur_ty, i == 0, &op, &step))
            return -1;
        if (!step.is_const)
            return -1;

        if ((step.const_byte_offset > 0 &&
             total > INT64_MAX - step.const_byte_offset) ||
            (step.const_byte_offset < 0 &&
             total < INT64_MIN - step.const_byte_offset)) {
            return -1;
        }
        total += step.const_byte_offset;
        cur_ty = step.next_type;
    }

    *out_offset = total;
    return 0;
}

/* ---- Function ---- */

static lc_value_t *create_func_value(lc_module_compat_t *mod,
                                      lr_func_t *func) {
    uint32_t sym_id = compat_symbol_id(mod, func->name);
    if (sym_id == UINT32_MAX)
        return safe_undef(mod);
    lc_value_t *v = lc_value_global(mod, sym_id, mod->mod->type_ptr,
                                     func->name);
    v->global.func = func;
    cache_func_by_symbol(mod, sym_id, func);
    func_cache_add(mod, v);
    return v;
}

lc_value_t *lc_func_create(lc_module_compat_t *mod, const char *name,
                            lr_type_t *func_type) {
    if (!mod) return safe_undef(NULL);
    if (name && name[0]) {
        lr_func_t *existing = lookup_func_cached(mod, name, NULL);
        if (existing) {
            /* Promote an existing declaration to a definition placeholder.
             * This keeps one canonical symbol and avoids declare/define
             * duplication under different internal names. */
            existing->is_decl = false;
            return create_func_value(mod, existing);
        }
    }
    lr_type_t *ret = mod->mod->type_void;
    lr_type_t **params = NULL;
    uint32_t num_params = 0;
    bool vararg = false;
    if (func_type && func_type->kind == LR_TYPE_FUNC) {
        ret = func_type->func.ret ? func_type->func.ret : ret;
        params = func_type->func.params;
        num_params = func_type->func.num_params;
        vararg = func_type->func.vararg;
    }
    lr_func_t *f = lr_func_create(mod->mod, name, ret,
                                   params, num_params, vararg);
    return create_func_value(mod, f);
}

lc_value_t *lc_func_declare(lc_module_compat_t *mod, const char *name,
                             lr_type_t *func_type) {
    if (!mod) return safe_undef(NULL);
    if (name && name[0]) {
        lr_func_t *existing = lookup_func_cached(mod, name, NULL);
        if (existing)
            return create_func_value(mod, existing);
    }
    lr_type_t *ret = mod->mod->type_void;
    lr_type_t **params = NULL;
    uint32_t num_params = 0;
    bool vararg = false;
    if (func_type && func_type->kind == LR_TYPE_FUNC) {
        ret = func_type->func.ret ? func_type->func.ret : ret;
        params = func_type->func.params;
        num_params = func_type->func.num_params;
        vararg = func_type->func.vararg;
    }
    lr_func_t *f = lr_func_declare(mod->mod, name, ret,
                                    params, num_params, vararg);
    return create_func_value(mod, f);
}

lr_func_t *lc_value_get_func(lc_value_t *val) {
    if (!val || val->kind != LC_VAL_GLOBAL) return NULL;
    return val->global.func;
}

lc_value_t *lc_func_get_arg(lc_module_compat_t *mod, lc_value_t *func_val,
                             unsigned idx) {
    if (!func_val || func_val->kind != LC_VAL_GLOBAL)
        return safe_undef(mod);
    lr_func_t *f = func_val->global.func;
    if (!f && mod) {
        f = lookup_func_cached(mod, func_val->global.name, NULL);
    }
    if (!f || idx >= f->num_params) return safe_undef(mod);
    return lc_value_argument(mod, idx, f->param_types[idx], f);
}

unsigned lc_func_arg_count(lc_value_t *func_val) {
    if (!func_val || func_val->kind != LC_VAL_GLOBAL) return 0;
    lr_func_t *f = func_val->global.func;
    if (!f) return 0;
    return f->num_params;
}

/* ---- Basic block ---- */

lc_value_t *lc_block_create(lc_module_compat_t *mod, lr_func_t *func,
                             const char *name) {
    if (!mod || !func) return safe_undef(mod);
    lr_block_t *b = lr_block_create(func, mod->mod->arena, name);
    lc_value_t *v = lc_value_block_ref(mod, b);
    if (v) v->block.func = func;
    return v ? v : safe_undef(mod);
}

static int detached_block_index(lc_module_compat_t *mod, lr_block_t *block) {
    if (!mod || !block) return -1;
    for (uint32_t i = 0; i < mod->detached_count; i++) {
        if (mod->detached_blocks[i] == block)
            return (int)i;
    }
    return -1;
}

static int detached_block_add(lc_module_compat_t *mod, lr_block_t *block) {
    if (!mod || !block) return -1;
    if (detached_block_index(mod, block) >= 0)
        return 0;
    if (mod->detached_count == mod->detached_cap) {
        uint32_t new_cap = mod->detached_cap ? mod->detached_cap * 2u : 16u;
        lr_block_t **new_blocks = (lr_block_t **)realloc(
            mod->detached_blocks, sizeof(*new_blocks) * new_cap);
        if (!new_blocks)
            return -1;
        mod->detached_blocks = new_blocks;
        mod->detached_cap = new_cap;
    }
    mod->detached_blocks[mod->detached_count++] = block;
    return 0;
}

static void detached_block_remove_idx(lc_module_compat_t *mod, uint32_t idx) {
    if (!mod || idx >= mod->detached_count)
        return;
    mod->detached_blocks[idx] =
        mod->detached_blocks[mod->detached_count - 1u];
    mod->detached_count--;
}

lc_value_t *lc_block_create_detached(lc_module_compat_t *mod, lr_func_t *func,
                                      const char *name) {
    if (!mod || !func) return safe_undef(mod);
    if (!name) name = "";
    lr_arena_t *arena = mod->mod->arena;
    lr_block_t *b = lr_arena_new(arena, lr_block_t);
    if (!b) return safe_undef(mod);
    b->name = lr_arena_strdup(arena, name, strlen(name));
    b->id = func->num_blocks++;
    b->func = func;
    b->next = NULL;
    func->block_array = NULL;
    func->linear_inst_array = NULL;
    func->block_inst_offsets = NULL;
    func->num_linear_insts = 0;
    if (detached_block_add(mod, b) != 0)
        return safe_undef(mod);
    lc_value_t *v = lc_value_block_ref(mod, b);
    if (v) v->block.func = func;
    return v ? v : safe_undef(mod);
}

int lc_block_attach(lc_module_compat_t *mod, lr_block_t *block) {
    if (!mod || !block) return -1;
    int idx = detached_block_index(mod, block);
    if (idx < 0)
        return 0;
    lr_func_t *func = block->func;
    if (!func) {
        detached_block_remove_idx(mod, (uint32_t)idx);
        return -1;
    }
    block->next = NULL;
    if (!func->first_block) {
        func->first_block = block;
        func->is_decl = false;
    } else {
        func->last_block->next = block;
    }
    func->last_block = block;
    func->block_array = NULL;
    func->linear_inst_array = NULL;
    func->block_inst_offsets = NULL;
    func->num_linear_insts = 0;
    detached_block_remove_idx(mod, (uint32_t)idx);
    return 0;
}

lr_block_t *lc_value_get_block(lc_value_t *val) {
    if (!val || val->kind != LC_VAL_BLOCK) return NULL;
    return val->block.block;
}

lr_func_t *lc_value_get_block_func(lc_value_t *val) {
    if (!val || val->kind != LC_VAL_BLOCK) return NULL;
    return val->block.func;
}

lc_phi_node_t *lc_value_get_phi_node(lc_value_t *val) {
    if (!val || val->kind != LC_VAL_VREG) return NULL;
    return (lc_phi_node_t *)val->vreg.phi_node;
}

lr_type_t *lc_value_get_alloca_type(lc_value_t *val) {
    if (!val || val->kind != LC_VAL_VREG) return NULL;
    return val->vreg.alloc_type;
}

bool lc_block_has_terminator(lr_block_t *block) {
    if (!block || !block->last) return false;
    lr_opcode_t op = block->last->op;
    return op == LR_OP_RET || op == LR_OP_RET_VOID || op == LR_OP_BR
        || op == LR_OP_CONDBR || op == LR_OP_UNREACHABLE;
}

/* ---- Global variable ---- */

static size_t global_storage_size(const lr_global_t *g) {
    size_t sz;
    if (!g)
        return 0;
    sz = lr_type_size(g->type);
    if (sz == 0)
        sz = sizeof(void *);
    return sz;
}

static int global_set_data_zeros(lc_module_compat_t *mod, lr_global_t *g,
                                 uint8_t **out_buf, size_t *out_size) {
    size_t gsize;
    uint8_t *buf;
    if (!mod || !g || !out_buf || !out_size)
        return -1;
    gsize = global_storage_size(g);
    buf = (uint8_t *)lr_arena_alloc_uninit(mod->mod->arena, gsize, 1);
    if (!buf)
        return -1;
    memset(buf, 0, gsize);
    *out_buf = buf;
    *out_size = gsize;
    return 0;
}

static int global_add_reloc(lc_module_compat_t *mod, lr_global_t *g,
                            size_t offset, const char *sym_name,
                            int64_t addend) {
    lr_reloc_t *r;
    if (!mod || !g || !sym_name)
        return -1;
    r = lr_arena_new(mod->mod->arena, lr_reloc_t);
    if (!r)
        return -1;
    r->offset = offset;
    r->addend = addend;
    r->symbol_name = lr_arena_strdup(mod->mod->arena, sym_name,
                                     strlen(sym_name));
    r->next = g->relocs;
    g->relocs = r;
    return 0;
}

lc_value_t *lc_global_create(lc_module_compat_t *mod, const char *name,
                             lr_type_t *type, bool is_const,
                             const void *init_data, size_t init_size) {
    const char *actual_name = name;
    char auto_name[32];
    char *unique_name = NULL;
    uint32_t sym_id = UINT32_MAX;
    lr_global_t *g = NULL;
    if (!mod || !mod->mod)
        return safe_undef(mod);
    if (!name || name[0] == '\0') {
        snprintf(auto_name, sizeof(auto_name), ".str.%u",
                 mod->mod->num_globals);
        actual_name = auto_name;
    }
    /* If this global was referenced earlier as an unresolved external,
     * materialize the definition in-place. For already-defined globals
     * we must create a distinct symbol (LLVM-style auto-rename). */
    g = lookup_global_cached(mod, actual_name, &sym_id);
    if (g && g->is_external) {
        if (type)
            g->type = type;
        g->is_const = is_const;
        g->is_external = false;
        if (init_data && init_size > 0) {
            g->init_data = lr_arena_alloc_uninit(mod->mod->arena, init_size, 1);
            memcpy(g->init_data, init_data, init_size);
            g->init_size = init_size;
        }
        if (sym_id == UINT32_MAX)
            sym_id = compat_symbol_id(mod, g->name);
        if (sym_id == UINT32_MAX)
            return safe_undef(mod);
        cache_global_by_symbol(mod, sym_id, g);
        return lc_value_global(mod, sym_id, mod->mod->type_ptr, g->name);
    }
    unique_name = make_unique_symbol_name(mod, actual_name);
    if (!unique_name)
        return safe_undef(mod);
    g = lr_global_create(mod->mod, unique_name, type, is_const);
    free(unique_name);
    if (init_data && init_size > 0) {
        g->init_data = lr_arena_alloc_uninit(mod->mod->arena, init_size, 1);
        memcpy(g->init_data, init_data, init_size);
        g->init_size = init_size;
    }
    sym_id = compat_symbol_id(mod, g->name);
    if (sym_id == UINT32_MAX)
        return safe_undef(mod);
    cache_global_by_symbol(mod, sym_id, g);
    return lc_value_global(mod, sym_id, mod->mod->type_ptr, g->name);
}

lc_value_t *lc_global_declare(lc_module_compat_t *mod, const char *name,
                              lr_type_t *type) {
    char *unique_name;
    lr_global_t *g;
    if (!mod || !mod->mod || !name || !name[0])
        return safe_undef(mod);
    unique_name = make_unique_symbol_name(mod, name);
    if (!unique_name)
        return safe_undef(mod);
    g = lr_global_create(mod->mod, unique_name, type, false);
    free(unique_name);
    g->is_external = true;
    uint32_t sym_id = compat_symbol_id(mod, g->name);
    if (sym_id == UINT32_MAX)
        return safe_undef(mod);
    cache_global_by_symbol(mod, sym_id, g);
    return lc_value_global(mod, sym_id, mod->mod->type_ptr, g->name);
}

lc_value_t *lc_global_lookup(lc_module_compat_t *mod, const char *name) {
    uint32_t sym_id = UINT32_MAX;
    lr_global_t *g;
    if (!mod || !name || !name[0])
        return NULL;
    g = lookup_global_cached(mod, name, &sym_id);
    if (!g || sym_id == UINT32_MAX)
        return NULL;
    return lc_value_global(mod, sym_id, mod->mod->type_ptr, g->name);
}

int lc_global_set_initializer(lc_module_compat_t *mod, lc_value_t *global_val,
                               lc_value_t *init_val) {
    lr_global_t *g = NULL;
    uint8_t *buf = NULL;
    size_t gsize = 0;
    if (!mod || !global_val || global_val->kind != LC_VAL_GLOBAL || !init_val)
        return -1;

    if (global_val->global.name)
        g = lookup_global_cached(mod, global_val->global.name, NULL);
    if (!g && global_val->global.id < mod->sym_cache_cap)
        g = mod->global_by_sym[global_val->global.id];
    if (!g)
        return -1;

    if (global_set_data_zeros(mod, g, &buf, &gsize) != 0)
        return -1;

    /* Reset to a plain data initializer. Relocs are rebuilt if needed. */
    g->relocs = NULL;

    switch (init_val->kind) {
    case LC_VAL_CONST_NULL:
    case LC_VAL_CONST_UNDEF:
        break;
    case LC_VAL_CONST_INT: {
        uint64_t raw = (uint64_t)init_val->const_int.val;
        if (g->type && g->type->kind == LR_TYPE_I1)
            buf[0] = init_val->const_int.val ? 1u : 0u;
        else {
            size_t n = gsize < sizeof(raw) ? gsize : sizeof(raw);
            for (size_t i = 0; i < n; i++)
                buf[i] = (uint8_t)((raw >> (8u * i)) & 0xffu);
        }
        break;
    }
    case LC_VAL_CONST_FP:
        if (g->type && g->type->kind == LR_TYPE_FLOAT) {
            float f = (float)init_val->const_fp.val;
            size_t n = gsize < sizeof(f) ? gsize : sizeof(f);
            memcpy(buf, &f, n);
        } else {
            double d = init_val->const_fp.val;
            size_t n = gsize < sizeof(d) ? gsize : sizeof(d);
            memcpy(buf, &d, n);
        }
        break;
    case LC_VAL_CONST_AGGREGATE:
        if (init_val->aggregate.data && init_val->aggregate.size > 0) {
            size_t n = init_val->aggregate.size < gsize
                ? init_val->aggregate.size : gsize;
            memcpy(buf, init_val->aggregate.data, n);
        }
        {
            lc_const_value_meta_t *meta =
                lookup_const_value_meta(mod, init_val);
            if (meta) {
                for (lc_const_reloc_meta_t *cr = meta->relocs;
                     cr; cr = cr->next) {
                    if (!cr->symbol_name)
                        continue;
                    if (global_add_reloc(mod, g, cr->offset,
                                         cr->symbol_name, cr->addend) != 0)
                        return -1;
                }
            }
        }
        break;
    case LC_VAL_GLOBAL:
        if (!init_val->global.name ||
            global_add_reloc(mod, g, 0, init_val->global.name,
                             init_val->global.offset) != 0)
            return -1;
        break;
    default:
        return -1;
    }

    g->init_data = buf;
    g->init_size = gsize;
    g->is_external = false;
    return 0;
}

bool lc_global_has_initializer(lc_module_compat_t *mod, lc_value_t *global_val) {
    lr_global_t *g = NULL;
    if (!mod || !global_val || global_val->kind != LC_VAL_GLOBAL)
        return false;
    if (global_val->global.name)
        g = lookup_global_cached(mod, global_val->global.name, NULL);
    if (!g && global_val->global.id < mod->sym_cache_cap)
        g = mod->global_by_sym[global_val->global.id];
    if (!g)
        return false;
    return g->init_data != NULL && g->init_size > 0;
}

/* ---- Internal builder helpers ---- */

static lc_value_t *wrap_vreg(lc_module_compat_t *mod, uint32_t vreg_id,
                              lr_type_t *type, lr_func_t *func) {
    return lc_value_vreg(mod, vreg_id, type, func);
}

static uint32_t build_binop(lc_module_compat_t *mod, lr_block_t *b, lr_func_t *f,
                            lr_opcode_t op, lr_type_t *ty,
                            lr_operand_desc_t lhs, lr_operand_desc_t rhs) {
    lr_inst_desc_t inst;
    lr_operand_desc_t ops[2];
    memset(&inst, 0, sizeof(inst));
    ops[0] = lhs;
    ops[1] = rhs;
    inst.op = op;
    inst.type = ty;
    inst.operands = ops;
    inst.num_operands = 2;
    return compat_emit(mod, b, f, &inst);
}

static uint32_t build_cast(lc_module_compat_t *mod, lr_block_t *b, lr_func_t *f,
                           lr_opcode_t op, lr_type_t *to_type,
                           lr_operand_desc_t val) {
    lr_inst_desc_t inst;
    lr_operand_desc_t ops[1];
    memset(&inst, 0, sizeof(inst));
    ops[0] = val;
    inst.op = op;
    inst.type = to_type;
    inst.operands = ops;
    inst.num_operands = 1;
    return compat_emit(mod, b, f, &inst);
}

static lc_value_t *compat_binop(lc_module_compat_t *mod, lr_block_t *b,
                                  lr_func_t *f, lr_opcode_t op,
                                  lc_value_t *lhs, lc_value_t *rhs) {
    if (!mod || !b || !f || !lhs) return safe_undef(mod);
    lr_type_t *ty = lhs->type ? lhs->type : mod->mod->type_i64;
    uint32_t v = build_binop(mod, b, f, op, ty,
                             lc_value_to_desc(lhs), lc_value_to_desc(rhs));
    return wrap_vreg(mod, v, ty, f);
}

static lc_value_t *compat_cast(lc_module_compat_t *mod, lr_block_t *b,
                                 lr_func_t *f, lr_opcode_t op,
                                 lc_value_t *val, lr_type_t *to_type) {
    if (!mod || !b || !f || !val) return safe_undef(mod);

    /* Keep constant null/pointer casts well-typed in emitted IR. */
    if (op == LR_OP_PTRTOINT &&
        to_type && lc_type_is_integer(to_type) &&
        val->kind == LC_VAL_CONST_NULL) {
        unsigned width = lc_type_int_width(to_type);
        if (width == 0) width = 64;
        return lc_value_const_int(mod, to_type, 0, width);
    }
    if (op == LR_OP_INTTOPTR &&
        to_type && to_type->kind == LR_TYPE_PTR &&
        val->kind == LC_VAL_CONST_INT &&
        val->const_int.val == 0) {
        return lc_value_const_null(mod, to_type);
    }

    uint32_t v = build_cast(mod, b, f, op, to_type, lc_value_to_desc(val));
    return wrap_vreg(mod, v, to_type, f);
}

static lc_value_t *compat_icmp(lc_module_compat_t *mod, lr_block_t *b,
                                 lr_func_t *f, int pred,
                                 lc_value_t *lhs, lc_value_t *rhs) {
    lr_inst_desc_t inst;
    lr_operand_desc_t ops[2];
    uint32_t dest;
    if (!mod || !b || !f) return safe_undef(mod);
    memset(&inst, 0, sizeof(inst));
    ops[0] = lc_value_to_desc(lhs);
    ops[1] = lc_value_to_desc(rhs);
    inst.op = LR_OP_ICMP;
    inst.type = mod->mod->type_i1;
    inst.operands = ops;
    inst.num_operands = 2;
    inst.icmp_pred = pred;
    dest = compat_emit(mod, b, f, &inst);
    return wrap_vreg(mod, dest, mod->mod->type_i1, f);
}

static lc_value_t *compat_fcmp(lc_module_compat_t *mod, lr_block_t *b,
                                 lr_func_t *f, int pred,
                                 lc_value_t *lhs, lc_value_t *rhs) {
    lr_inst_desc_t inst;
    lr_operand_desc_t ops[2];
    uint32_t dest;
    if (!mod || !b || !f) return safe_undef(mod);
    memset(&inst, 0, sizeof(inst));
    ops[0] = lc_value_to_desc(lhs);
    ops[1] = lc_value_to_desc(rhs);
    inst.op = LR_OP_FCMP;
    inst.type = mod->mod->type_i1;
    inst.operands = ops;
    inst.num_operands = 2;
    inst.fcmp_pred = pred;
    dest = compat_emit(mod, b, f, &inst);
    return wrap_vreg(mod, dest, mod->mod->type_i1, f);
}

/* ---- Arithmetic ---- */

lc_value_t *lc_create_add(lc_module_compat_t *mod, lr_block_t *b,
                           lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                           const char *name) {
    (void)name;
    return compat_binop(mod, b, f, LR_OP_ADD, lhs, rhs);
}

lc_value_t *lc_create_sub(lc_module_compat_t *mod, lr_block_t *b,
                           lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                           const char *name) {
    (void)name;
    return compat_binop(mod, b, f, LR_OP_SUB, lhs, rhs);
}

lc_value_t *lc_create_mul(lc_module_compat_t *mod, lr_block_t *b,
                           lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                           const char *name) {
    (void)name;
    return compat_binop(mod, b, f, LR_OP_MUL, lhs, rhs);
}

lc_value_t *lc_create_sdiv(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                            const char *name) {
    (void)name;
    return compat_binop(mod, b, f, LR_OP_SDIV, lhs, rhs);
}

lc_value_t *lc_create_srem(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                            const char *name) {
    (void)name;
    return compat_binop(mod, b, f, LR_OP_SREM, lhs, rhs);
}

lc_value_t *lc_create_udiv(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                            const char *name) {
    (void)name;
    /* liric does not have a distinct udiv opcode; use sdiv as approximation */
    return compat_binop(mod, b, f, LR_OP_SDIV, lhs, rhs);
}

lc_value_t *lc_create_urem(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                            const char *name) {
    (void)name;
    /* liric does not have a distinct urem opcode; use srem as approximation */
    return compat_binop(mod, b, f, LR_OP_SREM, lhs, rhs);
}

lc_value_t *lc_create_neg(lc_module_compat_t *mod, lr_block_t *b,
                           lr_func_t *f, lc_value_t *val, const char *name) {
    (void)name;
    if (!val) return safe_undef(mod);
    lc_value_t *zero = lc_value_const_int(mod, val->type, 0,
                                           lc_type_int_width(val->type));
    return compat_binop(mod, b, f, LR_OP_SUB, zero, val);
}

/* ---- Bitwise ---- */

lc_value_t *lc_create_and(lc_module_compat_t *mod, lr_block_t *b,
                           lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                           const char *name) {
    (void)name;
    return compat_binop(mod, b, f, LR_OP_AND, lhs, rhs);
}

lc_value_t *lc_create_or(lc_module_compat_t *mod, lr_block_t *b,
                          lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                          const char *name) {
    (void)name;
    return compat_binop(mod, b, f, LR_OP_OR, lhs, rhs);
}

lc_value_t *lc_create_xor(lc_module_compat_t *mod, lr_block_t *b,
                           lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                           const char *name) {
    (void)name;
    return compat_binop(mod, b, f, LR_OP_XOR, lhs, rhs);
}

lc_value_t *lc_create_shl(lc_module_compat_t *mod, lr_block_t *b,
                           lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                           const char *name) {
    (void)name;
    return compat_binop(mod, b, f, LR_OP_SHL, lhs, rhs);
}

lc_value_t *lc_create_lshr(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                            const char *name) {
    (void)name;
    return compat_binop(mod, b, f, LR_OP_LSHR, lhs, rhs);
}

lc_value_t *lc_create_ashr(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                            const char *name) {
    (void)name;
    return compat_binop(mod, b, f, LR_OP_ASHR, lhs, rhs);
}

lc_value_t *lc_create_not(lc_module_compat_t *mod, lr_block_t *b,
                           lr_func_t *f, lc_value_t *val, const char *name) {
    (void)name;
    int64_t mask = -1;
    unsigned width;
    if (!val) return safe_undef(mod);
    width = lc_type_int_width(val->type);
    /* Keep i1 canonical as xor with 1 (not -1) to match parser path. */
    if (width == 1)
        mask = 1;
    lc_value_t *neg1 = lc_value_const_int(mod, val->type, mask, width);
    return compat_binop(mod, b, f, LR_OP_XOR, val, neg1);
}

/* ---- FP arithmetic ---- */

lc_value_t *lc_create_fadd(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                            const char *name) {
    (void)name;
    return compat_binop(mod, b, f, LR_OP_FADD, lhs, rhs);
}

lc_value_t *lc_create_fsub(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                            const char *name) {
    (void)name;
    return compat_binop(mod, b, f, LR_OP_FSUB, lhs, rhs);
}

lc_value_t *lc_create_fmul(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                            const char *name) {
    (void)name;
    return compat_binop(mod, b, f, LR_OP_FMUL, lhs, rhs);
}

lc_value_t *lc_create_fdiv(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                            const char *name) {
    (void)name;
    return compat_binop(mod, b, f, LR_OP_FDIV, lhs, rhs);
}

lc_value_t *lc_create_fneg(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *val,
                            const char *name) {
    (void)name;
    if (!val) return safe_undef(mod);
    return compat_cast(mod, b, f, LR_OP_FNEG, val, val->type);
}

/* ---- Comparison ---- */

lc_value_t *lc_create_icmp_eq(lc_module_compat_t *mod, lr_block_t *b,
                               lr_func_t *f, lc_value_t *lhs,
                               lc_value_t *rhs, const char *name) {
    (void)name;
    return compat_icmp(mod, b, f, LR_ICMP_EQ, lhs, rhs);
}

lc_value_t *lc_create_icmp_ne(lc_module_compat_t *mod, lr_block_t *b,
                               lr_func_t *f, lc_value_t *lhs,
                               lc_value_t *rhs, const char *name) {
    (void)name;
    return compat_icmp(mod, b, f, LR_ICMP_NE, lhs, rhs);
}

lc_value_t *lc_create_icmp_slt(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name) {
    (void)name;
    return compat_icmp(mod, b, f, LR_ICMP_SLT, lhs, rhs);
}

lc_value_t *lc_create_icmp_sle(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name) {
    (void)name;
    return compat_icmp(mod, b, f, LR_ICMP_SLE, lhs, rhs);
}

lc_value_t *lc_create_icmp_sgt(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name) {
    (void)name;
    return compat_icmp(mod, b, f, LR_ICMP_SGT, lhs, rhs);
}

lc_value_t *lc_create_icmp_sge(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name) {
    (void)name;
    return compat_icmp(mod, b, f, LR_ICMP_SGE, lhs, rhs);
}

lc_value_t *lc_create_icmp_ult(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name) {
    (void)name;
    return compat_icmp(mod, b, f, LR_ICMP_ULT, lhs, rhs);
}

lc_value_t *lc_create_icmp_uge(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name) {
    (void)name;
    return compat_icmp(mod, b, f, LR_ICMP_UGE, lhs, rhs);
}

lc_value_t *lc_create_icmp_ugt(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name) {
    (void)name;
    return compat_icmp(mod, b, f, LR_ICMP_UGT, lhs, rhs);
}

lc_value_t *lc_create_icmp_ule(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name) {
    (void)name;
    return compat_icmp(mod, b, f, LR_ICMP_ULE, lhs, rhs);
}

lc_value_t *lc_create_fcmp_oeq(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name) {
    (void)name;
    return compat_fcmp(mod, b, f, LR_FCMP_OEQ, lhs, rhs);
}

lc_value_t *lc_create_fcmp_one(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name) {
    (void)name;
    return compat_fcmp(mod, b, f, LR_FCMP_ONE, lhs, rhs);
}

lc_value_t *lc_create_fcmp_olt(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name) {
    (void)name;
    return compat_fcmp(mod, b, f, LR_FCMP_OLT, lhs, rhs);
}

lc_value_t *lc_create_fcmp_ole(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name) {
    (void)name;
    return compat_fcmp(mod, b, f, LR_FCMP_OLE, lhs, rhs);
}

lc_value_t *lc_create_fcmp_ogt(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name) {
    (void)name;
    return compat_fcmp(mod, b, f, LR_FCMP_OGT, lhs, rhs);
}

lc_value_t *lc_create_fcmp_oge(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name) {
    (void)name;
    return compat_fcmp(mod, b, f, LR_FCMP_OGE, lhs, rhs);
}

lc_value_t *lc_create_fcmp_une(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name) {
    (void)name;
    return compat_fcmp(mod, b, f, LR_FCMP_UNE, lhs, rhs);
}

lc_value_t *lc_create_fcmp_ord(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name) {
    (void)name;
    return compat_fcmp(mod, b, f, LR_FCMP_ORD, lhs, rhs);
}

lc_value_t *lc_create_fcmp_uno(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name) {
    (void)name;
    return compat_fcmp(mod, b, f, LR_FCMP_UNO, lhs, rhs);
}

lc_value_t *lc_create_fcmp_ueq(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name) {
    (void)name;
    return compat_fcmp(mod, b, f, LR_FCMP_UEQ, lhs, rhs);
}

/* ---- Memory ---- */

lc_alloca_inst_t *lc_create_alloca(lc_module_compat_t *mod, lr_block_t *b,
                                    lr_func_t *f, lr_type_t *type,
                                    lc_value_t *array_size,
                                    const char *name) {
    lr_inst_desc_t inst;
    lr_operand_desc_t ops[1];
    uint32_t dest;
    lc_alloca_inst_t *ai;
    lc_value_t *result;
    (void)name;
    if (!mod || !b || !f || !type) {
        ai = (lc_alloca_inst_t *)calloc(1, sizeof(*ai));
        if (!ai) return NULL;
        ai->result = safe_undef(mod);
        ai->alloc_type = type;
        return ai;
    }
    memset(&inst, 0, sizeof(inst));
    inst.op = LR_OP_ALLOCA;
    inst.type = type;
    if (array_size) {
        ops[0] = lc_value_to_desc(array_size);
        inst.operands = ops;
        inst.num_operands = 1;
    }
    dest = compat_emit(mod, b, f, &inst);
    ai = (lc_alloca_inst_t *)malloc(sizeof(*ai));
    if (!ai)
        return NULL;
    result = wrap_vreg(mod, dest, mod->mod->type_ptr, f);
    result->vreg.alloc_type = type;
    ai->result = result;
    ai->alloc_type = type;
    return ai;
}

lc_value_t *lc_create_load(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lr_type_t *ty, lc_value_t *ptr,
                            const char *name) {
    lr_inst_desc_t inst;
    lr_operand_desc_t ops[1];
    uint32_t dest;
    (void)name;
    if (!mod || !b || !f) return safe_undef(mod);
    memset(&inst, 0, sizeof(inst));
    ops[0] = lc_value_to_desc(ptr);
    inst.op = LR_OP_LOAD;
    inst.type = ty;
    inst.operands = ops;
    inst.num_operands = 1;
    dest = compat_emit(mod, b, f, &inst);
    return wrap_vreg(mod, dest, ty, f);
}

void lc_create_store(lc_module_compat_t *mod, lr_block_t *b,
                     lc_value_t *val, lc_value_t *ptr) {
    lr_inst_desc_t inst;
    lr_operand_desc_t ops[2];
    lr_func_t *f;
    if (!mod || !b) return;
    f = b->func;
    if (val && val->kind == LC_VAL_CONST_AGGREGATE && val->type && f) {
        size_t nbytes = lr_type_size(val->type);
        lc_value_t *src = materialize_const_aggregate_global(mod, val);
        lc_value_t *sz = lc_value_const_int(mod, mod->mod->type_i64,
                                            (int64_t)nbytes, 64u);
        if (src && nbytes > 0) {
            lc_create_memcpy(mod, b, f, ptr, src, sz);
            return;
        }
    }
    memset(&inst, 0, sizeof(inst));
    ops[0] = lc_value_to_desc(val);
    ops[1] = lc_value_to_desc(ptr);
    inst.op = LR_OP_STORE;
    inst.type = mod->mod->type_void;
    inst.operands = ops;
    inst.num_operands = 2;
    (void)compat_emit(mod, b, b->func, &inst);
}

lc_value_t *lc_create_gep(lc_module_compat_t *mod, lr_block_t *b,
                           lr_func_t *f, lr_type_t *base_type,
                           lc_value_t *base_ptr, lc_value_t **indices,
                           unsigned num_indices, const char *name) {
    lr_inst_desc_t inst;
    lr_operand_desc_t *ops;
    uint32_t nops = 1 + num_indices;
    uint32_t dest;
    (void)name;
    if (!mod || !b || !f) return safe_undef(mod);
    ops = (lr_operand_desc_t *)calloc(nops, sizeof(*ops));
    if (!ops)
        return safe_undef(mod);
    memset(&inst, 0, sizeof(inst));
    ops[0] = lc_value_to_desc(base_ptr);
    for (unsigned i = 0; i < num_indices; i++) {
        ops[1 + i] = lc_value_to_desc(indices ? indices[i] : NULL);
    }
    inst.op = LR_OP_GEP;
    inst.type = base_type;
    inst.operands = ops;
    inst.num_operands = nops;
    dest = compat_emit(mod, b, f, &inst);
    free(ops);
    return wrap_vreg(mod, dest, mod->mod->type_ptr, f);
}

lc_value_t *lc_create_inbounds_gep(lc_module_compat_t *mod, lr_block_t *b,
                                    lr_func_t *f, lr_type_t *base_type,
                                    lc_value_t *base_ptr,
                                    lc_value_t **indices,
                                    unsigned num_indices, const char *name) {
    return lc_create_gep(mod, b, f, base_type, base_ptr,
                         indices, num_indices, name);
}

lc_value_t *lc_create_struct_gep(lc_module_compat_t *mod, lr_block_t *b,
                                  lr_func_t *f, lr_type_t *base_type,
                                  lc_value_t *base_ptr, unsigned idx,
                                  const char *name) {
    lc_value_t *zero = lc_value_const_int(mod, mod->mod->type_i32, 0, 32);
    lc_value_t *field = lc_value_const_int(mod, mod->mod->type_i32,
                                            (int64_t)idx, 32);
    lc_value_t *indices[2] = { zero, field };
    return lc_create_gep(mod, b, f, base_type, base_ptr, indices, 2, name);
}

/* ---- Control flow ---- */

void lc_create_ret(lc_module_compat_t *mod, lr_block_t *b,
                   lc_value_t *val) {
    lr_inst_desc_t inst;
    lr_operand_desc_t ops[1];
    if (!mod || !b || !val) return;
    memset(&inst, 0, sizeof(inst));
    ops[0] = lc_value_to_desc(val);
    inst.op = LR_OP_RET;
    inst.type = val->type;
    inst.operands = ops;
    inst.num_operands = 1;
    (void)compat_emit(mod, b, b->func, &inst);
}

void lc_create_ret_void(lc_module_compat_t *mod, lr_block_t *b) {
    lr_inst_desc_t inst;
    if (!mod || !b) return;
    memset(&inst, 0, sizeof(inst));
    inst.op = LR_OP_RET_VOID;
    inst.type = mod->mod->type_void;
    (void)compat_emit(mod, b, b->func, &inst);
}

void lc_create_br(lc_module_compat_t *mod, lr_block_t *b,
                  lr_block_t *target) {
    lr_inst_desc_t inst;
    lr_operand_desc_t ops[1];
    if (!mod || !b || !target) return;
    memset(&inst, 0, sizeof(inst));
    ops[0] = block_operand_desc(target);
    inst.op = LR_OP_BR;
    inst.type = mod->mod->type_void;
    inst.operands = ops;
    inst.num_operands = 1;
    (void)compat_emit(mod, b, b->func, &inst);
}

void lc_create_cond_br(lc_module_compat_t *mod, lr_block_t *b,
                       lc_value_t *cond, lr_block_t *true_bb,
                       lr_block_t *false_bb) {
    lr_inst_desc_t inst;
    lr_operand_desc_t ops[3];
    if (!mod || !b || !cond || !true_bb || !false_bb) return;
    memset(&inst, 0, sizeof(inst));
    ops[0] = lc_value_to_desc(cond);
    ops[1] = block_operand_desc(true_bb);
    ops[2] = block_operand_desc(false_bb);
    inst.op = LR_OP_CONDBR;
    inst.type = mod->mod->type_void;
    inst.operands = ops;
    inst.num_operands = 3;
    (void)compat_emit(mod, b, b->func, &inst);
}

void lc_create_unreachable(lc_module_compat_t *mod, lr_block_t *b) {
    lr_inst_desc_t inst;
    if (!mod || !b) return;
    memset(&inst, 0, sizeof(inst));
    inst.op = LR_OP_UNREACHABLE;
    inst.type = mod->mod->type_void;
    (void)compat_emit(mod, b, b->func, &inst);
}

/* ---- Calls ---- */

lc_value_t *lc_create_call(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lr_type_t *func_type,
                            lc_value_t *callee, lc_value_t **args,
                            unsigned num_args, const char *name) {
    lr_inst_desc_t inst;
    lr_operand_desc_t *ops;
    uint32_t nops = 1 + num_args;
    uint32_t dest;
    (void)name;
    if (!mod || !b || !f || !func_type) return safe_undef(mod);
    lr_type_t *ret_type = func_type->func.ret;
    if (!ret_type) ret_type = mod->mod->type_void;
    bool is_void = ret_type->kind == LR_TYPE_VOID;

    ops = (lr_operand_desc_t *)calloc(nops, sizeof(*ops));
    if (!ops)
        return safe_undef(mod);
    memset(&inst, 0, sizeof(inst));
    ops[0] = lc_value_to_desc(callee);
    for (unsigned i = 0; i < num_args; i++) {
        ops[1 + i] = lc_value_to_desc(args ? args[i] : NULL);
    }
    inst.op = LR_OP_CALL;
    inst.type = is_void ? mod->mod->type_void : ret_type;
    inst.operands = ops;
    inst.num_operands = nops;
    if (func_type && func_type->kind == LR_TYPE_FUNC) {
        inst.call_vararg = func_type->func.vararg;
        inst.call_fixed_args = func_type->func.num_params;
    }

    /* Indirect calls (e.g. bitcasted function values) should use C ABI. */
    if (ops[0].kind != LR_OP_KIND_GLOBAL)
        inst.call_external_abi = true;

    dest = compat_emit(mod, b, f, &inst);
    free(ops);
    if (is_void)
        return NULL;
    return wrap_vreg(mod, dest, ret_type, f);
}

/* ---- PHI ---- */

static lr_inst_t *find_phi_inst(lr_block_t *block, uint32_t dest_vreg) {
    lr_inst_t *it;
    if (!block)
        return NULL;
    for (it = block->first; it; it = it->next) {
        if (it->op == LR_OP_PHI && it->dest == dest_vreg)
            return it;
    }
    return NULL;
}

static void phi_refresh_operands(lc_phi_node_t *phi) {
    uint32_t n;
    lr_operand_t *ops;
    if (!phi || !phi->mod || !phi->inst || !phi->incoming_vals ||
        !phi->incoming_block_ids)
        return;
    n = phi->num_incoming;
    if (n == 0) {
        phi->inst->operands = NULL;
        phi->inst->num_operands = 0;
        return;
    }
    ops = lr_arena_array(phi->mod->arena, lr_operand_t, n * 2u);
    if (!ops)
        return;
    for (uint32_t i = 0; i < n; i++) {
        lr_operand_desc_t src = lc_value_to_desc(phi->incoming_vals[i]);
        lr_operand_desc_t pred;
        memset(&pred, 0, sizeof(pred));
        pred.kind = LR_OP_KIND_BLOCK;
        pred.block_id = phi->incoming_block_ids[i];
        ops[i * 2u] = compat_desc_to_operand(&src);
        ops[i * 2u + 1u] = compat_desc_to_operand(&pred);
    }
    phi->inst->operands = ops;
    phi->inst->num_operands = n * 2u;
}

lc_phi_node_t *lc_create_phi(lc_module_compat_t *mod, lr_block_t *b,
                              lr_func_t *f, lr_type_t *ty,
                              const char *name) {
    lr_inst_desc_t inst;
    uint32_t dest_vreg;
    lc_phi_node_t *phi;
    (void)name;
    if (!mod || !b || !f || !ty)
        return NULL;
    memset(&inst, 0, sizeof(inst));
    inst.op = LR_OP_PHI;
    inst.type = ty;
    dest_vreg = compat_emit(mod, b, f, &inst);

    phi = (lc_phi_node_t *)calloc(1, sizeof(lc_phi_node_t));
    if (!phi)
        return NULL;
    phi->result = wrap_vreg(mod, dest_vreg, ty, f);
    phi->result->vreg.phi_node = phi;
    phi->inst = find_phi_inst(b, dest_vreg);
    phi->type = ty;
    phi->block = b;
    phi->func = f;
    phi->mod = mod->mod;
    phi->session = mod->session;
    phi->cap_incoming = LC_INITIAL_PHI_CAP;
    phi->incoming_vals = (lc_value_t **)calloc(
        phi->cap_incoming, sizeof(lc_value_t *));
    phi->incoming_block_ids = (uint32_t *)calloc(
        phi->cap_incoming, sizeof(uint32_t));
    if (!phi->incoming_vals || !phi->incoming_block_ids) {
        free(phi->incoming_vals);
        free(phi->incoming_block_ids);
        free(phi);
        return NULL;
    }
    return phi;
}

void lc_phi_add_incoming(lc_phi_node_t *phi, lc_value_t *val,
                         lr_block_t *block) {
    lr_phi_copy_desc_t copy;
    lr_error_t err;
    if (!phi || !block || phi->finalized) return;
    if (!phi->incoming_vals || !phi->incoming_block_ids ||
        phi->cap_incoming == 0) return;
    if (phi->num_incoming == phi->cap_incoming) {
        uint32_t new_cap = phi->cap_incoming * 2;
        phi->incoming_vals = (lc_value_t **)realloc(
            phi->incoming_vals, sizeof(lc_value_t *) * new_cap);
        phi->incoming_block_ids = (uint32_t *)realloc(
            phi->incoming_block_ids, sizeof(uint32_t) * new_cap);
        phi->cap_incoming = new_cap;
    }
    phi->incoming_vals[phi->num_incoming] = val;
    phi->incoming_block_ids[phi->num_incoming] = block->id;
    phi->num_incoming++;
    memset(&copy, 0, sizeof(copy));
    copy.dest_vreg = phi->result ? phi->result->vreg.id : 0;
    copy.src_op = lc_value_to_desc(val);
    memset(&err, 0, sizeof(err));
    if (phi->session) {
        if (lr_session_is_direct(phi->session)) {
            (void)lr_session_add_phi_copy(phi->session, block->id,
                                          &copy, &err);
        } else if (lr_session_bind_ir(phi->session, phi->mod,
                                      phi->func, phi->block,
                                      &err) == 0) {
            (void)lr_session_add_phi_copy(phi->session, block->id,
                                          &copy, &err);
        }
    }
    /* Keep PHI instruction operands in sync whenever an IR PHI exists.
       DIRECT+llvm builds IR and lowers later through LLVM text, so it also
       requires concrete incoming operand lists even though policy is DIRECT. */
    phi_refresh_operands(phi);
}

void lc_phi_finalize(lc_phi_node_t *phi) {
    if (!phi) return;
    phi->finalized = true;
}

/* ---- Select ---- */

lc_value_t *lc_create_select(lc_module_compat_t *mod, lr_block_t *b,
                              lr_func_t *f, lc_value_t *cond,
                              lc_value_t *true_val, lc_value_t *false_val,
                              const char *name) {
    lr_inst_desc_t inst;
    lr_operand_desc_t ops[3];
    uint32_t dest;
    (void)name;
    if (!mod || !b || !f || !true_val) return safe_undef(mod);
    lr_type_t *ty = true_val->type ? true_val->type : mod->mod->type_i64;
    memset(&inst, 0, sizeof(inst));
    ops[0] = lc_value_to_desc(cond);
    ops[1] = lc_value_to_desc(true_val);
    ops[2] = lc_value_to_desc(false_val);
    inst.op = LR_OP_SELECT;
    inst.type = ty;
    inst.operands = ops;
    inst.num_operands = 3;
    dest = compat_emit(mod, b, f, &inst);
    return wrap_vreg(mod, dest, ty, f);
}

/* ---- Type conversions ---- */

lc_value_t *lc_create_sext(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *val,
                            lr_type_t *to_type, const char *name) {
    (void)name;
    return compat_cast(mod, b, f, LR_OP_SEXT, val, to_type);
}

lc_value_t *lc_create_zext(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *val,
                            lr_type_t *to_type, const char *name) {
    (void)name;
    return compat_cast(mod, b, f, LR_OP_ZEXT, val, to_type);
}

lc_value_t *lc_create_trunc(lc_module_compat_t *mod, lr_block_t *b,
                             lr_func_t *f, lc_value_t *val,
                             lr_type_t *to_type, const char *name) {
    (void)name;
    return compat_cast(mod, b, f, LR_OP_TRUNC, val, to_type);
}

lc_value_t *lc_create_bitcast(lc_module_compat_t *mod, lr_block_t *b,
                               lr_func_t *f, lc_value_t *val,
                               lr_type_t *to_type, const char *name) {
    (void)name;
    return compat_cast(mod, b, f, LR_OP_BITCAST, val, to_type);
}

lc_value_t *lc_create_ptrtoint(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *val,
                                lr_type_t *to_type, const char *name) {
    (void)name;
    return compat_cast(mod, b, f, LR_OP_PTRTOINT, val, to_type);
}

lc_value_t *lc_create_inttoptr(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *val,
                                lr_type_t *to_type, const char *name) {
    (void)name;
    return compat_cast(mod, b, f, LR_OP_INTTOPTR, val, to_type);
}

lc_value_t *lc_create_sitofp(lc_module_compat_t *mod, lr_block_t *b,
                              lr_func_t *f, lc_value_t *val,
                              lr_type_t *to_type, const char *name) {
    (void)name;
    return compat_cast(mod, b, f, LR_OP_SITOFP, val, to_type);
}

lc_value_t *lc_create_uitofp(lc_module_compat_t *mod, lr_block_t *b,
                              lr_func_t *f, lc_value_t *val,
                              lr_type_t *to_type, const char *name) {
    (void)name;
    return compat_cast(mod, b, f, LR_OP_UITOFP, val, to_type);
}

lc_value_t *lc_create_fptosi(lc_module_compat_t *mod, lr_block_t *b,
                              lr_func_t *f, lc_value_t *val,
                              lr_type_t *to_type, const char *name) {
    (void)name;
    return compat_cast(mod, b, f, LR_OP_FPTOSI, val, to_type);
}

lc_value_t *lc_create_fptoui(lc_module_compat_t *mod, lr_block_t *b,
                              lr_func_t *f, lc_value_t *val,
                              lr_type_t *to_type, const char *name) {
    (void)name;
    return compat_cast(mod, b, f, LR_OP_FPTOUI, val, to_type);
}

lc_value_t *lc_create_fpext(lc_module_compat_t *mod, lr_block_t *b,
                             lr_func_t *f, lc_value_t *val,
                             lr_type_t *to_type, const char *name) {
    (void)name;
    return compat_cast(mod, b, f, LR_OP_FPEXT, val, to_type);
}

lc_value_t *lc_create_fptrunc(lc_module_compat_t *mod, lr_block_t *b,
                               lr_func_t *f, lc_value_t *val,
                               lr_type_t *to_type, const char *name) {
    (void)name;
    return compat_cast(mod, b, f, LR_OP_FPTRUNC, val, to_type);
}

lc_value_t *lc_create_sext_or_trunc(lc_module_compat_t *mod, lr_block_t *b,
                                     lr_func_t *f, lc_value_t *val,
                                     lr_type_t *to_type, const char *name) {
    if (!val) return safe_undef(mod);
    unsigned src_w = lc_type_int_width(val->type);
    unsigned dst_w = lc_type_int_width(to_type);
    if (src_w == dst_w) return val;
    if (src_w < dst_w) return lc_create_sext(mod, b, f, val, to_type, name);
    return lc_create_trunc(mod, b, f, val, to_type, name);
}

lc_value_t *lc_create_zext_or_trunc(lc_module_compat_t *mod, lr_block_t *b,
                                     lr_func_t *f, lc_value_t *val,
                                     lr_type_t *to_type, const char *name) {
    if (!val) return safe_undef(mod);
    unsigned src_w = lc_type_int_width(val->type);
    unsigned dst_w = lc_type_int_width(to_type);
    if (src_w == dst_w) return val;
    if (src_w < dst_w) return lc_create_zext(mod, b, f, val, to_type, name);
    return lc_create_trunc(mod, b, f, val, to_type, name);
}

/* ---- Aggregate ---- */

lc_value_t *lc_create_extractvalue(lc_module_compat_t *mod, lr_block_t *b,
                                    lr_func_t *f, lc_value_t *agg,
                                    unsigned *indices, unsigned num_indices,
                                    const char *name) {
    lr_inst_desc_t inst;
    lr_operand_desc_t ops[1];
    uint32_t dest;
    (void)name;
    if (!mod || !b || !f || !agg) return safe_undef(mod);
    lr_module_t *m = mod->mod;

    lr_type_t *result_ty = agg->type;
    if (!result_ty) result_ty = m->type_i64;
    for (unsigned i = 0; i < num_indices; i++) {
        if (result_ty->kind == LR_TYPE_STRUCT) {
            result_ty = result_ty->struc.fields[indices[i]];
        } else if (result_ty->kind == LR_TYPE_ARRAY ||
                   result_ty->kind == LR_TYPE_VECTOR) {
            result_ty = result_ty->array.elem;
        }
    }

    if (agg->kind == LC_VAL_CONST_AGGREGATE) {
        lc_value_t *folded = const_extractvalue_fold(mod, agg, indices, num_indices);
        if (folded)
            return folded;
    }

    memset(&inst, 0, sizeof(inst));
    ops[0] = lc_value_to_desc(agg);
    inst.op = LR_OP_EXTRACTVALUE;
    inst.type = result_ty;
    inst.operands = ops;
    inst.num_operands = 1;
    inst.indices = indices;
    inst.num_indices = num_indices;
    dest = compat_emit(mod, b, f, &inst);
    return wrap_vreg(mod, dest, result_ty, f);
}

lc_value_t *lc_create_insertvalue(lc_module_compat_t *mod, lr_block_t *b,
                                   lr_func_t *f, lc_value_t *agg,
                                   lc_value_t *val, unsigned *indices,
                                   unsigned num_indices, const char *name) {
    lr_inst_desc_t inst;
    lr_operand_desc_t ops[2];
    uint32_t dest;
    (void)name;
    if (!mod || !b || !f || !agg) return safe_undef(mod);
    memset(&inst, 0, sizeof(inst));
    ops[0] = lc_value_to_desc(agg);
    ops[1] = lc_value_to_desc(val);
    inst.op = LR_OP_INSERTVALUE;
    inst.type = agg->type;
    inst.operands = ops;
    inst.num_operands = 2;
    inst.indices = indices;
    inst.num_indices = num_indices;
    dest = compat_emit(mod, b, f, &inst);
    return wrap_vreg(mod, dest, agg->type, f);
}

/* ---- MemCpy / MemSet ---- */

static lr_func_t *ensure_libc_decl(lc_module_compat_t *mod, const char *name,
                                   lr_type_t *ret, lr_type_t **params,
                                   uint32_t num_params) {
    uint32_t sym_id = UINT32_MAX;
    lr_func_t *f = lookup_func_cached(mod, name, &sym_id);
    if (f)
        return f;

    lr_func_t *decl = lr_func_declare(mod->mod, name, ret, params, num_params, false);
    if (!decl)
        return NULL;
    if (sym_id == UINT32_MAX)
        sym_id = compat_symbol_id(mod, name);
    cache_func_by_symbol(mod, sym_id, decl);
    return decl;
}

void lc_create_memcpy(lc_module_compat_t *mod, lr_block_t *b, lr_func_t *f,
                      lc_value_t *dst, lc_value_t *src, lc_value_t *size) {
    lr_inst_desc_t inst;
    lr_operand_desc_t ops[4];
    lc_value_t *size_arg;
    if (!mod || !b || !f) return;
    lr_module_t *m = mod->mod;
    lr_type_t *params[3] = { m->type_ptr, m->type_ptr, m->type_i64 };
    lr_func_t *decl = ensure_libc_decl(mod, "memcpy", m->type_ptr, params, 3);
    uint32_t sym_id = compat_symbol_id(mod, "memcpy");
    if (!decl || sym_id == UINT32_MAX) return;

    (void)decl;
    size_arg = size;
    if (size_arg && size_arg->type && size_arg->type != m->type_i64)
        size_arg = lc_create_zext_or_trunc(mod, b, f, size_arg, m->type_i64,
                                           "memcpy_size");

    memset(&inst, 0, sizeof(inst));
    ops[0] = global_operand_desc(sym_id, m->type_ptr);
    ops[1] = lc_value_to_desc(dst);
    ops[2] = lc_value_to_desc(src);
    ops[3] = lc_value_to_desc(size_arg);
    inst.op = LR_OP_CALL;
    inst.type = m->type_ptr;
    inst.operands = ops;
    inst.num_operands = 4;
    inst.call_fixed_args = 3;
    (void)compat_emit(mod, b, f, &inst);
}

void lc_create_memset(lc_module_compat_t *mod, lr_block_t *b, lr_func_t *f,
                      lc_value_t *dst, lc_value_t *val, lc_value_t *size) {
    lr_inst_desc_t inst;
    lr_operand_desc_t ops[4];
    lc_value_t *val_arg;
    lc_value_t *size_arg;
    if (!mod || !b || !f) return;
    lr_module_t *m = mod->mod;
    lr_type_t *params[3] = { m->type_ptr, m->type_i32, m->type_i64 };
    lr_func_t *decl = ensure_libc_decl(mod, "memset", m->type_ptr, params, 3);
    uint32_t sym_id = compat_symbol_id(mod, "memset");
    if (!decl || sym_id == UINT32_MAX) return;

    (void)decl;
    val_arg = val;
    if (val_arg && val_arg->type && val_arg->type != m->type_i32)
        val_arg = lc_create_zext_or_trunc(mod, b, f, val_arg, m->type_i32,
                                          "memset_val");
    size_arg = size;
    if (size_arg && size_arg->type && size_arg->type != m->type_i64)
        size_arg = lc_create_zext_or_trunc(mod, b, f, size_arg, m->type_i64,
                                           "memset_size");

    memset(&inst, 0, sizeof(inst));
    ops[0] = global_operand_desc(sym_id, m->type_ptr);
    ops[1] = lc_value_to_desc(dst);
    ops[2] = lc_value_to_desc(val_arg);
    ops[3] = lc_value_to_desc(size_arg);
    inst.op = LR_OP_CALL;
    inst.type = m->type_ptr;
    inst.operands = ops;
    inst.num_operands = 4;
    inst.call_fixed_args = 3;
    (void)compat_emit(mod, b, f, &inst);
}

void lc_create_memmove(lc_module_compat_t *mod, lr_block_t *b, lr_func_t *f,
                       lc_value_t *dst, lc_value_t *src, lc_value_t *size) {
    lr_inst_desc_t inst;
    lr_operand_desc_t ops[4];
    lc_value_t *size_arg;
    if (!mod || !b || !f) return;
    lr_module_t *m = mod->mod;
    lr_type_t *params[3] = { m->type_ptr, m->type_ptr, m->type_i64 };
    lr_func_t *decl = ensure_libc_decl(mod, "memmove", m->type_ptr, params, 3);
    uint32_t sym_id = compat_symbol_id(mod, "memmove");
    if (!decl || sym_id == UINT32_MAX) return;
    (void)decl;
    size_arg = size;
    if (size_arg && size_arg->type && size_arg->type != m->type_i64)
        size_arg = lc_create_zext_or_trunc(mod, b, f, size_arg, m->type_i64,
                                           "memmove_size");

    memset(&inst, 0, sizeof(inst));
    ops[0] = global_operand_desc(sym_id, m->type_ptr);
    ops[1] = lc_value_to_desc(dst);
    ops[2] = lc_value_to_desc(src);
    ops[3] = lc_value_to_desc(size_arg);
    inst.op = LR_OP_CALL;
    inst.type = m->type_ptr;
    inst.operands = ops;
    inst.num_operands = 4;
    inst.call_fixed_args = 3;
    (void)compat_emit(mod, b, f, &inst);
}

static int compat_add_to_jit_direct(lc_module_compat_t *mod, lr_jit_t *jit) {
    lr_jit_t *session_jit;
    lr_func_t *f;
    lr_global_t *g;

    if (compat_finish_active_func(mod) != 0)
        return -1;

    session_jit = lr_session_jit(mod->session);
    if (!session_jit)
        return -1;

    if (lr_jit_materialize_globals(session_jit, mod->mod) != 0)
        return -1;

    /* LLVM mode: compile mod->mod through the session JIT (which has LLVM
       mode), then register symbols into the user-provided JIT below. */
    if (session_jit->mode == LR_COMPILE_LLVM) {
        if (!lr_llvm_jit_is_available())
            return -1;
        if (lr_jit_add_module(session_jit, mod->mod) != 0)
            return -1;
    }

    for (f = mod->mod->first_func; f; f = f->next) {
        void *addr = NULL;
        if (!f->name || !f->name[0])
            continue;
        addr = lr_session_lookup(mod->session, f->name);
        if (session_jit->mode == LR_COMPILE_LLVM && !addr)
            addr = lr_jit_get_function(session_jit, f->name);
        if (!f->is_decl && !addr)
            return -1;
        if (addr)
            lr_jit_add_symbol(jit, f->name, addr);
    }

    for (g = mod->mod->first_global; g; g = g->next) {
        void *addr = NULL;
        if (!g->name || !g->name[0])
            continue;
        addr = lr_session_lookup(mod->session, g->name);
        if (session_jit->mode == LR_COMPILE_LLVM && !addr)
            addr = lr_jit_get_function(session_jit, g->name);
        if (!g->is_external && !addr)
            return -1;
        if (addr)
            lr_jit_add_symbol(jit, g->name, addr);
    }

    return 0;
}

int lc_module_add_to_jit(lc_module_compat_t *mod, lr_jit_t *jit) {
    if (!mod || !jit) return -1;
    {
        const char *dump_path = getenv("LIRIC_COMPAT_DUMP_FINAL_IR");
        if (dump_path && dump_path[0]) {
            FILE *f = fopen(dump_path, "a");
            if (f) {
                fprintf(f, "; ---- module ----\n");
                compat_dump_module(mod->mod, f);
                fprintf(f, "\n");
                fclose(f);
            }
        }
    }
    if (lr_session_is_direct(mod->session))
        return compat_add_to_jit_direct(mod, jit);
    return lr_jit_add_module(jit, mod->mod);
}

int lc_module_finalize_for_execution(lc_module_compat_t *mod) {
    if (!mod)
        return -1;
    return compat_finish_active_func(mod);
}

void *lc_module_lookup_in_session(lc_module_compat_t *mod, const char *name) {
    if (!mod || !name || !name[0])
        return NULL;
    if (compat_finish_active_func(mod) != 0)
        return NULL;
    return lr_session_lookup(mod->session, name);
}

void lc_module_add_external_symbol(lc_module_compat_t *mod, const char *name,
                                   void *addr) {
    if (!mod || !name || !name[0])
        return;
    lr_session_add_symbol(mod->session, name, addr);
}

int lc_module_emit_object_to_file(lc_module_compat_t *mod, FILE *out) {
    lr_error_t err = {0};
    if (!mod || !out) return -1;
    if (compat_finish_active_func(mod) != 0) return -1;
    if (mod->session) {
        int rc = lr_session_emit_object_stream(mod->session, out, &err);
        if (rc != 0 && err.msg[0])
            fprintf(stderr, "lc_module_emit_object_to_file: %s\n", err.msg);
        return rc;
    }
    char emit_err[256] = {0};
    return lr_emit_module_object_stream(mod->mod, NULL, out,
                                        emit_err, sizeof(emit_err));
}

int lc_module_emit_object(lc_module_compat_t *mod, const char *filename) {
    lr_error_t err = {0};
    if (!mod || !filename) return -1;
    if (compat_finish_active_func(mod) != 0) return -1;
    if (lr_session_emit_object(mod->session, filename, &err) != 0) {
        if (err.msg[0] && getenv("LIRIC_COMPAT_VERBOSE_ERRORS"))
            fprintf(stderr, "lc_module_emit_object: %s\n", err.msg);
        return -1;
    }
    return 0;
}

int lc_module_emit_executable(lc_module_compat_t *mod, const char *filename,
                               const char *runtime_ll, size_t runtime_len) {
    lr_error_t err = {0};
    if (!mod || !filename || !runtime_ll || runtime_len == 0) return -1;
    if (compat_finish_active_func(mod) != 0) return -1;
    if (lr_session_emit_exe_with_runtime(mod->session, filename,
                                         runtime_ll, runtime_len,
                                         &err) != 0) {
        if (err.msg[0] && getenv("LIRIC_COMPAT_VERBOSE_ERRORS"))
            fprintf(stderr, "lc_module_emit_executable: %s\n", err.msg);
        return -1;
    }
    return 0;
}
