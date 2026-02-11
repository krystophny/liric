#include "ir.h"
#include "arena.h"
#include "jit.h"
#include "objfile.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Replicate the public operand descriptor type and kind constants.
   We cannot include liric.h alongside ir.h due to enum redeclarations. */
typedef struct lr_operand_desc {
    int kind;
    union {
        uint32_t vreg;
        int64_t imm_i64;
        double imm_f64;
        uint32_t block_id;
        uint32_t global_id;
    };
    lr_type_t *type;
} lr_operand_desc_t;

enum {
    LR_OP_KIND_VREG    = 0,
    LR_OP_KIND_IMM_I64 = 1,
    LR_OP_KIND_IMM_F64 = 2,
    LR_OP_KIND_BLOCK   = 3,
    LR_OP_KIND_GLOBAL  = 4,
    LR_OP_KIND_NULL    = 5,
    LR_OP_KIND_UNDEF   = 6,
};

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

typedef struct lc_value {
    lc_value_kind_t kind;
    lr_type_t *type;
    union {
        struct { uint32_t id; lr_func_t *func; void *phi_node; lr_type_t *alloc_type; } vreg;
        struct { int64_t val; unsigned width; } const_int;
        struct { double val; bool is_double; } const_fp;
        struct { uint32_t id; const char *name; lr_func_t *func; } global;
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
} lc_context_t;

typedef struct lc_phi_node lc_phi_node_t;

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
    lc_phi_node_t **pending_phis;
    uint32_t phi_count;
    uint32_t phi_cap;
    lr_module_t *cache_owner_mod;
    lr_func_t **func_by_sym;
    lr_global_t **global_by_sym;
    uint32_t sym_cache_cap;
} lc_module_compat_t;

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
};

typedef struct lc_alloca_inst {
    lc_value_t *result;
    lr_type_t *alloc_type;
} lc_alloca_inst_t;

/* ---- Internal desc_to_op (same as builder.c) ---- */

/* Forward declaration for safe_undef */
lc_value_t *lc_value_undef(lc_module_compat_t *mod, lr_type_t *type);

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

/* ---- lc_value_to_desc ---- */

lr_operand_desc_t lc_value_to_desc(lc_value_t *val) {
    lr_operand_desc_t d;
    d.kind = LR_OP_KIND_NULL;
    d.type = NULL;
    d.imm_i64 = 0;
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

static inline lr_operand_t lc_value_to_op_fast(lc_value_t *val) {
    lr_operand_t op;
    op.kind = LR_VAL_NULL;
    op.type = NULL;
    op.global_offset = 0;
    op.imm_i64 = 0;
    if (!val)
        return op;

    op.type = val->type;
    switch (val->kind) {
    case LC_VAL_VREG:
        op.kind = LR_VAL_VREG;
        op.vreg = val->vreg.id;
        break;
    case LC_VAL_CONST_INT:
        op.kind = LR_VAL_IMM_I64;
        op.imm_i64 = val->const_int.val;
        break;
    case LC_VAL_CONST_FP:
        op.kind = LR_VAL_IMM_F64;
        op.imm_f64 = val->const_fp.val;
        break;
    case LC_VAL_CONST_NULL:
        op.kind = LR_VAL_NULL;
        break;
    case LC_VAL_CONST_UNDEF:
        op.kind = LR_VAL_UNDEF;
        break;
    case LC_VAL_GLOBAL:
        op.kind = LR_VAL_GLOBAL;
        op.global_id = val->global.id;
        break;
    case LC_VAL_ARGUMENT:
        if (val->argument.func && val->argument.func->param_vregs
                && val->argument.param_idx < val->argument.func->num_params) {
            op.kind = LR_VAL_VREG;
            op.vreg = val->argument.func->param_vregs[val->argument.param_idx];
        } else {
            op.kind = LR_VAL_IMM_I64;
            op.imm_i64 = 0;
        }
        break;
    case LC_VAL_BLOCK:
        if (val->block.block) {
            op.kind = LR_VAL_BLOCK;
            op.block_id = val->block.block->id;
        } else {
            op.kind = LR_VAL_IMM_I64;
            op.imm_i64 = 0;
        }
        break;
    case LC_VAL_CONST_AGGREGATE:
        op.kind = LR_VAL_IMM_I64;
        op.imm_i64 = 0;
        break;
    }
    return op;
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
    return ctx;
}

void lc_context_destroy(lc_context_t *ctx) {
    if (!ctx) return;
    if (ctx->type_arena) lr_arena_destroy(ctx->type_arena);
    free(ctx);
}

/* ---- Module ---- */

lc_module_compat_t *lc_module_create(lc_context_t *ctx, const char *name) {
    lc_module_compat_t *cm = (lc_module_compat_t *)calloc(
        1, sizeof(lc_module_compat_t));
    if (!cm) return NULL;

    lr_arena_t *arena = lr_arena_create(0);
    if (!arena) { free(cm); return NULL; }

    cm->mod = lr_module_create(arena);
    if (!cm->mod) { lr_arena_destroy(arena); free(cm); return NULL; }
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
    value_pool_free(mod);
    free(mod->func_values);
    free(mod->func_by_sym);
    free(mod->global_by_sym);
    free((void *)mod->name);
    free(mod);
}

lr_module_t *lc_module_get_ir(lc_module_compat_t *mod) {
    return mod->mod;
}

void lc_module_dump(lc_module_compat_t *mod) {
    lr_module_dump(mod->mod, stderr);
}

void lc_module_print(lc_module_compat_t *mod, FILE *out) {
    lr_module_dump(mod->mod, out ? out : stdout);
}

char *lc_module_sprint(lc_module_compat_t *mod, size_t *out_len) {
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f) return NULL;
    lr_module_dump(mod->mod, f);
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
    v->aggregate.data = data;
    v->aggregate.size = size;
    return v;
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

lc_value_t *lc_global_lookup_or_create(lc_module_compat_t *mod,
                                        const char *name, lr_type_t *type) {
    if (!mod || !name) return safe_undef(mod);
    lr_module_t *m = mod->mod;
    uint32_t sym_id = UINT32_MAX;
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

lc_value_t *lc_global_create(lc_module_compat_t *mod, const char *name,
                              lr_type_t *type, bool is_const,
                              const void *init_data, size_t init_size) {
    const char *actual_name = name;
    char auto_name[32];
    if (!name || name[0] == '\0') {
        snprintf(auto_name, sizeof(auto_name), ".str.%u",
                 mod->mod->num_globals);
        actual_name = auto_name;
    }
    lr_global_t *g = lr_global_create(mod->mod, actual_name, type, is_const);
    if (init_data && init_size > 0) {
        g->init_data = lr_arena_alloc_uninit(mod->mod->arena, init_size, 1);
        memcpy(g->init_data, init_data, init_size);
        g->init_size = init_size;
    }
    uint32_t sym_id = compat_symbol_id(mod, g->name);
    if (sym_id == UINT32_MAX)
        return safe_undef(mod);
    cache_global_by_symbol(mod, sym_id, g);
    return lc_value_global(mod, sym_id, mod->mod->type_ptr, g->name);
}

lc_value_t *lc_global_declare(lc_module_compat_t *mod, const char *name,
                               lr_type_t *type) {
    lr_global_t *g = lr_global_create(mod->mod, name, type, false);
    g->is_external = true;
    uint32_t sym_id = compat_symbol_id(mod, name);
    if (sym_id == UINT32_MAX)
        return safe_undef(mod);
    cache_global_by_symbol(mod, sym_id, g);
    return lc_value_global(mod, sym_id, mod->mod->type_ptr, g->name);
}

lr_global_t *lc_value_get_global(lc_value_t *val) {
    (void)val;
    return NULL;
}

/* ---- Internal builder helpers ---- */

static lc_value_t *wrap_vreg(lc_module_compat_t *mod, uint32_t vreg_id,
                              lr_type_t *type, lr_func_t *func) {
    return lc_value_vreg(mod, vreg_id, type, func);
}

static uint32_t build_binop(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                              lr_opcode_t op, lr_type_t *ty,
                              lr_operand_t lhs, lr_operand_t rhs) {
    uint32_t dest = lr_vreg_new(f);
    lr_operand_t ops[2] = { lhs, rhs };
    lr_inst_t *inst = lr_inst_create(m->arena, op, ty, dest, ops, 2);
    lr_block_append(b, inst);
    return dest;
}

static uint32_t build_cast(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                             lr_opcode_t op, lr_type_t *to_type,
                             lr_operand_t val) {
    uint32_t dest = lr_vreg_new(f);
    lr_operand_t ops[1] = { val };
    lr_inst_t *inst = lr_inst_create(m->arena, op, to_type, dest, ops, 1);
    lr_block_append(b, inst);
    return dest;
}

static lc_value_t *compat_binop(lc_module_compat_t *mod, lr_block_t *b,
                                  lr_func_t *f, lr_opcode_t op,
                                  lc_value_t *lhs, lc_value_t *rhs) {
    if (!mod || !b || !f || !lhs) return safe_undef(mod);
    lr_type_t *ty = lhs->type ? lhs->type : mod->mod->type_i64;
    uint32_t v = build_binop(mod->mod, b, f, op, ty,
                             lc_value_to_op_fast(lhs), lc_value_to_op_fast(rhs));
    return wrap_vreg(mod, v, ty, f);
}

static lc_value_t *compat_cast(lc_module_compat_t *mod, lr_block_t *b,
                                 lr_func_t *f, lr_opcode_t op,
                                 lc_value_t *val, lr_type_t *to_type) {
    if (!mod || !b || !f || !val) return safe_undef(mod);
    uint32_t v = build_cast(mod->mod, b, f, op, to_type,
                            lc_value_to_op_fast(val));
    return wrap_vreg(mod, v, to_type, f);
}

static lc_value_t *compat_icmp(lc_module_compat_t *mod, lr_block_t *b,
                                 lr_func_t *f, int pred,
                                 lc_value_t *lhs, lc_value_t *rhs) {
    if (!mod || !b || !f) return safe_undef(mod);
    lr_module_t *m = mod->mod;
    uint32_t dest = lr_vreg_new(f);
    lr_operand_t ops[2] = { lc_value_to_op_fast(lhs), lc_value_to_op_fast(rhs) };
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_ICMP, m->type_i1,
                                      dest, ops, 2);
    inst->icmp_pred = (lr_icmp_pred_t)pred;
    lr_block_append(b, inst);
    return wrap_vreg(mod, dest, m->type_i1, f);
}

static lc_value_t *compat_fcmp(lc_module_compat_t *mod, lr_block_t *b,
                                 lr_func_t *f, int pred,
                                 lc_value_t *lhs, lc_value_t *rhs) {
    if (!mod || !b || !f) return safe_undef(mod);
    lr_module_t *m = mod->mod;
    uint32_t dest = lr_vreg_new(f);
    lr_operand_t ops[2] = { lc_value_to_op_fast(lhs), lc_value_to_op_fast(rhs) };
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_FCMP, m->type_i1,
                                      dest, ops, 2);
    inst->fcmp_pred = (lr_fcmp_pred_t)pred;
    lr_block_append(b, inst);
    return wrap_vreg(mod, dest, m->type_i1, f);
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
    if (!val) return safe_undef(mod);
    lc_value_t *neg1 = lc_value_const_int(mod, val->type, -1,
                                            lc_type_int_width(val->type));
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
    (void)name;
    if (!mod || !b || !f || !type) {
        lc_alloca_inst_t *ai = (lc_alloca_inst_t *)calloc(1, sizeof(*ai));
        if (!ai) return NULL;
        ai->result = safe_undef(mod);
        ai->alloc_type = type;
        return ai;
    }
    lr_module_t *m = mod->mod;
    uint32_t dest = lr_vreg_new(f);

    if (array_size) {
        lr_operand_t ops[1] = { lc_value_to_op_fast(array_size) };
        lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_ALLOCA,
                                          m->type_ptr, dest, ops, 1);
        inst->type = type;
        lr_block_append(b, inst);
    } else {
        lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_ALLOCA,
                                          m->type_ptr, dest, NULL, 0);
        inst->type = type;
        lr_block_append(b, inst);
    }

    lc_alloca_inst_t *ai = (lc_alloca_inst_t *)malloc(sizeof(*ai));
    lc_value_t *result = wrap_vreg(mod, dest, m->type_ptr, f);
    result->vreg.alloc_type = type;
    ai->result = result;
    ai->alloc_type = type;
    return ai;
}

lc_value_t *lc_create_load(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lr_type_t *ty, lc_value_t *ptr,
                            const char *name) {
    (void)name;
    if (!mod || !b || !f) return safe_undef(mod);
    lr_module_t *m = mod->mod;
    uint32_t dest = lr_vreg_new(f);
    lr_operand_t ops[1] = { lc_value_to_op_fast(ptr) };
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_LOAD, ty,
                                      dest, ops, 1);
    lr_block_append(b, inst);
    return wrap_vreg(mod, dest, ty, f);
}

void lc_create_store(lc_module_compat_t *mod, lr_block_t *b,
                     lc_value_t *val, lc_value_t *ptr) {
    if (!mod || !b) return;
    lr_module_t *m = mod->mod;
    lr_operand_t ops[2] = { lc_value_to_op_fast(val), lc_value_to_op_fast(ptr) };
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_STORE,
                                      m->type_void, 0, ops, 2);
    lr_block_append(b, inst);
}

lc_value_t *lc_create_gep(lc_module_compat_t *mod, lr_block_t *b,
                           lr_func_t *f, lr_type_t *base_type,
                           lc_value_t *base_ptr, lc_value_t **indices,
                           unsigned num_indices, const char *name) {
    (void)name;
    if (!mod || !b || !f) return safe_undef(mod);
    lr_module_t *m = mod->mod;
    uint32_t dest = lr_vreg_new(f);
    uint32_t nops = 1 + num_indices;
    lr_operand_t *ops = lr_arena_array(m->arena, lr_operand_t, nops);
    ops[0] = lc_value_to_op_fast(base_ptr);
    for (unsigned i = 0; i < num_indices; i++) {
        lr_operand_t idx = lc_value_to_op_fast(indices[i]);
        ops[1 + i] = lr_canonicalize_gep_index(m, b, f, idx);
    }
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_GEP,
                                      m->type_ptr, dest, ops, nops);
    inst->type = base_type;
    lr_block_append(b, inst);
    return wrap_vreg(mod, dest, m->type_ptr, f);
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
    if (!mod || !b || !val) return;
    lr_module_t *m = mod->mod;
    lr_operand_t ops[1] = { lc_value_to_op_fast(val) };
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_RET,
                                      val->type, 0, ops, 1);
    lr_block_append(b, inst);
}

void lc_create_ret_void(lc_module_compat_t *mod, lr_block_t *b) {
    if (!mod || !b) return;
    lr_module_t *m = mod->mod;
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_RET_VOID,
                                      m->type_void, 0, NULL, 0);
    lr_block_append(b, inst);
}

void lc_create_br(lc_module_compat_t *mod, lr_block_t *b,
                  lr_block_t *target) {
    if (!mod || !b || !target) return;
    lr_module_t *m = mod->mod;
    lr_operand_t ops[1] = { lr_op_block(target->id) };
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_BR,
                                      m->type_void, 0, ops, 1);
    lr_block_append(b, inst);
}

void lc_create_cond_br(lc_module_compat_t *mod, lr_block_t *b,
                       lc_value_t *cond, lr_block_t *true_bb,
                       lr_block_t *false_bb) {
    if (!mod || !b || !cond || !true_bb || !false_bb) return;
    lr_module_t *m = mod->mod;
    lr_operand_t ops[3] = {
        lc_value_to_op_fast(cond),
        lr_op_block(true_bb->id),
        lr_op_block(false_bb->id)
    };
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_CONDBR,
                                      m->type_void, 0, ops, 3);
    lr_block_append(b, inst);
}

void lc_create_unreachable(lc_module_compat_t *mod, lr_block_t *b) {
    if (!mod || !b) return;
    lr_module_t *m = mod->mod;
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_UNREACHABLE,
                                      m->type_void, 0, NULL, 0);
    lr_block_append(b, inst);
}

/* ---- Calls ---- */

lc_value_t *lc_create_call(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lr_type_t *func_type,
                            lc_value_t *callee, lc_value_t **args,
                            unsigned num_args, const char *name) {
    (void)name;
    if (!mod || !b || !f || !func_type) return safe_undef(mod);
    lr_module_t *m = mod->mod;
    lr_type_t *ret_type = func_type->func.ret;
    if (!ret_type) ret_type = m->type_void;
    bool is_void = ret_type->kind == LR_TYPE_VOID;

    uint32_t nops = 1 + num_args;
    lr_operand_t *ops = lr_arena_array(m->arena, lr_operand_t, nops);
    ops[0] = lc_value_to_op_fast(callee);
    for (unsigned i = 0; i < num_args; i++) {
        ops[1 + i] = lc_value_to_op_fast(args[i]);
    }

    lr_inst_t *inst = NULL;
    uint32_t dest = 0;
    if (is_void) {
        inst = lr_inst_create(m->arena, LR_OP_CALL,
                               m->type_void, 0, ops, nops);
    } else {
        dest = lr_vreg_new(f);
        inst = lr_inst_create(m->arena, LR_OP_CALL,
                               ret_type, dest, ops, nops);
    }
    if (!inst)
        return safe_undef(mod);

    if (func_type && func_type->kind == LR_TYPE_FUNC)
        inst->call_vararg = func_type->func.vararg;

    /* Indirect calls (e.g. bitcasted function values) should use C ABI. */
    if (ops[0].kind != LR_VAL_GLOBAL)
        inst->call_external_abi = true;

    lr_block_append(b, inst);
    if (is_void)
        return NULL;
    return wrap_vreg(mod, dest, ret_type, f);
}

/* ---- PHI ---- */

lc_phi_node_t *lc_create_phi(lc_module_compat_t *mod, lr_block_t *b,
                              lr_func_t *f, lr_type_t *ty,
                              const char *name) {
    (void)name;
    uint32_t vreg_id = lr_vreg_new(f);

    lc_phi_node_t *phi = (lc_phi_node_t *)calloc(1, sizeof(lc_phi_node_t));
    phi->result = wrap_vreg(mod, vreg_id, ty, f);
    phi->result->vreg.phi_node = phi;
    phi->type = ty;
    phi->block = b;
    phi->func = f;
    phi->mod = mod->mod;
    phi->cap_incoming = LC_INITIAL_PHI_CAP;
    phi->incoming_vals = (lc_value_t **)calloc(
        phi->cap_incoming, sizeof(lc_value_t *));
    phi->incoming_block_ids = (uint32_t *)calloc(
        phi->cap_incoming, sizeof(uint32_t));

    if (mod->phi_count == mod->phi_cap) {
        uint32_t new_cap = mod->phi_cap ? mod->phi_cap * 2 : 16;
        mod->pending_phis = (lc_phi_node_t **)realloc(
            mod->pending_phis, sizeof(lc_phi_node_t *) * new_cap);
        mod->phi_cap = new_cap;
    }
    mod->pending_phis[mod->phi_count++] = phi;
    return phi;
}

void lc_phi_add_incoming(lc_phi_node_t *phi, lc_value_t *val,
                         lr_block_t *block) {
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
}

void lc_phi_finalize(lc_phi_node_t *phi) {
    if (!phi || phi->finalized) return;
    phi->finalized = true;

    lr_module_t *m = phi->mod;
    uint32_t n = phi->num_incoming;
    uint32_t nops = n * 2;
    lr_operand_t *ops = lr_arena_array(m->arena, lr_operand_t, nops);

    for (uint32_t i = 0; i < n; i++) {
        ops[i * 2]     = lc_value_to_op_fast(phi->incoming_vals[i]);
        ops[i * 2 + 1] = lr_op_block(phi->incoming_block_ids[i]);
    }

    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_PHI, phi->type,
                                      phi->result->vreg.id, ops, nops);
    lr_block_append(phi->block, inst);

    free(phi->incoming_vals);
    phi->incoming_vals = NULL;
    free(phi->incoming_block_ids);
    phi->incoming_block_ids = NULL;
    phi->num_incoming = 0;
    phi->cap_incoming = 0;
    if (phi->result)
        phi->result->vreg.phi_node = NULL;
}

void lc_module_finalize_phis(lc_module_compat_t *mod) {
    if (!mod) return;
    for (uint32_t i = 0; i < mod->phi_count; i++) {
        lc_phi_node_t *phi = mod->pending_phis[i];
        lc_phi_finalize(phi);
        if (!phi) continue;
        free(phi->incoming_vals);
        free(phi->incoming_block_ids);
        free(phi);
    }
    free(mod->pending_phis);
    mod->pending_phis = NULL;
    mod->phi_count = 0;
    mod->phi_cap = 0;
}

/* ---- Select ---- */

lc_value_t *lc_create_select(lc_module_compat_t *mod, lr_block_t *b,
                              lr_func_t *f, lc_value_t *cond,
                              lc_value_t *true_val, lc_value_t *false_val,
                              const char *name) {
    (void)name;
    if (!mod || !b || !f || !true_val) return safe_undef(mod);
    lr_module_t *m = mod->mod;
    lr_type_t *ty = true_val->type ? true_val->type : m->type_i64;
    uint32_t dest = lr_vreg_new(f);
    lr_operand_t ops[3] = {
        lc_value_to_op_fast(cond),
        lc_value_to_op_fast(true_val),
        lc_value_to_op_fast(false_val)
    };
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_SELECT, ty,
                                      dest, ops, 3);
    lr_block_append(b, inst);
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
    /* liric does not have uitofp; use sitofp as approximation */
    return compat_cast(mod, b, f, LR_OP_SITOFP, val, to_type);
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
    /* liric does not have fptoui; use fptosi as approximation */
    return compat_cast(mod, b, f, LR_OP_FPTOSI, val, to_type);
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
    (void)name;
    if (!mod || !b || !f || !agg) return safe_undef(mod);
    lr_module_t *m = mod->mod;

    lr_type_t *result_ty = agg->type;
    if (!result_ty) result_ty = m->type_i64;
    for (unsigned i = 0; i < num_indices; i++) {
        if (result_ty->kind == LR_TYPE_STRUCT) {
            result_ty = result_ty->struc.fields[indices[i]];
        } else if (result_ty->kind == LR_TYPE_ARRAY) {
            result_ty = result_ty->array.elem;
        }
    }

    uint32_t dest = lr_vreg_new(f);
    lr_operand_t ops[1] = { lc_value_to_op_fast(agg) };
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_EXTRACTVALUE,
                                      result_ty, dest, ops, 1);
    inst->indices = lr_arena_array(m->arena, uint32_t, num_indices);
    memcpy(inst->indices, indices, sizeof(uint32_t) * num_indices);
    inst->num_indices = num_indices;
    lr_block_append(b, inst);
    return wrap_vreg(mod, dest, result_ty, f);
}

lc_value_t *lc_create_insertvalue(lc_module_compat_t *mod, lr_block_t *b,
                                   lr_func_t *f, lc_value_t *agg,
                                   lc_value_t *val, unsigned *indices,
                                   unsigned num_indices, const char *name) {
    (void)name;
    if (!mod || !b || !f || !agg) return safe_undef(mod);
    lr_module_t *m = mod->mod;
    uint32_t dest = lr_vreg_new(f);
    lr_operand_t ops[2] = { lc_value_to_op_fast(agg), lc_value_to_op_fast(val) };
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_INSERTVALUE,
                                      agg->type, dest, ops, 2);
    inst->indices = lr_arena_array(m->arena, uint32_t, num_indices);
    memcpy(inst->indices, indices, sizeof(uint32_t) * num_indices);
    inst->num_indices = num_indices;
    lr_block_append(b, inst);
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
    if (!mod || !b || !f) return;
    lr_module_t *m = mod->mod;
    lr_type_t *params[3] = { m->type_ptr, m->type_ptr, m->type_i64 };
    lr_func_t *decl = ensure_libc_decl(mod, "memcpy", m->type_ptr, params, 3);
    uint32_t sym_id = compat_symbol_id(mod, "memcpy");
    if (!decl || sym_id == UINT32_MAX) return;

    (void)decl;

    uint32_t nops = 4;
    lr_operand_t *ops = lr_arena_array(m->arena, lr_operand_t, nops);
    ops[0] = lr_op_global(sym_id, m->type_ptr);
    ops[1] = lc_value_to_op_fast(dst);
    ops[2] = lc_value_to_op_fast(src);
    ops[3] = lc_value_to_op_fast(size);

    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_CALL,
                                      m->type_ptr, lr_vreg_new(f),
                                      ops, nops);
    lr_block_append(b, inst);
}

void lc_create_memset(lc_module_compat_t *mod, lr_block_t *b, lr_func_t *f,
                      lc_value_t *dst, lc_value_t *val, lc_value_t *size) {
    if (!mod || !b || !f) return;
    lr_module_t *m = mod->mod;
    lr_type_t *params[3] = { m->type_ptr, m->type_i32, m->type_i64 };
    lr_func_t *decl = ensure_libc_decl(mod, "memset", m->type_ptr, params, 3);
    uint32_t sym_id = compat_symbol_id(mod, "memset");
    if (!decl || sym_id == UINT32_MAX) return;

    (void)decl;

    uint32_t nops = 4;
    lr_operand_t *ops = lr_arena_array(m->arena, lr_operand_t, nops);
    ops[0] = lr_op_global(sym_id, m->type_ptr);
    ops[1] = lc_value_to_op_fast(dst);
    ops[2] = lc_value_to_op_fast(val);
    ops[3] = lc_value_to_op_fast(size);

    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_CALL,
                                      m->type_ptr, lr_vreg_new(f),
                                      ops, nops);
    lr_block_append(b, inst);
}

void lc_create_memmove(lc_module_compat_t *mod, lr_block_t *b, lr_func_t *f,
                       lc_value_t *dst, lc_value_t *src, lc_value_t *size) {
    if (!mod || !b || !f) return;
    lr_module_t *m = mod->mod;
    lr_type_t *params[3] = { m->type_ptr, m->type_ptr, m->type_i64 };
    lr_func_t *decl = ensure_libc_decl(mod, "memmove", m->type_ptr, params, 3);
    uint32_t sym_id = compat_symbol_id(mod, "memmove");
    if (!decl || sym_id == UINT32_MAX) return;
    (void)decl;

    uint32_t nops = 4;
    lr_operand_t *ops = lr_arena_array(m->arena, lr_operand_t, nops);
    ops[0] = lr_op_global(sym_id, m->type_ptr);
    ops[1] = lc_value_to_op_fast(dst);
    ops[2] = lc_value_to_op_fast(src);
    ops[3] = lc_value_to_op_fast(size);

    lr_inst_t *inst2 = lr_inst_create(m->arena, LR_OP_CALL,
                                       m->type_ptr, lr_vreg_new(f),
                                       ops, nops);
    lr_block_append(b, inst2);
}

int lc_module_add_to_jit(lc_module_compat_t *mod, lr_jit_t *jit) {
    if (!mod || !jit) return -1;
    lc_module_finalize_phis(mod);
    return lr_jit_add_module(jit, mod->mod);
}

int lc_module_emit_object_to_file(lc_module_compat_t *mod, FILE *out) {
    if (!mod || !out) return -1;
    const lr_target_t *target = lr_target_host();
    if (!target) return -1;
    return lr_emit_object(mod->mod, target, out);
}

int lc_module_emit_object(lc_module_compat_t *mod, const char *filename) {
    if (!mod || !filename) return -1;
    FILE *f = fopen(filename, "wb");
    if (!f) return -1;
    int rc = lc_module_emit_object_to_file(mod, f);
    fclose(f);
    return rc;
}
