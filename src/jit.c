#include "jit.h"
#include "compile_mode.h"
#include "ir.h"
#include "llvm_backend.h"
#include "objfile.h"
#include "target.h"
#include "platform/platform.h"
#include "platform/platform_os.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#define LR_HAS_PTHREADS 1
#else
#define LR_HAS_PTHREADS 0
#endif

#ifdef LR_JIT_PROFILE
static double lr_jit_now_us(void) {
    return (double)lr_platform_time_ns() / 1e3;
}
#define JIT_PROF_START(name) double _prof_##name = lr_jit_now_us()
#define JIT_PROF_END(name) fprintf(stderr, "  jit-prof %-20s %7.2f us\n", #name, lr_jit_now_us() - _prof_##name)
#else
#define JIT_PROF_START(name) ((void)0)
#define JIT_PROF_END(name) ((void)0)
#endif

#define CODE_PAGE_SIZE (16 * 1024 * 1024)
#define DATA_PAGE_SIZE (16 * 1024 * 1024)
#define SYM_BUCKET_COUNT 8192u
#define MISS_BUCKET_COUNT 4096u
#define LAZY_FUNC_BUCKET_COUNT 8192u
#define MATERIALIZE_CACHE_BUCKET_COUNT 4096u
#define MATERIALIZE_CACHE_SCHEMA_VERSION 1u
#define MATERIALIZE_PREFETCH_MAX_THREADS 16u
#define MATERIALIZE_PREFETCH_MIN_PENDING 2u

typedef enum lr_lazy_func_state {
    LR_LAZY_FUNC_PENDING = 0,
    LR_LAZY_FUNC_COMPILING = 1,
    LR_LAZY_FUNC_READY = 2,
} lr_lazy_func_state_t;

struct lr_lazy_func_entry {
    char *name;
    uint32_t hash;
    lr_module_t *module;
    lr_func_t *func;
    const uint8_t *module_sig;
    size_t module_sig_len;
    const uint8_t *func_sig;
    size_t func_sig_len;
    void *pending_addr;
    lr_lazy_func_state_t state;
    lr_lazy_func_entry_t *next;
    lr_lazy_func_entry_t *bucket_next;
};

typedef struct lr_sig_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
} lr_sig_buf_t;

typedef struct lr_mat_cache_entry {
    uint64_t key_hash;
    uint32_t epoch;
    uint8_t target_ptr_size;
    const char *target_name;
    uint8_t *module_sig;
    size_t module_sig_len;
    uint8_t *func_sig;
    size_t func_sig_len;
    uint8_t *code;
    size_t code_len;
    lr_cached_reloc_t *relocs;
    uint32_t num_relocs;
    struct lr_mat_cache_entry *bucket_next;
    struct lr_mat_cache_entry *next;
} lr_mat_cache_entry_t;

typedef struct lr_materialize_prefetch_task {
    lr_lazy_func_entry_t *entry;
    uint8_t *code;
    size_t code_len;
    lr_cached_reloc_t *relocs;
    uint32_t num_relocs;
    int rc;
} lr_materialize_prefetch_task_t;

#if LR_HAS_PTHREADS
typedef struct lr_materialize_prefetch_worker {
    const lr_target_t *target;
    lr_compile_mode_t mode;
    size_t code_cap;
    lr_materialize_prefetch_task_t *tasks;
    uint32_t begin;
    uint32_t end;
} lr_materialize_prefetch_worker_t;
#endif

/*
 * Cache correctness invariants:
 * - Reuse requires exact module/function signature byte equality.
 * - Reuse is scoped by target identity + ptr size + cache schema + epoch.
 * - Stored code is pre-relocation bytes; stored reloc metadata is replayed.
 * - Replay failures are hard failures (no silent compile fallback).
 */
static lr_mat_cache_entry_t *g_mat_cache_buckets[MATERIALIZE_CACHE_BUCKET_COUNT];
static lr_mat_cache_entry_t *g_mat_cache_entries;
static uint64_t g_mat_cache_hit_count;
static uint64_t g_mat_cache_miss_count;
static uint64_t g_mat_cache_entry_count;
static uint32_t g_mat_cache_epoch = 1u;

static void register_builtin_symbols(lr_jit_t *j);
static int register_default_symbol_providers(lr_jit_t *j);
static void *resolve_symbol_from_loaded_libraries(lr_jit_t *j, const char *name);
static void *resolve_symbol_from_process(lr_jit_t *j, const char *name);
static lr_lazy_func_entry_t *find_lazy_func_entry(lr_jit_t *j, const char *name, uint32_t hash);
static int materialize_lazy_function(lr_jit_t *j, lr_lazy_func_entry_t *entry);
static int jit_ensure_module_symbols_interned(lr_module_t *m);
static void *lookup_symbol_hashed(lr_jit_t *j, const char *name, uint32_t hash);
static const lr_mat_cache_entry_t *materialize_cache_lookup(const lr_target_t *target,
                                                            const uint8_t *module_sig,
                                                            size_t module_sig_len,
                                                            const uint8_t *func_sig,
                                                            size_t func_sig_len,
                                                            bool update_stats);

static uint32_t symbol_hash(const char *name) {
    uint32_t h = 2166136261u;
    while (*name) {
        h ^= (uint8_t)*name++;
        h *= 16777619u;
    }
    return h;
}

static uint64_t hash64_extend(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static int sig_buf_reserve(lr_sig_buf_t *sb, size_t add) {
    if (!sb)
        return -1;
    if (add > SIZE_MAX - sb->len)
        return -1;
    size_t need = sb->len + add;
    if (need <= sb->cap)
        return 0;
    size_t new_cap = sb->cap ? sb->cap : 256u;
    while (new_cap < need) {
        if (new_cap > (SIZE_MAX / 2u)) {
            new_cap = need;
            break;
        }
        new_cap *= 2u;
    }
    uint8_t *new_data = (uint8_t *)realloc(sb->data, new_cap);
    if (!new_data)
        return -1;
    sb->data = new_data;
    sb->cap = new_cap;
    return 0;
}

static int sig_buf_append(lr_sig_buf_t *sb, const void *data, size_t n) {
    if (n == 0)
        return 0;
    if (sig_buf_reserve(sb, n) != 0)
        return -1;
    memcpy(sb->data + sb->len, data, n);
    sb->len += n;
    return 0;
}

static int sig_buf_u8(lr_sig_buf_t *sb, uint8_t v) {
    return sig_buf_append(sb, &v, sizeof(v));
}

static int sig_buf_u32(lr_sig_buf_t *sb, uint32_t v) {
    return sig_buf_append(sb, &v, sizeof(v));
}

static int sig_buf_u64(lr_sig_buf_t *sb, uint64_t v) {
    return sig_buf_append(sb, &v, sizeof(v));
}

static int sig_buf_i64(lr_sig_buf_t *sb, int64_t v) {
    return sig_buf_append(sb, &v, sizeof(v));
}

static int sig_buf_double_bits(lr_sig_buf_t *sb, double v) {
    uint64_t bits = 0;
    memcpy(&bits, &v, sizeof(bits));
    return sig_buf_u64(sb, bits);
}

static int sig_buf_str(lr_sig_buf_t *sb, const char *s) {
    uint32_t len = s ? (uint32_t)strlen(s) : 0u;
    if (sig_buf_u32(sb, len) != 0)
        return -1;
    if (len == 0)
        return 0;
    return sig_buf_append(sb, s, len);
}

static int sig_serialize_type(lr_sig_buf_t *sb, const lr_type_t *t) {
    if (!t)
        return sig_buf_u8(sb, 0xFFu);

    if (sig_buf_u8(sb, (uint8_t)t->kind) != 0)
        return -1;

    switch (t->kind) {
    case LR_TYPE_ARRAY:
        if (sig_buf_u64(sb, t->array.count) != 0)
            return -1;
        return sig_serialize_type(sb, t->array.elem);
    case LR_TYPE_STRUCT:
        if (sig_buf_u8(sb, t->struc.packed ? 1u : 0u) != 0)
            return -1;
        if (sig_buf_str(sb, t->struc.name) != 0)
            return -1;
        if (sig_buf_u32(sb, t->struc.num_fields) != 0)
            return -1;
        for (uint32_t i = 0; i < t->struc.num_fields; i++) {
            if (sig_serialize_type(sb, t->struc.fields[i]) != 0)
                return -1;
        }
        return 0;
    case LR_TYPE_FUNC:
        if (sig_serialize_type(sb, t->func.ret) != 0)
            return -1;
        if (sig_buf_u32(sb, t->func.num_params) != 0)
            return -1;
        if (sig_buf_u8(sb, t->func.vararg ? 1u : 0u) != 0)
            return -1;
        for (uint32_t i = 0; i < t->func.num_params; i++) {
            if (sig_serialize_type(sb, t->func.params[i]) != 0)
                return -1;
        }
        return 0;
    default:
        return 0;
    }
}

static int sig_serialize_operand(lr_sig_buf_t *sb, const lr_operand_t *op,
                                 const lr_module_t *m) {
    if (!op)
        return -1;
    if (sig_buf_u8(sb, (uint8_t)op->kind) != 0)
        return -1;
    if (sig_serialize_type(sb, op->type) != 0)
        return -1;
    if (sig_buf_i64(sb, op->global_offset) != 0)
        return -1;

    switch (op->kind) {
    case LR_VAL_VREG:
        return sig_buf_u32(sb, op->vreg);
    case LR_VAL_IMM_I64:
        return sig_buf_i64(sb, op->imm_i64);
    case LR_VAL_IMM_F64:
        return sig_buf_double_bits(sb, op->imm_f64);
    case LR_VAL_BLOCK:
        return sig_buf_u32(sb, op->block_id);
    case LR_VAL_GLOBAL: {
        const char *sym_name = m ? lr_module_symbol_name(m, op->global_id) : NULL;
        if (sig_buf_u32(sb, op->global_id) != 0)
            return -1;
        return sig_buf_str(sb, sym_name);
    }
    case LR_VAL_NULL:
    case LR_VAL_UNDEF:
        return 0;
    default:
        return -1;
    }
}

static int sig_serialize_inst(lr_sig_buf_t *sb, const lr_inst_t *inst,
                              const lr_module_t *m) {
    if (!inst)
        return -1;
    if (sig_buf_u8(sb, (uint8_t)inst->op) != 0)
        return -1;
    if (sig_serialize_type(sb, inst->type) != 0)
        return -1;
    if (sig_buf_u32(sb, inst->dest) != 0)
        return -1;
    if (sig_buf_u32(sb, inst->num_operands) != 0)
        return -1;
    if (sig_buf_u32(sb, inst->num_indices) != 0)
        return -1;
    if (sig_buf_u8(sb, inst->call_external_abi ? 1u : 0u) != 0)
        return -1;
    if (sig_buf_u8(sb, inst->call_vararg ? 1u : 0u) != 0)
        return -1;
    if (sig_buf_u32(sb, inst->call_fixed_args) != 0)
        return -1;
    if (sig_buf_u32(sb, (uint32_t)inst->icmp_pred) != 0)
        return -1;
    if (sig_buf_u32(sb, (uint32_t)inst->fcmp_pred) != 0)
        return -1;

    for (uint32_t i = 0; i < inst->num_operands; i++) {
        if (sig_serialize_operand(sb, &inst->operands[i], m) != 0)
            return -1;
    }
    for (uint32_t i = 0; i < inst->num_indices; i++) {
        if (sig_buf_u32(sb, inst->indices ? inst->indices[i] : 0u) != 0)
            return -1;
    }
    return 0;
}

static int sig_serialize_block(lr_sig_buf_t *sb, const lr_block_t *b,
                               const lr_module_t *m) {
    if (!b)
        return -1;
    if (sig_buf_u32(sb, b->id) != 0)
        return -1;
    if (sig_buf_str(sb, b->name) != 0)
        return -1;

    uint32_t num_insts = b->num_insts;
    if (num_insts == 0) {
        for (const lr_inst_t *inst = b->first; inst; inst = inst->next)
            num_insts++;
    }
    if (sig_buf_u32(sb, num_insts) != 0)
        return -1;

    if (b->inst_array && b->num_insts == num_insts) {
        for (uint32_t i = 0; i < num_insts; i++) {
            if (sig_serialize_inst(sb, b->inst_array[i], m) != 0)
                return -1;
        }
        return 0;
    }

    uint32_t seen = 0;
    for (const lr_inst_t *inst = b->first; inst; inst = inst->next) {
        if (sig_serialize_inst(sb, inst, m) != 0)
            return -1;
        seen++;
    }
    return seen == num_insts ? 0 : -1;
}

static int sig_serialize_function(lr_sig_buf_t *sb, const lr_module_t *m,
                                  const lr_func_t *f) {
    if (!f)
        return -1;
    if (sig_buf_str(sb, f->name) != 0)
        return -1;
    if (sig_buf_u8(sb, f->is_decl ? 1u : 0u) != 0)
        return -1;
    if (sig_buf_u8(sb, f->vararg ? 1u : 0u) != 0)
        return -1;
    if (sig_buf_u32(sb, f->num_params) != 0)
        return -1;
    if (sig_buf_u32(sb, f->next_vreg) != 0)
        return -1;
    if (sig_serialize_type(sb, f->ret_type) != 0)
        return -1;
    if (sig_serialize_type(sb, f->type) != 0)
        return -1;
    for (uint32_t i = 0; i < f->num_params; i++) {
        if (sig_serialize_type(sb, f->param_types ? f->param_types[i] : NULL) != 0)
            return -1;
    }
    for (uint32_t i = 0; i < f->num_params; i++) {
        uint32_t vreg = f->param_vregs ? f->param_vregs[i] : 0u;
        if (sig_buf_u32(sb, vreg) != 0)
            return -1;
    }

    if (sig_buf_u32(sb, f->num_blocks) != 0)
        return -1;
    if (f->num_blocks == 0)
        return 0;

    if (f->block_array) {
        for (uint32_t bi = 0; bi < f->num_blocks; bi++) {
            if (sig_serialize_block(sb, f->block_array[bi], m) != 0)
                return -1;
        }
        return 0;
    }

    uint32_t seen = 0;
    for (const lr_block_t *b = f->first_block; b; b = b->next) {
        if (sig_serialize_block(sb, b, m) != 0)
            return -1;
        seen++;
    }
    return seen == f->num_blocks ? 0 : -1;
}

static int sig_serialize_global(lr_sig_buf_t *sb, const lr_global_t *g) {
    if (!g)
        return -1;
    if (sig_buf_str(sb, g->name) != 0)
        return -1;
    if (sig_serialize_type(sb, g->type) != 0)
        return -1;
    if (sig_buf_u8(sb, g->is_const ? 1u : 0u) != 0)
        return -1;
    if (sig_buf_u8(sb, g->is_external ? 1u : 0u) != 0)
        return -1;
    if (sig_buf_u32(sb, g->id) != 0)
        return -1;

    if (sig_buf_u64(sb, (uint64_t)g->init_size) != 0)
        return -1;
    if (g->init_size > 0 && sig_buf_append(sb, g->init_data, g->init_size) != 0)
        return -1;

    uint32_t reloc_count = 0;
    for (const lr_reloc_t *r = g->relocs; r; r = r->next)
        reloc_count++;
    if (sig_buf_u32(sb, reloc_count) != 0)
        return -1;
    for (const lr_reloc_t *r = g->relocs; r; r = r->next) {
        if (sig_buf_u64(sb, (uint64_t)r->offset) != 0)
            return -1;
        if (sig_buf_i64(sb, r->addend) != 0)
            return -1;
        if (sig_buf_str(sb, r->symbol_name) != 0)
            return -1;
    }
    return 0;
}

static int sig_serialize_module(lr_sig_buf_t *sb, const lr_module_t *m) {
    if (!m)
        return -1;
    if (sig_buf_u32(sb, m->num_symbols) != 0)
        return -1;
    for (uint32_t i = 0; i < m->num_symbols; i++) {
        const char *sym_name = (m->symbol_names && i < m->num_symbols) ? m->symbol_names[i] : NULL;
        uint32_t sym_hash = m->symbol_hashes ? m->symbol_hashes[i]
                                             : (sym_name ? symbol_hash(sym_name) : 0u);
        if (sig_buf_u32(sb, sym_hash) != 0)
            return -1;
        if (sig_buf_str(sb, sym_name) != 0)
            return -1;
    }

    uint32_t nfuncs = 0;
    for (const lr_func_t *f = m->first_func; f; f = f->next)
        nfuncs++;
    if (sig_buf_u32(sb, nfuncs) != 0)
        return -1;
    for (const lr_func_t *f = m->first_func; f; f = f->next) {
        if (sig_serialize_function(sb, m, f) != 0)
            return -1;
    }

    uint32_t nglobals = 0;
    for (const lr_global_t *g = m->first_global; g; g = g->next)
        nglobals++;
    if (sig_buf_u32(sb, nglobals) != 0)
        return -1;
    for (const lr_global_t *g = m->first_global; g; g = g->next) {
        if (sig_serialize_global(sb, g) != 0)
            return -1;
    }

    return 0;
}

static int build_module_signature(const lr_module_t *m, uint8_t **out, size_t *out_len) {
    lr_sig_buf_t sb = {0};
    if (!out || !out_len)
        return -1;
    if (sig_serialize_module(&sb, m) != 0) {
        free(sb.data);
        return -1;
    }
    *out = sb.data;
    *out_len = sb.len;
    return 0;
}

static int build_function_signature(const lr_module_t *m, const lr_func_t *f,
                                    uint8_t **out, size_t *out_len) {
    lr_sig_buf_t sb = {0};
    if (!out || !out_len)
        return -1;
    if (sig_serialize_function(&sb, m, f) != 0) {
        free(sb.data);
        return -1;
    }
    *out = sb.data;
    *out_len = sb.len;
    return 0;
}

static const uint8_t *arena_dup_bytes(lr_arena_t *arena, const uint8_t *src, size_t n) {
    if (!arena || !src || n == 0)
        return NULL;
    uint8_t *dst = (uint8_t *)lr_arena_alloc_uninit(arena, n, _Alignof(uint8_t));
    if (!dst)
        return NULL;
    memcpy(dst, src, n);
    return dst;
}

static uint64_t materialize_key_hash(const lr_target_t *target,
                                     const uint8_t *module_sig, size_t module_sig_len,
                                     const uint8_t *func_sig, size_t func_sig_len,
                                     uint32_t epoch) {
    const uint64_t seed = 1469598103934665603ull;
    const char *target_name = (target && target->name) ? target->name : "";
    uint32_t schema = MATERIALIZE_CACHE_SCHEMA_VERSION;
    uint32_t tname_len = (uint32_t)strlen(target_name);
    uint8_t ptr_size = target ? target->ptr_size : 0u;
    uint64_t h = seed;
    h = hash64_extend(h, &schema, sizeof(schema));
    h = hash64_extend(h, &epoch, sizeof(epoch));
    h = hash64_extend(h, &ptr_size, sizeof(ptr_size));
    h = hash64_extend(h, &tname_len, sizeof(tname_len));
    h = hash64_extend(h, target_name, tname_len);
    h = hash64_extend(h, &module_sig_len, sizeof(module_sig_len));
    h = hash64_extend(h, module_sig, module_sig_len);
    h = hash64_extend(h, &func_sig_len, sizeof(func_sig_len));
    h = hash64_extend(h, func_sig, func_sig_len);
    return h;
}

static void free_materialize_cache_entry(lr_mat_cache_entry_t *entry) {
    if (!entry)
        return;
    free((char *)entry->target_name);
    free(entry->module_sig);
    free(entry->func_sig);
    free(entry->code);
    if (entry->relocs) {
        for (uint32_t i = 0; i < entry->num_relocs; i++)
            free((void *)entry->relocs[i].symbol_name);
    }
    free(entry->relocs);
    free(entry);
}

static void clear_materialize_cache_entries(void) {
    lr_mat_cache_entry_t *entry = g_mat_cache_entries;
    while (entry) {
        lr_mat_cache_entry_t *next = entry->next;
        free_materialize_cache_entry(entry);
        entry = next;
    }
    memset(g_mat_cache_buckets, 0, sizeof(g_mat_cache_buckets));
    g_mat_cache_entries = NULL;
    g_mat_cache_entry_count = 0;
}

static bool materialize_cache_key_matches(const lr_mat_cache_entry_t *entry,
                                          const lr_target_t *target,
                                          const uint8_t *module_sig, size_t module_sig_len,
                                          const uint8_t *func_sig, size_t func_sig_len,
                                          uint32_t epoch) {
    const char *target_name = (target && target->name) ? target->name : "";
    if (!entry || entry->epoch != epoch)
        return false;
    if (entry->target_ptr_size != (target ? target->ptr_size : 0u))
        return false;
    if (strcmp(entry->target_name ? entry->target_name : "", target_name) != 0)
        return false;
    if (entry->module_sig_len != module_sig_len || entry->func_sig_len != func_sig_len)
        return false;
    if (module_sig_len > 0 && memcmp(entry->module_sig, module_sig, module_sig_len) != 0)
        return false;
    if (func_sig_len > 0 && memcmp(entry->func_sig, func_sig, func_sig_len) != 0)
        return false;
    return true;
}

static const lr_mat_cache_entry_t *materialize_cache_lookup(const lr_target_t *target,
                                                            const uint8_t *module_sig,
                                                            size_t module_sig_len,
                                                            const uint8_t *func_sig,
                                                            size_t func_sig_len,
                                                            bool update_stats) {
    if (!target || !module_sig || module_sig_len == 0 || !func_sig || func_sig_len == 0) {
        if (update_stats)
            g_mat_cache_miss_count++;
        return NULL;
    }

    uint64_t key_hash = materialize_key_hash(target, module_sig, module_sig_len,
                                             func_sig, func_sig_len, g_mat_cache_epoch);
    uint32_t bucket = (uint32_t)(key_hash & (MATERIALIZE_CACHE_BUCKET_COUNT - 1u));
    for (lr_mat_cache_entry_t *entry = g_mat_cache_buckets[bucket]; entry; entry = entry->bucket_next) {
        if (entry->key_hash != key_hash)
            continue;
        if (materialize_cache_key_matches(entry, target, module_sig, module_sig_len,
                                          func_sig, func_sig_len, g_mat_cache_epoch)) {
            if (update_stats)
                g_mat_cache_hit_count++;
            return entry;
        }
    }
    if (update_stats)
        g_mat_cache_miss_count++;
    return NULL;
}

static int materialize_cache_insert(const lr_target_t *target,
                                    const uint8_t *module_sig, size_t module_sig_len,
                                    const uint8_t *func_sig, size_t func_sig_len,
                                    const uint8_t *code, size_t code_len,
                                    const lr_cached_reloc_t *relocs,
                                    uint32_t num_relocs) {
    if (!target || !module_sig || module_sig_len == 0 || !func_sig || func_sig_len == 0 ||
        !code || code_len == 0)
        return -1;

    uint64_t key_hash = materialize_key_hash(target, module_sig, module_sig_len,
                                             func_sig, func_sig_len, g_mat_cache_epoch);
    uint32_t bucket = (uint32_t)(key_hash & (MATERIALIZE_CACHE_BUCKET_COUNT - 1u));
    for (lr_mat_cache_entry_t *entry = g_mat_cache_buckets[bucket]; entry; entry = entry->bucket_next) {
        if (entry->key_hash != key_hash)
            continue;
        if (materialize_cache_key_matches(entry, target, module_sig, module_sig_len,
                                          func_sig, func_sig_len, g_mat_cache_epoch))
            return 0;
    }

    lr_mat_cache_entry_t *entry = (lr_mat_cache_entry_t *)calloc(1, sizeof(*entry));
    if (!entry)
        return -1;
    entry->key_hash = key_hash;
    entry->epoch = g_mat_cache_epoch;
    entry->target_ptr_size = target->ptr_size;
    entry->target_name = target->name ? strdup(target->name) : strdup("");
    if (!entry->target_name)
        goto fail;

    entry->module_sig = (uint8_t *)malloc(module_sig_len);
    entry->func_sig = (uint8_t *)malloc(func_sig_len);
    entry->code = (uint8_t *)malloc(code_len);
    if (!entry->module_sig || !entry->func_sig || !entry->code)
        goto fail;
    memcpy(entry->module_sig, module_sig, module_sig_len);
    memcpy(entry->func_sig, func_sig, func_sig_len);
    memcpy(entry->code, code, code_len);
    entry->module_sig_len = module_sig_len;
    entry->func_sig_len = func_sig_len;
    entry->code_len = code_len;

    if (num_relocs > 0) {
        entry->relocs = (lr_cached_reloc_t *)calloc(num_relocs, sizeof(*entry->relocs));
        if (!entry->relocs)
            goto fail;
        entry->num_relocs = num_relocs;
        for (uint32_t i = 0; i < num_relocs; i++) {
            entry->relocs[i].offset = relocs[i].offset;
            entry->relocs[i].type = relocs[i].type;
            entry->relocs[i].symbol_name = relocs[i].symbol_name
                ? strdup(relocs[i].symbol_name)
                : strdup("");
            if (!entry->relocs[i].symbol_name)
                goto fail;
        }
    }

    entry->bucket_next = g_mat_cache_buckets[bucket];
    g_mat_cache_buckets[bucket] = entry;
    entry->next = g_mat_cache_entries;
    g_mat_cache_entries = entry;
    g_mat_cache_entry_count++;
    return 0;

fail:
    free_materialize_cache_entry(entry);
    return -1;
}

void lr_jit_materialize_cache_invalidate_all(void) {
    clear_materialize_cache_entries();
    g_mat_cache_epoch++;
    if (g_mat_cache_epoch == 0u)
        g_mat_cache_epoch = 1u;
}

uint32_t lr_jit_materialize_cache_epoch(void) {
    return g_mat_cache_epoch;
}

void lr_jit_materialize_cache_reset_stats(void) {
    g_mat_cache_hit_count = 0;
    g_mat_cache_miss_count = 0;
}

uint64_t lr_jit_materialize_cache_hits(void) {
    return g_mat_cache_hit_count;
}

uint64_t lr_jit_materialize_cache_misses(void) {
    return g_mat_cache_miss_count;
}

uint64_t lr_jit_materialize_cache_entries(void) {
    return g_mat_cache_entry_count;
}

static int jit_ensure_module_symbols_interned(lr_module_t *m) {
    if (!m)
        return -1;
    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (f->name && f->name[0] &&
            lr_module_intern_symbol(m, f->name) == UINT32_MAX)
            return -1;
    }
    for (lr_global_t *g = m->first_global; g; g = g->next) {
        if (g->name && g->name[0] &&
            lr_module_intern_symbol(m, g->name) == UINT32_MAX)
            return -1;
    }
    return 0;
}

static size_t align_up(size_t value, size_t align) {
    if (align == 0)
        return value;
    size_t mask = align - 1;
    return (value + mask) & ~mask;
}

static int make_writable(lr_jit_t *j) {
    return lr_platform_jit_make_writable(j->code_buf, j->code_cap, j->map_jit_enabled);
}

static int make_executable_from(lr_jit_t *j, size_t clear_from) {
    if (clear_from > j->code_size)
        clear_from = j->code_size;
    const void *clear_begin = (clear_from < j->code_size) ? (j->code_buf + clear_from) : NULL;
    const void *clear_end = (clear_from < j->code_size) ? (j->code_buf + j->code_size) : NULL;
    return lr_platform_jit_make_executable(j->code_buf, j->code_cap, j->map_jit_enabled,
                                           clear_begin, clear_end);
}

static int make_executable(lr_jit_t *j) {
    return make_executable_from(j, 0);
}

const char *lr_jit_host_target_name(void) {
    const lr_target_t *host = lr_target_host();
    return host ? host->name : NULL;
}

const char *lr_jit_target_name(const lr_jit_t *j) {
    return (j && j->target) ? j->target->name : NULL;
}

lr_jit_t *lr_jit_create_for_target(const char *target_name) {
    const lr_target_t *target = lr_target_by_name(target_name);
    if (!target || !lr_target_is_host_compatible(target))
        return NULL;

    lr_jit_t *j = calloc(1, sizeof(lr_jit_t));
    if (!j) return NULL;

    j->target = target;

    j->mode = lr_compile_mode_from_env();

    j->arena = lr_arena_create(0);
    if (!j->arena) {
        free(j);
        return NULL;
    }
    j->sym_bucket_count = SYM_BUCKET_COUNT;
    j->miss_bucket_count = MISS_BUCKET_COUNT;
    j->lazy_func_bucket_count = LAZY_FUNC_BUCKET_COUNT;
    j->sym_buckets = calloc(j->sym_bucket_count, sizeof(*j->sym_buckets));
    j->miss_buckets = calloc(j->miss_bucket_count, sizeof(*j->miss_buckets));
    j->lazy_func_buckets = calloc(j->lazy_func_bucket_count, sizeof(*j->lazy_func_buckets));
    if (!j->sym_buckets || !j->miss_buckets || !j->lazy_func_buckets) {
        free(j->lazy_func_buckets);
        free(j->miss_buckets);
        free(j->sym_buckets);
        lr_arena_destroy(j->arena);
        free(j);
        return NULL;
    }

    j->code_cap = CODE_PAGE_SIZE;
    j->code_buf = lr_platform_alloc_jit_code(j->code_cap, &j->map_jit_enabled);
    if (!j->code_buf) {
        free(j->lazy_func_buckets);
        free(j->miss_buckets);
        free(j->sym_buckets);
        lr_arena_destroy(j->arena);
        free(j);
        return NULL;
    }

    j->data_cap = DATA_PAGE_SIZE;
    j->data_buf = lr_platform_alloc_rw(j->data_cap);
    if (!j->data_buf) {
        (void)lr_platform_free_pages(j->code_buf, j->code_cap);
        free(j->lazy_func_buckets);
        free(j->miss_buckets);
        free(j->sym_buckets);
        lr_arena_destroy(j->arena);
        free(j);
        return NULL;
    }

    if (make_executable(j) != 0) {
        (void)lr_platform_free_pages(j->data_buf, j->data_cap);
        (void)lr_platform_free_pages(j->code_buf, j->code_cap);
        free(j->lazy_func_buckets);
        free(j->miss_buckets);
        free(j->sym_buckets);
        lr_arena_destroy(j->arena);
        free(j);
        return NULL;
    }

    if (register_default_symbol_providers(j) != 0) {
        (void)lr_platform_free_pages(j->data_buf, j->data_cap);
        (void)lr_platform_free_pages(j->code_buf, j->code_cap);
        free(j->lazy_func_buckets);
        free(j->miss_buckets);
        free(j->sym_buckets);
        lr_arena_destroy(j->arena);
        free(j);
        return NULL;
    }

    register_builtin_symbols(j);

    return j;
}

lr_jit_t *lr_jit_create(void) {
    const char *host = lr_jit_host_target_name();
    return host ? lr_jit_create_for_target(host) : NULL;
}

static lr_sym_entry_t *find_symbol_entry(lr_jit_t *j, const char *name, uint32_t hash) {
    if (!j || !j->sym_buckets || j->sym_bucket_count == 0)
        return NULL;
    uint32_t bucket = hash & (j->sym_bucket_count - 1u);
    for (lr_sym_entry_t *e = j->sym_buckets[bucket]; e; e = e->bucket_next) {
        if (e->hash == hash && strcmp(e->name, name) == 0)
            return e;
    }
    return NULL;
}

static lr_sym_entry_t *lookup_last_entry(lr_jit_t *j, const char *name, uint32_t hash) {
    if (!j || !j->lookup_last_entry || j->lookup_last_hash != hash)
        return NULL;
    lr_sym_entry_t *entry = j->lookup_last_entry;
    if (entry->hash != hash || strcmp(entry->name, name) != 0)
        return NULL;
    return entry;
}

static void update_last_symbol_lookup(lr_jit_t *j, lr_sym_entry_t *entry, uint32_t hash) {
    if (!j)
        return;
    j->lookup_last_entry = entry;
    j->lookup_last_hash = entry ? hash : 0u;
}

static lr_lazy_func_entry_t *lookup_last_lazy_entry(lr_jit_t *j, const char *name, uint32_t hash) {
    if (!j || !j->lookup_last_lazy_entry || j->lookup_last_lazy_hash != hash)
        return NULL;
    lr_lazy_func_entry_t *entry = j->lookup_last_lazy_entry;
    if (entry->hash != hash || strcmp(entry->name, name) != 0)
        return NULL;
    return entry;
}

static void update_last_lazy_lookup(lr_jit_t *j, lr_lazy_func_entry_t *entry, uint32_t hash) {
    if (!j)
        return;
    j->lookup_last_lazy_entry = entry;
    j->lookup_last_lazy_hash = entry ? hash : 0u;
}

static lr_lazy_func_entry_t *find_lazy_func_entry(lr_jit_t *j, const char *name, uint32_t hash) {
    if (!j || !name || !name[0] || !j->lazy_func_buckets || j->lazy_func_bucket_count == 0)
        return NULL;
    lr_lazy_func_entry_t *cached = lookup_last_lazy_entry(j, name, hash);
    if (cached)
        return cached;
    uint32_t bucket = hash & (j->lazy_func_bucket_count - 1u);
    for (lr_lazy_func_entry_t *e = j->lazy_func_buckets[bucket]; e; e = e->bucket_next) {
        if (e->hash == hash && strcmp(e->name, name) == 0) {
            update_last_lazy_lookup(j, e, hash);
            return e;
        }
    }
    return NULL;
}

static lr_lazy_func_entry_t *upsert_lazy_func_entry(lr_jit_t *j, lr_module_t *m, lr_func_t *f,
                                                    const uint8_t *module_sig, size_t module_sig_len,
                                                    const uint8_t *func_sig, size_t func_sig_len) {
    if (!j || !f || !f->name || !f->name[0] || !m)
        return NULL;

    uint32_t hash = symbol_hash(f->name);
    lr_lazy_func_entry_t *existing = find_lazy_func_entry(j, f->name, hash);
    if (existing) {
        existing->module = m;
        existing->func = f;
        existing->module_sig = module_sig;
        existing->module_sig_len = module_sig_len;
        existing->func_sig = func_sig;
        existing->func_sig_len = func_sig_len;
        existing->pending_addr = NULL;
        existing->state = LR_LAZY_FUNC_PENDING;
        update_last_lazy_lookup(j, existing, hash);
        return existing;
    }

    lr_lazy_func_entry_t *entry = lr_arena_new(j->arena, lr_lazy_func_entry_t);
    if (!entry)
        return NULL;
    entry->name = lr_arena_strdup(j->arena, f->name, strlen(f->name));
    entry->hash = hash;
    entry->module = m;
    entry->func = f;
    entry->module_sig = module_sig;
    entry->module_sig_len = module_sig_len;
    entry->func_sig = func_sig;
    entry->func_sig_len = func_sig_len;
    entry->pending_addr = NULL;
    entry->state = LR_LAZY_FUNC_PENDING;
    entry->next = j->lazy_funcs;
    j->lazy_funcs = entry;

    if (j->lazy_func_buckets && j->lazy_func_bucket_count > 0) {
        uint32_t bucket = hash & (j->lazy_func_bucket_count - 1u);
        entry->bucket_next = j->lazy_func_buckets[bucket];
        j->lazy_func_buckets[bucket] = entry;
    } else {
        entry->bucket_next = NULL;
    }
    update_last_lazy_lookup(j, entry, hash);
    return entry;
}

static bool miss_cache_contains(lr_jit_t *j, const char *name, uint32_t hash) {
    if (!j || !j->miss_buckets || j->miss_bucket_count == 0)
        return false;
    uint32_t bucket = hash & (j->miss_bucket_count - 1u);
    for (lr_sym_miss_entry_t *m = j->miss_buckets[bucket]; m; m = m->bucket_next) {
        if (m->hash == hash && strcmp(m->name, name) == 0)
            return true;
    }
    return false;
}

static void miss_cache_add(lr_jit_t *j, const char *name, uint32_t hash) {
    if (!j || !j->miss_buckets || j->miss_bucket_count == 0)
        return;
    if (miss_cache_contains(j, name, hash))
        return;
    uint32_t bucket = hash & (j->miss_bucket_count - 1u);
    lr_sym_miss_entry_t *m = lr_arena_new(j->arena, lr_sym_miss_entry_t);
    m->name = lr_arena_strdup(j->arena, name, strlen(name));
    m->hash = hash;
    m->bucket_next = j->miss_buckets[bucket];
    j->miss_buckets[bucket] = m;
}

static int register_symbol_provider(lr_jit_t *j, const char *name,
                                    lr_symbol_provider_resolve_fn resolve,
                                    bool skip_when_miss_cached,
                                    bool cache_result) {
    if (!j || !name || !name[0] || !resolve)
        return -1;

    lr_symbol_provider_t *provider = lr_arena_new(j->arena, lr_symbol_provider_t);
    if (!provider)
        return -1;

    provider->name = name;
    provider->resolve = resolve;
    provider->skip_when_miss_cached = skip_when_miss_cached;
    provider->cache_result = cache_result;
    provider->next = NULL;

    if (!j->symbol_providers) {
        j->symbol_providers = provider;
        j->symbol_providers_tail = provider;
    } else {
        j->symbol_providers_tail->next = provider;
        j->symbol_providers_tail = provider;
    }
    return 0;
}

static int register_default_symbol_providers(lr_jit_t *j) {
    if (register_symbol_provider(j, "loaded-libraries", resolve_symbol_from_loaded_libraries,
                                 true, true) != 0)
        return -1;
    if (register_symbol_provider(j, "process", resolve_symbol_from_process,
                                 true, true) != 0)
        return -1;
    return 0;
}

void lr_jit_add_symbol(lr_jit_t *j, const char *name, void *addr) {
    if (!j || !name || !name[0])
        return;
    uint32_t hash = symbol_hash(name);
    lr_sym_entry_t *existing = find_symbol_entry(j, name, hash);
    if (existing) {
        existing->addr = addr;
        update_last_symbol_lookup(j, existing, hash);
        return;
    }

    lr_sym_entry_t *e = lr_arena_new(j->arena, lr_sym_entry_t);
    e->name = lr_arena_strdup(j->arena, name, strlen(name));
    e->hash = hash;
    e->addr = addr;
    e->next = j->symbols;
    j->symbols = e;
    if (j->sym_buckets && j->sym_bucket_count > 0) {
        uint32_t bucket = hash & (j->sym_bucket_count - 1u);
        e->bucket_next = j->sym_buckets[bucket];
        j->sym_buckets[bucket] = e;
    } else {
        e->bucket_next = NULL;
    }
    update_last_symbol_lookup(j, e, hash);
}

static void register_builtin_symbols(lr_jit_t *j) {
    size_t n = lr_platform_intrinsic_count();
    for (size_t i = 0; i < n; i++) {
        const char *name = lr_platform_intrinsic_name(i);
        const uint8_t *blob_begin, *blob_end;
        if (lr_platform_intrinsic_blob_lookup(name, &blob_begin, &blob_end)) {
            size_t blob_size = (size_t)(blob_end - blob_begin);
            size_t dest = align_up(j->code_size, 16);
            if (dest + blob_size > j->code_cap)
                continue;
            if (make_writable(j) != 0)
                continue;
            memcpy(j->code_buf + dest, blob_begin, blob_size);
            j->code_size = dest + blob_size;
            make_executable_from(j, dest);
            lr_jit_add_symbol(j, name, (void *)(j->code_buf + dest));
            continue;
        }
        /* No blob available (e.g. macOS): resolve via libc equivalent. */
        const char *libc_name = lr_platform_intrinsic_libc_name(name);
        if (libc_name != name) {
            void *addr = lr_platform_dlsym_default(libc_name);
            if (addr)
                lr_jit_add_symbol(j, name, addr);
        }
    }
}

int lr_jit_load_library(lr_jit_t *j, const char *path) {
    if (!j || !path || !path[0])
        return -1;
    void *handle = lr_platform_dlopen(path);
    if (!handle)
        return -1;
    lr_lib_entry_t *entry = lr_arena_new(j->arena, lr_lib_entry_t);
    entry->handle = handle;
    entry->next = j->libs;
    j->libs = entry;
    if (j->miss_buckets && j->miss_bucket_count > 0) {
        memset(j->miss_buckets, 0, j->miss_bucket_count * sizeof(*j->miss_buckets));
    }
    return 0;
}

static void *resolve_symbol_from_loaded_libraries(lr_jit_t *j, const char *name) {
    if (!j || !name || !name[0])
        return NULL;
    for (lr_lib_entry_t *l = j->libs; l; l = l->next) {
        void *addr = lr_platform_dlsym(l->handle, name);
        if (addr)
            return addr;
    }
    return NULL;
}

static void *resolve_symbol_from_process(lr_jit_t *j, const char *name) {
    (void)j;
    if (!name || !name[0])
        return NULL;
    return lr_platform_dlsym_default(name);
}

static void *lookup_symbol_hashed(lr_jit_t *j, const char *name, uint32_t hash) {
    if (j->lazy_funcs) {
        lr_lazy_func_entry_t *lazy = find_lazy_func_entry(j, name, hash);
        if (lazy) {
            if (lazy->state == LR_LAZY_FUNC_COMPILING)
                return lazy->pending_addr;
            if (lazy->state != LR_LAZY_FUNC_READY)
                return NULL;
        }
    }

    lr_sym_entry_t *cached_entry = lookup_last_entry(j, name, hash);
    if (cached_entry)
        return cached_entry->addr;

    lr_sym_entry_t *entry = find_symbol_entry(j, name, hash);
    if (entry) {
        update_last_symbol_lookup(j, entry, hash);
        return entry->addr;
    }

    bool miss_known = miss_cache_contains(j, name, hash);

    for (lr_symbol_provider_t *provider = j->symbol_providers;
         provider; provider = provider->next) {
        if (miss_known && provider->skip_when_miss_cached)
            continue;

        void *addr = provider->resolve(j, name);
        if (!addr)
            continue;

        if (provider->cache_result)
            lr_jit_add_symbol(j, name, addr);
        return addr;
    }

    if (!miss_known)
        miss_cache_add(j, name, hash);
    return NULL;
}

static void *lookup_symbol(lr_jit_t *j, const char *name) {
    if (!j || !name || !name[0])
        return NULL;
    return lookup_symbol_hashed(j, name, symbol_hash(name));
}

static void *lookup_symbol_materializing_lazy(lr_jit_t *j, const char *name) {
    if (!j || !name || !name[0])
        return NULL;

    uint32_t hash = symbol_hash(name);
    void *addr = lookup_symbol_hashed(j, name, hash);
    if (addr)
        return addr;

    lr_lazy_func_entry_t *lazy = find_lazy_func_entry(j, name, hash);
    if (!lazy || lazy->state == LR_LAZY_FUNC_READY || lazy->state == LR_LAZY_FUNC_COMPILING)
        return NULL;
    if (materialize_lazy_function(j, lazy) != 0)
        return NULL;
    return lookup_symbol_hashed(j, name, hash);
}

static int apply_module_global_relocs(lr_jit_t *j, lr_module_t *m) {
    for (lr_global_t *g = m->first_global; g; g = g->next) {
        if (!g->relocs)
            continue;
        uint8_t *base = lookup_symbol(j, g->name);
        if (!base)
            continue;
        for (lr_reloc_t *r = g->relocs; r; r = r->next) {
            void *target = lookup_symbol_materializing_lazy(j, r->symbol_name);
            if (!target)
                continue;
            uintptr_t addr = (uintptr_t)((intptr_t)target + r->addend);
            size_t global_size = lr_type_size(g->type);
            if (global_size == 0)
                global_size = sizeof(void *);
            if (r->offset + sizeof(uintptr_t) <= global_size)
                memcpy(base + r->offset, &addr, sizeof(addr));
        }
    }
    return 0;
}

int lr_jit_materialize_globals(lr_jit_t *j, lr_module_t *m) {
    /* First pass: allocate space and copy raw init_data for all globals */
    for (lr_global_t *g = m->first_global; g; g = g->next) {
        if (!g->name || !g->name[0])
            continue;
        if (g->is_external) {
            if (lookup_symbol(j, g->name))
                continue;
        } else {
            uint32_t hash = symbol_hash(g->name);
            if (find_symbol_entry(j, g->name, hash))
                continue;
        }

        size_t align = lr_type_align(g->type);
        if (align == 0)
            align = sizeof(void *);
        size_t size = lr_type_size(g->type);
        if (size == 0)
            size = sizeof(void *);

        size_t off = align_up(j->data_size, align);
        if (off + size > j->data_cap)
            return -1;

        uint8_t *dst = j->data_buf + off;
        memset(dst, 0, size);
        if (g->init_data && g->init_size > 0) {
            size_t copy_n = g->init_size < size ? g->init_size : size;
            memcpy(dst, g->init_data, copy_n);
        }

        j->data_size = off + size;
        lr_jit_add_symbol(j, g->name, dst);
    }

    /* Initial relocation pass may already resolve globals/external symbols. */
    return apply_module_global_relocs(j, m);
}

static uint32_t module_symbol_id(const lr_module_t *m, const char *name, uint32_t hash) {
    if (!m || !name || !name[0] || m->symbol_index_cap == 0)
        return UINT32_MAX;
    uint32_t slot = hash & (m->symbol_index_cap - 1u);
    for (;;) {
        uint32_t stored = m->symbol_index[slot];
        if (stored == 0)
            return UINT32_MAX;
        uint32_t idx = stored - 1u;
        if (m->symbol_hashes[idx] == hash &&
            strcmp(m->symbol_names[idx], name) == 0)
            return idx;
        slot = (slot + 1u) & (m->symbol_index_cap - 1u);
    }
}

static int jit_build_module_symbol_cache(lr_objfile_ctx_t *oc, lr_module_t *m) {
    if (!oc || !m)
        return -1;
    if (jit_ensure_module_symbols_interned(m) != 0)
        return -1;

    oc->module_sym_count = m->num_symbols;
    if (oc->module_sym_count == 0)
        return 0;

    oc->module_sym_defined = (uint8_t *)calloc(oc->module_sym_count, sizeof(uint8_t));
    oc->module_sym_funcs = (lr_func_t **)calloc(oc->module_sym_count, sizeof(lr_func_t *));
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
        uint32_t hash = symbol_hash(f->name);
        uint32_t sym_id = module_symbol_id(m, f->name, hash);
        if (sym_id == UINT32_MAX)
            return -1;
        if (sym_id >= oc->module_sym_count)
            continue;
        oc->module_sym_funcs[sym_id] = f;
        if (f->first_block)
            oc->module_sym_defined[sym_id] = 1;
    }
    for (lr_global_t *g = m->first_global; g; g = g->next) {
        if (!g->name || !g->name[0] || g->is_external)
            continue;
        uint32_t hash = symbol_hash(g->name);
        uint32_t sym_id = module_symbol_id(m, g->name, hash);
        if (sym_id == UINT32_MAX)
            return -1;
        if (sym_id < oc->module_sym_count)
            oc->module_sym_defined[sym_id] = 1;
    }
    return 0;
}

static void jit_free_obj_ctx(lr_objfile_ctx_t *ctx) {
    if (!ctx)
        return;
    free(ctx->relocs);
    free(ctx->symbols);
    free(ctx->symbol_index);
    free(ctx->module_sym_defined);
    free(ctx->module_sym_funcs);
    memset(ctx, 0, sizeof(*ctx));
}

static uint32_t read_u32(const uint8_t *buf, uint32_t off) {
    return (uint32_t)buf[off]
         | ((uint32_t)buf[off + 1] << 8)
         | ((uint32_t)buf[off + 2] << 16)
         | ((uint32_t)buf[off + 3] << 24);
}

static int write_u32(uint8_t *buf, size_t buflen, uint32_t off, uint32_t value) {
    if ((size_t)off + 4 > buflen)
        return -1;
    buf[off] = (uint8_t)value;
    buf[off + 1] = (uint8_t)(value >> 8);
    buf[off + 2] = (uint8_t)(value >> 16);
    buf[off + 3] = (uint8_t)(value >> 24);
    return 0;
}

static int write_u64(uint8_t *buf, size_t buflen, uint32_t off, uint64_t value) {
    if ((size_t)off + 8 > buflen)
        return -1;
    for (int i = 0; i < 8; i++)
        buf[off + i] = (uint8_t)(value >> (i * 8));
    return 0;
}

static int patch_x86_rel32(uint8_t *buf, size_t buflen, uint32_t off, uintptr_t target) {
    uintptr_t place = (uintptr_t)(buf + off);
    int64_t disp = (int64_t)(intptr_t)target - (int64_t)(intptr_t)(place + 4u);
    if (disp < INT32_MIN || disp > INT32_MAX)
        return -1;
    return write_u32(buf, buflen, off, (uint32_t)(int32_t)disp);
}

static int patch_aarch64_branch26(uint8_t *buf, size_t buflen, uint32_t off, uintptr_t target) {
    if ((size_t)off + 4 > buflen)
        return -1;
    uintptr_t place = (uintptr_t)(buf + off);
    int64_t imm = ((int64_t)(intptr_t)target - (int64_t)(intptr_t)place) / 4;
    if (imm < -(1LL << 25) || imm >= (1LL << 25))
        return -1;
    uint32_t insn = read_u32(buf, off);
    insn = (insn & 0xFC000000u) | ((uint32_t)imm & 0x03FFFFFFu);
    return write_u32(buf, buflen, off, insn);
}

static int patch_aarch64_page21(uint8_t *buf, size_t buflen, uint32_t off, uintptr_t target) {
    if ((size_t)off + 4 > buflen)
        return -1;
    uintptr_t place = (uintptr_t)(buf + off);
    uint64_t target_page = ((uint64_t)target) & ~0xFFFULL;
    uint64_t place_page = ((uint64_t)place) & ~0xFFFULL;
    int64_t pages = ((int64_t)target_page - (int64_t)place_page) >> 12;
    if (pages < -(1LL << 20) || pages >= (1LL << 20))
        return -1;
    uint32_t insn = read_u32(buf, off);
    insn &= ~((0x3u << 29) | (0x7FFFFu << 5));
    insn |= (((uint32_t)pages & 0x3u) << 29);
    insn |= ((((uint32_t)pages >> 2) & 0x7FFFFu) << 5);
    return write_u32(buf, buflen, off, insn);
}

static int patch_aarch64_pageoff12(uint8_t *buf, size_t buflen, uint32_t off,
                                   uintptr_t target, bool got_load) {
    if ((size_t)off + 4 > buflen)
        return -1;
    uint32_t imm = (uint32_t)(target & 0xFFFu);
    if (got_load) {
        if ((imm & 0x7u) != 0)
            return -1;
        imm >>= 3;
    }
    uint32_t insn = read_u32(buf, off);
    insn &= ~(0xFFFu << 10);
    insn |= ((imm & 0xFFFu) << 10);
    return write_u32(buf, buflen, off, insn);
}

static void *alloc_got_slot(lr_jit_t *j, void *target_addr) {
    size_t off = align_up(j->data_size, sizeof(void *));
    if (off + sizeof(void *) > j->data_cap)
        return NULL;
    void *slot = j->data_buf + off;
    memcpy(slot, &target_addr, sizeof(target_addr));
    j->data_size = off + sizeof(void *);
    return slot;
}

static int apply_jit_relocs(lr_jit_t *j, const lr_objfile_ctx_t *ctx,
                            uint32_t reloc_start,
                            const char **missing_symbol) {
    if (!j || !ctx)
        return -1;
    if (reloc_start > ctx->num_relocs)
        return -1;
    void **got_slots = NULL;
    void **resolved_targets = NULL;
    uint8_t *resolved_mask = NULL;
    if (ctx->num_symbols > 0) {
        got_slots = (void **)calloc(ctx->num_symbols, sizeof(void *));
        resolved_targets = (void **)calloc(ctx->num_symbols, sizeof(void *));
        resolved_mask = (uint8_t *)calloc(ctx->num_symbols, sizeof(uint8_t));
        if (!got_slots || !resolved_targets || !resolved_mask) {
            free(got_slots);
            free(resolved_targets);
            free(resolved_mask);
            return -1;
        }
    }

    int rc = 0;
    for (uint32_t i = reloc_start; i < ctx->num_relocs; i++) {
        const lr_obj_reloc_t *rel = &ctx->relocs[i];
        if (rel->symbol_idx >= ctx->num_symbols) {
            rc = -1;
            break;
        }

        const lr_obj_symbol_t *sym = &ctx->symbols[rel->symbol_idx];
        const char *sym_name = sym->name;
        void *target_addr = resolved_targets ? resolved_targets[rel->symbol_idx] : NULL;
        if (!resolved_mask || !resolved_mask[rel->symbol_idx]) {
            if (sym->is_defined && sym->section == 1) {
                if ((size_t)sym->offset >= j->code_size) {
                    rc = -1;
                    break;
                }
                target_addr = (void *)(j->code_buf + sym->offset);
            } else {
                if (!sym_name || !sym_name[0]) {
                    rc = -1;
                    break;
                }
                target_addr = lookup_symbol_hashed(j, sym_name, sym->hash);
            }
            if (resolved_targets)
                resolved_targets[rel->symbol_idx] = target_addr;
            if (resolved_mask)
                resolved_mask[rel->symbol_idx] = 1;
        }
        if (!target_addr) {
            if (missing_symbol)
                *missing_symbol = sym_name;
            rc = -1;
            break;
        }

        uintptr_t patch_target = (uintptr_t)target_addr;
        if (rel->type == LR_RELOC_X86_64_GOTPCREL ||
            rel->type == LR_RELOC_ARM64_GOT_LOAD_PAGE21 ||
            rel->type == LR_RELOC_ARM64_GOT_LOAD_PAGEOFF12) {
            if (!got_slots || rel->symbol_idx >= ctx->num_symbols) {
                rc = -1;
                break;
            }
            if (!got_slots[rel->symbol_idx]) {
                got_slots[rel->symbol_idx] = alloc_got_slot(j, target_addr);
                if (!got_slots[rel->symbol_idx]) {
                    rc = -1;
                    break;
                }
            } else {
                memcpy(got_slots[rel->symbol_idx], &target_addr, sizeof(target_addr));
            }
            patch_target = (uintptr_t)got_slots[rel->symbol_idx];
        }

        switch (rel->type) {
        case LR_RELOC_X86_64_PC32:
        case LR_RELOC_X86_64_PLT32:
        case LR_RELOC_X86_64_GOTPCREL:
            rc = patch_x86_rel32(j->code_buf, j->code_size, rel->offset, patch_target);
            break;
        case LR_RELOC_X86_64_64:
            rc = write_u64(j->code_buf, j->code_size, rel->offset, (uint64_t)patch_target);
            break;
        case LR_RELOC_ARM64_BRANCH26:
            rc = patch_aarch64_branch26(j->code_buf, j->code_size, rel->offset, patch_target);
            break;
        case LR_RELOC_ARM64_PAGE21:
        case LR_RELOC_ARM64_GOT_LOAD_PAGE21:
            rc = patch_aarch64_page21(j->code_buf, j->code_size, rel->offset, patch_target);
            break;
        case LR_RELOC_ARM64_PAGEOFF12:
            rc = patch_aarch64_pageoff12(j->code_buf, j->code_size, rel->offset,
                                         patch_target, false);
            break;
        case LR_RELOC_ARM64_GOT_LOAD_PAGEOFF12:
            rc = patch_aarch64_pageoff12(j->code_buf, j->code_size, rel->offset,
                                         patch_target, true);
            break;
        default:
            rc = -1;
            break;
        }
        if (rc != 0)
            break;
        if (j->update_active) {
            j->update_dirty = true;
            if ((size_t)rel->offset < j->update_begin_code_size)
                j->update_begin_code_size = rel->offset;
        }
    }

    free(got_slots);
    free(resolved_targets);
    free(resolved_mask);
    return rc;
}

static int capture_function_relocs(const lr_objfile_ctx_t *fixup_ctx, uint32_t reloc_base,
                                   uint32_t code_base, lr_cached_reloc_t **out_relocs,
                                   uint32_t *out_num_relocs) {
    if (!fixup_ctx || !out_relocs || !out_num_relocs)
        return -1;
    *out_relocs = NULL;
    *out_num_relocs = 0;

    if (fixup_ctx->num_relocs < reloc_base)
        return -1;
    uint32_t num = fixup_ctx->num_relocs - reloc_base;
    if (num == 0)
        return 0;

    lr_cached_reloc_t *cached = (lr_cached_reloc_t *)calloc(num, sizeof(*cached));
    if (!cached)
        return -1;

    for (uint32_t i = 0; i < num; i++) {
        const lr_obj_reloc_t *rel = &fixup_ctx->relocs[reloc_base + i];
        if (rel->symbol_idx >= fixup_ctx->num_symbols) {
            free(cached);
            return -1;
        }
        if (rel->offset < code_base) {
            free(cached);
            return -1;
        }
        const char *sym_name = fixup_ctx->symbols[rel->symbol_idx].name;
        if (!sym_name || !sym_name[0]) {
            free(cached);
            return -1;
        }

        cached[i].offset = rel->offset - code_base;
        cached[i].type = rel->type;
        cached[i].symbol_name = sym_name;
    }

    *out_relocs = cached;
    *out_num_relocs = num;
    return 0;
}

static int replay_cached_function(lr_jit_t *j, lr_objfile_ctx_t *fixup_ctx,
                                  const lr_lazy_func_entry_t *entry,
                                  const lr_mat_cache_entry_t *cached_entry,
                                  void **func_addr_out) {
    if (!j || !fixup_ctx || !entry || !entry->name || !cached_entry || !cached_entry->code)
        return -1;
    if (cached_entry->code_len == 0)
        return -1;

    size_t free_space = j->code_cap - j->code_size;
    if (cached_entry->code_len > free_space)
        return -1;

    uint32_t code_base = (uint32_t)j->code_size;
    uint8_t *func_start = j->code_buf + j->code_size;
    uint32_t sym_idx = lr_obj_ensure_symbol(fixup_ctx, entry->name, true, 1, code_base);
    if (sym_idx == UINT32_MAX)
        return -1;

    memcpy(func_start, cached_entry->code, cached_entry->code_len);

    for (uint32_t i = 0; i < cached_entry->num_relocs; i++) {
        const lr_cached_reloc_t *rel = &cached_entry->relocs[i];
        if (!rel->symbol_name || !rel->symbol_name[0])
            return -1;
        if (rel->offset >= cached_entry->code_len)
            return -1;
        uint32_t reloc_sym = lr_obj_ensure_symbol(fixup_ctx, rel->symbol_name, false, 0, 0);
        if (reloc_sym == UINT32_MAX)
            return -1;
        lr_obj_add_reloc(fixup_ctx, code_base + rel->offset, reloc_sym, rel->type);
    }

    if (func_addr_out)
        *func_addr_out = func_start;
    j->code_size += cached_entry->code_len;
    return 0;
}

static int compile_one_function(lr_jit_t *j, lr_module_t *m, lr_func_t *f,
                                lr_objfile_ctx_t *fixup_ctx, void **func_addr_out) {
    uint8_t *func_start = j->code_buf + j->code_size;
    size_t free_space = j->code_cap - j->code_size;
    uint32_t reloc_base = fixup_ctx->num_relocs;
    if (f->name && f->name[0]) {
        uint32_t sym_idx = lr_obj_ensure_symbol(fixup_ctx, f->name, true, 1,
                                                (uint32_t)j->code_size);
        if (sym_idx == UINT32_MAX)
            return -1;
    }
    size_t code_len = 0;
    JIT_PROF_START(compile);
    int rc;
    if (j->mode == LR_COMPILE_LLVM) {
        rc = -1; /* per-function streaming unsupported in LLVM mode */
    } else {
        rc = lr_target_compile(j->target, j->mode, f, m, func_start,
                               free_space, &code_len, j->arena);
    }
    JIT_PROF_END(compile);
    if (rc != 0)
        return rc;
    if (code_len > free_space)
        return -1;

    for (uint32_t ri = reloc_base; ri < fixup_ctx->num_relocs; ri++)
        fixup_ctx->relocs[ri].offset += (uint32_t)j->code_size;

    if (func_addr_out)
        *func_addr_out = func_start;
    j->code_size += code_len;
    return 0;
}

static uint32_t materialize_prefetch_thread_count(uint32_t pending_count) {
#if !LR_HAS_PTHREADS
    (void)pending_count;
    return 1;
#else
    if (pending_count < MATERIALIZE_PREFETCH_MIN_PENDING)
        return 1;

    uint32_t max_threads = MATERIALIZE_PREFETCH_MAX_THREADS;
    if (pending_count < max_threads)
        max_threads = pending_count;

    const char *env = getenv("LIRIC_JIT_MAT_THREADS");
    if (!env || !env[0])
        return 1;

    char *end = NULL;
    long parsed = strtol(env, &end, 10);
    if (end == env || *end != '\0' || parsed <= 1)
        return 1;

    uint32_t requested = (uint32_t)parsed;
    if (requested > max_threads)
        requested = max_threads;
    if (requested < 2)
        return 1;
    return requested;
#endif
}

static bool jit_lazy_materialization_enabled(void) {
    const char *env = getenv("LIRIC_JIT_LAZY");
    if (!env || !env[0])
        return false;

    if (strcmp(env, "0") == 0 ||
        strcmp(env, "false") == 0 || strcmp(env, "FALSE") == 0 ||
        strcmp(env, "off") == 0 || strcmp(env, "OFF") == 0 ||
        strcmp(env, "no") == 0 || strcmp(env, "NO") == 0) {
        return false;
    }

    return true;
}

#if LR_HAS_PTHREADS
static void *materialize_prefetch_worker_main(void *arg) {
    lr_materialize_prefetch_worker_t *w = (lr_materialize_prefetch_worker_t *)arg;
    if (!w || !w->target || !w->tasks)
        return NULL;

    for (uint32_t i = w->begin; i < w->end; i++) {
        lr_materialize_prefetch_task_t *task = &w->tasks[i];
        task->rc = -1;
        task->code = NULL;
        task->code_len = 0;
        task->relocs = NULL;
        task->num_relocs = 0;

        if (!task->entry || !task->entry->module || !task->entry->func)
            continue;

        lr_arena_t *worker_arena = lr_arena_create(64 * 1024);
        if (!worker_arena)
            continue;

        uint8_t *scratch_buf = (uint8_t *)malloc(w->code_cap);
        if (!scratch_buf) {
            lr_arena_destroy(worker_arena);
            continue;
        }

        lr_objfile_ctx_t fixup_ctx;
        memset(&fixup_ctx, 0, sizeof(fixup_ctx));
        fixup_ctx.preserve_symbol_names = true;
        if (jit_build_module_symbol_cache(&fixup_ctx, task->entry->module) != 0)
            goto worker_done;

        lr_module_t module_view = *task->entry->module;
        module_view.obj_ctx = &fixup_ctx;

        lr_jit_t worker_jit;
        memset(&worker_jit, 0, sizeof(worker_jit));
        worker_jit.target = w->target;
        worker_jit.mode = w->mode;
        worker_jit.code_buf = scratch_buf;
        worker_jit.code_cap = w->code_cap;
        worker_jit.arena = worker_arena;

        if (compile_one_function(&worker_jit, &module_view, task->entry->func,
                                 &fixup_ctx, NULL) != 0)
            goto worker_done;
        if (worker_jit.code_size == 0)
            goto worker_done;

        task->code = (uint8_t *)malloc(worker_jit.code_size);
        if (!task->code)
            goto worker_done;
        memcpy(task->code, scratch_buf, worker_jit.code_size);
        task->code_len = worker_jit.code_size;

        if (capture_function_relocs(&fixup_ctx, 0, 0, &task->relocs, &task->num_relocs) != 0)
            goto worker_done;

        task->rc = 0;

worker_done:
        if (task->rc != 0) {
            free(task->code);
            task->code = NULL;
            task->code_len = 0;
            free(task->relocs);
            task->relocs = NULL;
            task->num_relocs = 0;
        }
        jit_free_obj_ctx(&fixup_ctx);
        free(scratch_buf);
        lr_arena_destroy(worker_arena);
    }
    return NULL;
}
#endif

static bool materialize_prefetch_has_task(const lr_materialize_prefetch_task_t *tasks,
                                          uint32_t count,
                                          const lr_lazy_func_entry_t *entry) {
    if (!tasks || !entry)
        return false;
    for (uint32_t i = 0; i < count; i++) {
        if (tasks[i].entry == entry)
            return true;
    }
    return false;
}

static uint32_t materialize_prefetch_maybe_add_target(lr_jit_t *j,
                                                      lr_module_t *root_module,
                                                      const char *sym_name,
                                                      lr_materialize_prefetch_task_t *tasks,
                                                      uint32_t count,
                                                      uint32_t task_cap) {
    if (!j || !root_module || !sym_name || !sym_name[0] || !tasks || count >= task_cap)
        return count;

    uint32_t hash = symbol_hash(sym_name);
    lr_lazy_func_entry_t *entry = find_lazy_func_entry(j, sym_name, hash);
    if (!entry || entry->state != LR_LAZY_FUNC_PENDING)
        return count;
    if (entry->module != root_module)
        return count;
    if (!entry->module_sig || entry->module_sig_len == 0 ||
        !entry->func_sig || entry->func_sig_len == 0)
        return count;
    if (materialize_cache_lookup(j->target, entry->module_sig, entry->module_sig_len,
                                 entry->func_sig, entry->func_sig_len, false))
        return count;
    if (materialize_prefetch_has_task(tasks, count, entry))
        return count;

    tasks[count].entry = entry;
    return count + 1u;
}

static uint32_t materialize_prefetch_collect_from_function(lr_jit_t *j,
                                                           lr_module_t *root_module,
                                                           const lr_lazy_func_entry_t *source,
                                                           lr_materialize_prefetch_task_t *tasks,
                                                           uint32_t count,
                                                           uint32_t task_cap) {
    if (!j || !root_module || !source || !source->module || !source->func ||
        !tasks || count >= task_cap)
        return count;

    lr_module_t *m = source->module;
    lr_func_t *f = source->func;

    if (f->block_array && f->num_blocks > 0) {
        for (uint32_t bi = 0; bi < f->num_blocks && count < task_cap; bi++) {
            const lr_block_t *b = f->block_array[bi];
            if (!b)
                continue;

            if (b->inst_array && b->num_insts > 0) {
                for (uint32_t ii = 0; ii < b->num_insts && count < task_cap; ii++) {
                    const lr_inst_t *inst = b->inst_array[ii];
                    if (!inst || inst->op != LR_OP_CALL || inst->num_operands == 0)
                        continue;
                    const lr_operand_t *callee = &inst->operands[0];
                    if (callee->kind != LR_VAL_GLOBAL)
                        continue;
                    const char *sym_name = lr_module_symbol_name(m, callee->global_id);
                    count = materialize_prefetch_maybe_add_target(j, root_module, sym_name,
                                                                   tasks, count, task_cap);
                }
                continue;
            }

            for (const lr_inst_t *inst = b->first; inst && count < task_cap; inst = inst->next) {
                if (inst->op != LR_OP_CALL || inst->num_operands == 0)
                    continue;
                const lr_operand_t *callee = &inst->operands[0];
                if (callee->kind != LR_VAL_GLOBAL)
                    continue;
                const char *sym_name = lr_module_symbol_name(m, callee->global_id);
                count = materialize_prefetch_maybe_add_target(j, root_module, sym_name,
                                                               tasks, count, task_cap);
            }
        }
        return count;
    }

    for (const lr_block_t *b = f->first_block; b && count < task_cap; b = b->next) {
        for (const lr_inst_t *inst = b->first; inst && count < task_cap; inst = inst->next) {
            if (inst->op != LR_OP_CALL || inst->num_operands == 0)
                continue;
            const lr_operand_t *callee = &inst->operands[0];
            if (callee->kind != LR_VAL_GLOBAL)
                continue;
            const char *sym_name = lr_module_symbol_name(m, callee->global_id);
            count = materialize_prefetch_maybe_add_target(j, root_module, sym_name,
                                                           tasks, count, task_cap);
        }
    }
    return count;
}

static uint32_t materialize_prefetch_collect_targets(lr_jit_t *j,
                                                     lr_materialize_prefetch_task_t *tasks,
                                                     uint32_t task_cap) {
    if (!j || !tasks || task_cap == 0 || !tasks[0].entry || !tasks[0].entry->module)
        return 0;

    lr_module_t *root_module = tasks[0].entry->module;
    uint32_t count = 1u;

    /* Walk the reachable call graph so deep dependency chains can be prefetched once. */
    for (uint32_t scan = 0; scan < count && count < task_cap; scan++) {
        const lr_lazy_func_entry_t *source = tasks[scan].entry;
        count = materialize_prefetch_collect_from_function(j, root_module, source,
                                                            tasks, count, task_cap);
    }

    return count;
}

static bool materialize_prefetch_finalize_targets(lr_jit_t *j,
                                                  lr_materialize_prefetch_task_t *tasks,
                                                  uint32_t pending) {
    if (!j || !tasks || pending == 0)
        return false;

    for (uint32_t i = 0; i < pending; i++) {
        lr_lazy_func_entry_t *entry = tasks[i].entry;
        if (!entry || !entry->func || lr_func_is_finalized(entry->func))
            continue;

        lr_arena_t *layout_arena =
            (entry->module && entry->module->arena) ? entry->module->arena : j->arena;
        if (!layout_arena)
            return false;
        if (lr_func_finalize(entry->func, layout_arena) != 0)
            return false;
    }

    return true;
}

static bool materialize_prefetch_module_functions(lr_jit_t *j,
                                                  const lr_lazy_func_entry_t *trigger,
                                                  lr_materialize_prefetch_task_t *out_trigger_task) {
    /*
     * Thread-safety model:
     * - Worker threads compile into private arenas/code buffers/fixup contexts.
     * - Shared JIT state (code buffer, symbol table, relocation patching) stays single-threaded.
     * - Cache insertion is serialized in module order for deterministic replay behavior.
     */
    if (out_trigger_task)
        memset(out_trigger_task, 0, sizeof(*out_trigger_task));
    if (!j || !j->target || !trigger || !trigger->module)
        return false;

    uint32_t task_cap = 1; /* trigger + reachable callees */
    for (lr_func_t *f = trigger->module->first_func; f; f = f->next) {
        if (!f->name || !f->name[0] || f->is_decl)
            continue;
        if (strcmp(f->name, trigger->name) == 0)
            continue;
        task_cap++;
    }
    if (task_cap < MATERIALIZE_PREFETCH_MIN_PENDING)
        return false;

    lr_materialize_prefetch_task_t *tasks =
        (lr_materialize_prefetch_task_t *)calloc(task_cap, sizeof(*tasks));
    if (!tasks)
        return false;

    tasks[0].entry = (lr_lazy_func_entry_t *)trigger;
    uint32_t pending = materialize_prefetch_collect_targets(j, tasks, task_cap);
    if (pending < MATERIALIZE_PREFETCH_MIN_PENDING) {
        free(tasks);
        return false;
    }

    uint32_t nthreads = materialize_prefetch_thread_count(pending);
    if (nthreads <= 1) {
        free(tasks);
        return false;
    }
    if (nthreads > pending)
        nthreads = pending;
    /* Finalize once on the caller thread to avoid concurrent arena mutation. */
    if (!materialize_prefetch_finalize_targets(j, tasks, pending)) {
        free(tasks);
        return false;
    }

#if LR_HAS_PTHREADS
    pthread_t *threads = (pthread_t *)calloc(nthreads, sizeof(*threads));
    lr_materialize_prefetch_worker_t *workers =
        (lr_materialize_prefetch_worker_t *)calloc(nthreads, sizeof(*workers));
    uint8_t *started = (uint8_t *)calloc(nthreads, sizeof(*started));
    if (!threads || !workers || !started) {
        free(started);
        free(workers);
        free(threads);
        free(tasks);
        return false;
    }

    uint32_t chunk = (pending + nthreads - 1u) / nthreads;
    for (uint32_t wi = 0; wi < nthreads; wi++) {
        uint32_t begin = wi * chunk;
        uint32_t end = begin + chunk;
        if (begin >= pending)
            break;
        if (end > pending)
            end = pending;

        workers[wi].target = j->target;
        workers[wi].mode = j->mode;
        workers[wi].code_cap = j->code_cap;
        workers[wi].tasks = tasks;
        workers[wi].begin = begin;
        workers[wi].end = end;

        if (pthread_create(&threads[wi], NULL,
                           materialize_prefetch_worker_main, &workers[wi]) == 0) {
            started[wi] = 1u;
        } else {
            (void)materialize_prefetch_worker_main(&workers[wi]);
        }
    }

    for (uint32_t wi = 0; wi < nthreads; wi++) {
        if (started[wi])
            (void)pthread_join(threads[wi], NULL);
    }

    free(started);
    free(workers);
    free(threads);
#endif

    bool have_trigger = false;
    if (out_trigger_task &&
        tasks[0].rc == 0 &&
        tasks[0].code && tasks[0].code_len > 0) {
        *out_trigger_task = tasks[0];
        memset(&tasks[0], 0, sizeof(tasks[0]));
        have_trigger = true;
    }

    for (uint32_t i = 1; i < pending; i++) {
        lr_materialize_prefetch_task_t *task = &tasks[i];
        if (task->rc == 0 && task->entry && task->code && task->code_len > 0) {
            (void)materialize_cache_insert(j->target,
                                           task->entry->module_sig, task->entry->module_sig_len,
                                           task->entry->func_sig, task->entry->func_sig_len,
                                           task->code, task->code_len,
                                           task->relocs, task->num_relocs);
        }
        free(task->code);
        task->code = NULL;
        free(task->relocs);
        task->relocs = NULL;
    }
    free(tasks[0].code);
    tasks[0].code = NULL;
    free(tasks[0].relocs);
    tasks[0].relocs = NULL;
    free(tasks);
    return have_trigger;
}

static int register_lazy_module_functions(lr_jit_t *j, lr_module_t *m,
                                          lr_func_t **funcs, uint32_t nfuncs) {
    if (!j || !m)
        return -1;

    if (jit_ensure_module_symbols_interned(m) != 0)
        return -1;

    uint8_t *module_sig_heap = NULL;
    size_t module_sig_len = 0;
    if (build_module_signature(m, &module_sig_heap, &module_sig_len) != 0)
        return -1;

    const uint8_t *module_sig = arena_dup_bytes(j->arena, module_sig_heap, module_sig_len);
    free(module_sig_heap);
    if (module_sig_len > 0 && !module_sig)
        return -1;

    for (uint32_t i = 0; i < nfuncs; i++) {
        lr_func_t *f = funcs[i];
        if (!f || !f->name || !f->name[0])
            continue;
        uint8_t *func_sig_heap = NULL;
        size_t func_sig_len = 0;
        if (build_function_signature(m, f, &func_sig_heap, &func_sig_len) != 0)
            return -1;
        const uint8_t *func_sig = arena_dup_bytes(j->arena, func_sig_heap, func_sig_len);
        free(func_sig_heap);
        if (func_sig_len > 0 && !func_sig)
            return -1;

        if (!upsert_lazy_func_entry(j, m, f, module_sig, module_sig_len,
                                    func_sig, func_sig_len))
            return -1;
        uint32_t hash = symbol_hash(f->name);
        if (!find_symbol_entry(j, f->name, hash))
            miss_cache_add(j, f->name, hash);
    }
    return 0;
}

static int materialize_lazy_function(lr_jit_t *j, lr_lazy_func_entry_t *entry) {
    if (!j || !entry || !entry->module || !entry->func)
        return -1;
    if (entry->state == LR_LAZY_FUNC_READY)
        return 0;
    if (entry->state == LR_LAZY_FUNC_COMPILING)
        return 0;

    bool top_level_materialize = (j->materialize_depth == 0);
    bool own_wx_transition = top_level_materialize && !j->update_active;
    size_t code_size_before = j->code_size;
    int rc = -1;
    lr_objfile_ctx_t fixup_ctx;
    memset(&fixup_ctx, 0, sizeof(fixup_ctx));
    bool fixup_ctx_ready = false;
    void *saved_obj_ctx = NULL;
    bool obj_ctx_installed = false;
    bool compiled_from_scratch = false;
    uint32_t compiled_reloc_base = 0;
    uint32_t compiled_code_base = 0;
    size_t compiled_code_len = 0;
    uint8_t *compiled_code_copy = NULL;
    lr_cached_reloc_t *compiled_relocs = NULL;
    uint32_t compiled_num_relocs = 0;
    lr_materialize_prefetch_task_t prefetched_self;
    memset(&prefetched_self, 0, sizeof(prefetched_self));

    j->materialize_depth++;

    if (own_wx_transition) {
        JIT_PROF_START(make_writable);
        if (make_writable(j) != 0) {
            j->materialize_depth--;
            return -1;
        }
        JIT_PROF_END(make_writable);
    }

    fixup_ctx.preserve_symbol_names = true;
    if (jit_build_module_symbol_cache(&fixup_ctx, entry->module) != 0)
        goto done;
    fixup_ctx_ready = true;

    saved_obj_ctx = entry->module->obj_ctx;
    entry->module->obj_ctx = &fixup_ctx;
    obj_ctx_installed = true;

    entry->state = LR_LAZY_FUNC_COMPILING;
    entry->pending_addr = NULL;

    void *func_addr = NULL;
    bool record_cache_stats = (j->materialize_depth == 1u);
    const lr_mat_cache_entry_t *cached_entry =
        materialize_cache_lookup(j->target, entry->module_sig, entry->module_sig_len,
                                 entry->func_sig, entry->func_sig_len,
                                 record_cache_stats);

    if (cached_entry) {
        JIT_PROF_START(compile_loop);
        int replay_rc = replay_cached_function(j, &fixup_ctx, entry, cached_entry, &func_addr);
        JIT_PROF_END(compile_loop);
        if (replay_rc != 0) {
            rc = -1;
            goto done;
        }
    } else {
        compiled_from_scratch = true;
        bool used_prefetched_self = false;
        if (j->materialize_depth == 1u) {
            used_prefetched_self =
                materialize_prefetch_module_functions(j, entry, &prefetched_self);
        }
        if (used_prefetched_self &&
            prefetched_self.code && prefetched_self.code_len > 0) {
            lr_mat_cache_entry_t prefetched_entry;
            memset(&prefetched_entry, 0, sizeof(prefetched_entry));
            prefetched_entry.code = prefetched_self.code;
            prefetched_entry.code_len = prefetched_self.code_len;
            prefetched_entry.relocs = prefetched_self.relocs;
            prefetched_entry.num_relocs = prefetched_self.num_relocs;

            JIT_PROF_START(compile_loop);
            int replay_rc = replay_cached_function(j, &fixup_ctx, entry, &prefetched_entry, &func_addr);
            JIT_PROF_END(compile_loop);
            if (replay_rc == 0) {
                compiled_code_copy = prefetched_self.code;
                compiled_code_len = prefetched_self.code_len;
                compiled_relocs = prefetched_self.relocs;
                compiled_num_relocs = prefetched_self.num_relocs;
                prefetched_self.code = NULL;
                prefetched_self.relocs = NULL;
                used_prefetched_self = true;
            } else {
                used_prefetched_self = false;
            }
        }

        if (!used_prefetched_self) {
            compiled_reloc_base = fixup_ctx.num_relocs;
            compiled_code_base = (uint32_t)j->code_size;

            JIT_PROF_START(compile_loop);
            int func_rc = compile_one_function(j, entry->module, entry->func, &fixup_ctx, &func_addr);
            JIT_PROF_END(compile_loop);
            if (func_rc != 0) {
                rc = func_rc;
                goto done;
            }

            compiled_code_len = j->code_size - (size_t)compiled_code_base;
            if (compiled_code_len == 0) {
                rc = -1;
                goto done;
            }
            compiled_code_copy = (uint8_t *)malloc(compiled_code_len);
            if (!compiled_code_copy) {
                rc = -1;
                goto done;
            }
            memcpy(compiled_code_copy, j->code_buf + compiled_code_base, compiled_code_len);
            if (capture_function_relocs(&fixup_ctx, compiled_reloc_base, compiled_code_base,
                                        &compiled_relocs, &compiled_num_relocs) != 0) {
                rc = -1;
                goto done;
            }
        }
    }
    entry->pending_addr = func_addr;

    while (1) {
        JIT_PROF_START(patch_fixups);
        const char *missing_symbol = NULL;
        if (apply_jit_relocs(j, &fixup_ctx, 0, &missing_symbol) == 0) {
            JIT_PROF_END(patch_fixups);
            break;
        }
        JIT_PROF_END(patch_fixups);

        if (!missing_symbol) {
            rc = -1;
            goto done;
        }

        uint32_t missing_hash = symbol_hash(missing_symbol);
        lr_lazy_func_entry_t *dep = find_lazy_func_entry(j, missing_symbol, missing_hash);
        if (!dep || dep->state == LR_LAZY_FUNC_READY) {
            fprintf(stderr, "unresolved symbol: %s\n", missing_symbol);
            rc = -1;
            goto done;
        }
        if (dep->state == LR_LAZY_FUNC_COMPILING) {
            fprintf(stderr, "unresolved symbol: %s\n", missing_symbol);
            rc = -1;
            goto done;
        }
        if (materialize_lazy_function(j, dep) != 0) {
            rc = -1;
            goto done;
        }
    }

    lr_jit_add_symbol(j, entry->name, entry->pending_addr);
    entry->state = LR_LAZY_FUNC_READY;
    entry->pending_addr = NULL;

    /* Re-apply relocations after function symbols exist. */
    if (apply_module_global_relocs(j, entry->module) != 0)
        goto done;

    if (compiled_from_scratch && compiled_code_copy &&
        entry->module_sig && entry->module_sig_len > 0 &&
        entry->func_sig && entry->func_sig_len > 0) {
        (void)materialize_cache_insert(j->target,
                                       entry->module_sig, entry->module_sig_len,
                                       entry->func_sig, entry->func_sig_len,
                                       compiled_code_copy, compiled_code_len,
                                       compiled_relocs, compiled_num_relocs);
    }

    rc = 0;

done:
    if (j->materialize_depth > 0)
        j->materialize_depth--;
    if (rc != 0) {
        entry->state = LR_LAZY_FUNC_PENDING;
        entry->pending_addr = NULL;
    }
    if (obj_ctx_installed) {
        entry->module->obj_ctx = saved_obj_ctx;
        obj_ctx_installed = false;
    }
    if (fixup_ctx_ready) {
        jit_free_obj_ctx(&fixup_ctx);
        fixup_ctx_ready = false;
    }
    free(compiled_code_copy);
    compiled_code_copy = NULL;
    free(compiled_relocs);
    compiled_relocs = NULL;
    free(prefetched_self.code);
    prefetched_self.code = NULL;
    free(prefetched_self.relocs);
    prefetched_self.relocs = NULL;
    if (j->update_active && j->code_size > code_size_before)
        j->update_dirty = true;
    if (own_wx_transition) {
        JIT_PROF_START(make_exec);
        if (make_executable_from(j, code_size_before) != 0)
            rc = -1;
        JIT_PROF_END(make_exec);
    }
    return rc;
}

int lr_jit_add_module(lr_jit_t *j, lr_module_t *m) {
    if (!j || !j->target || !m) return -1;

    if (j->mode == LR_COMPILE_LLVM) {
        char llvm_err[256] = {0};
        int llvm_rc = lr_llvm_jit_add_module(j, m, llvm_err, sizeof(llvm_err));
        if (llvm_rc != 0 && llvm_err[0]) {
            fprintf(stderr, "llvm mode jit failed: %s\n", llvm_err);
        }
        return llvm_rc;
    }

    bool own_wx_transition = !j->update_active;
    bool lazy_mode = jit_lazy_materialization_enabled();
    int rc = -1;
    size_t code_size_before = j->code_size;
    lr_objfile_ctx_t fixup_ctx;
    memset(&fixup_ctx, 0, sizeof(fixup_ctx));
    bool fixup_ctx_ready = false;
    void *saved_obj_ctx = NULL;
    bool obj_ctx_installed = false;

    if (own_wx_transition) {
        JIT_PROF_START(make_writable);
        if (make_writable(j) != 0) return -1;
        JIT_PROF_END(make_writable);
    }

    JIT_PROF_START(globals);
    if (lr_jit_materialize_globals(j, m) != 0) goto done;
    JIT_PROF_END(globals);

    uint32_t nfuncs = 0;
    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (!f->is_decl)
            nfuncs++;
    }

    if (nfuncs == 0) {
        if (apply_module_global_relocs(j, m) != 0)
            goto done;
        rc = 0;
        goto done;
    }

    /* Pre-populate miss cache for all module-defined function names.
       This prevents dlsym from being called when resolving intra-module
       cross-references — the miss cache hit is O(1) vs dlsym at ~5 us. */
    JIT_PROF_START(pre_register);
    lr_func_t **funcs = lr_arena_array(j->arena, lr_func_t *, nfuncs);
    uint32_t fi = 0;
    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (!f->is_decl)
            funcs[fi++] = f;
    }
    if (lazy_mode) {
        if (register_lazy_module_functions(j, m, funcs, nfuncs) != 0)
            goto done;
    } else {
        for (uint32_t i = 0; i < nfuncs; i++) {
            const char *name = funcs[i]->name;
            if (name && name[0]) {
                uint32_t hash = symbol_hash(name);
                if (!find_symbol_entry(j, name, hash))
                    if (!miss_cache_contains(j, name, hash))
                        miss_cache_add(j, name, hash);
            }
        }
    }

    /* Resolve function declarations eagerly — these are external symbols
       that will be needed during compilation (e.g., runtime functions). */
    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (f->is_decl && f->name && f->name[0])
            lookup_symbol(j, f->name);
    }
    JIT_PROF_END(pre_register);

    if (lazy_mode) {
        /* In lazy mode, globals may reference functions not materialized yet. */
        if (apply_module_global_relocs(j, m) != 0)
            goto done;
        rc = 0;
        goto done;
    }

    fixup_ctx.preserve_symbol_names = true;
    if (jit_build_module_symbol_cache(&fixup_ctx, m) != 0)
        goto done;
    fixup_ctx_ready = true;
    saved_obj_ctx = m->obj_ctx;
    m->obj_ctx = &fixup_ctx;
    obj_ctx_installed = true;

    JIT_PROF_START(compile_loop);
    void **func_addrs = lr_arena_array(j->arena, void *, nfuncs);
    if (!func_addrs)
        goto done;
    for (uint32_t i = 0; i < nfuncs; i++) {
        int func_rc = compile_one_function(j, m, funcs[i], &fixup_ctx, &func_addrs[i]);
        if (func_rc != 0) {
            rc = func_rc;
            goto done;
        }
    }
    JIT_PROF_END(compile_loop);

    JIT_PROF_START(patch_fixups);
    const char *missing_symbol = NULL;
    if (apply_jit_relocs(j, &fixup_ctx, 0, &missing_symbol) != 0) {
        JIT_PROF_END(patch_fixups);
        if (missing_symbol)
            fprintf(stderr, "unresolved symbol: %s\n", missing_symbol);
        goto done;
    }
    JIT_PROF_END(patch_fixups);

    for (uint32_t i = 0; i < nfuncs; i++) {
        if (funcs[i]->name && funcs[i]->name[0] && func_addrs[i])
            lr_jit_add_symbol(j, funcs[i]->name, func_addrs[i]);
    }

    /* Re-apply relocations after module-defined function symbols exist. */
    if (apply_module_global_relocs(j, m) != 0)
        goto done;
    rc = 0;

done:
    if (obj_ctx_installed) {
        m->obj_ctx = saved_obj_ctx;
        obj_ctx_installed = false;
    }
    if (fixup_ctx_ready) {
        jit_free_obj_ctx(&fixup_ctx);
        fixup_ctx_ready = false;
    }
    if (j->update_active && j->code_size > code_size_before)
        j->update_dirty = true;
    if (own_wx_transition) {
        JIT_PROF_START(make_exec);
        if (make_executable(j) != 0)
            rc = -1;
        JIT_PROF_END(make_exec);
    }
    return rc;
}

void lr_jit_begin_update(lr_jit_t *j) {
    if (!j)
        return;
    if (make_writable(j) != 0)
        return;
    if (!j->update_active) {
        j->update_active = true;
        j->update_dirty = false;
        j->update_begin_code_size = j->code_size;
    }
}

void lr_jit_end_update(lr_jit_t *j) {
    if (!j || !j->update_active)
        return;
    size_t clear_from = j->update_dirty ? j->update_begin_code_size : j->code_size;
    (void)make_executable_from(j, clear_from);
    j->update_active = false;
    j->update_dirty = false;
    j->update_begin_code_size = j->code_size;
}

void *lr_jit_get_function(lr_jit_t *j, const char *name) {
    if (!j || !name || !name[0])
        return NULL;
    uint32_t hash = symbol_hash(name);
    void *addr = lookup_symbol_hashed(j, name, hash);
    if (addr || !jit_lazy_materialization_enabled())
        return addr;

    /* Materialize only when the fast path misses a lazy pending entry. */
    lr_lazy_func_entry_t *lazy = find_lazy_func_entry(j, name, hash);
    if (lazy && lazy->state != LR_LAZY_FUNC_READY) {
        if (materialize_lazy_function(j, lazy) != 0)
            return NULL;
    }
    return lookup_symbol_hashed(j, name, hash);
}

int lr_jit_patch_relocs(lr_jit_t *j, const struct lr_objfile_ctx *ctx) {
    return apply_jit_relocs(j, ctx, 0, NULL);
}

int lr_jit_patch_relocs_from(lr_jit_t *j, const struct lr_objfile_ctx *ctx,
                             uint32_t reloc_start) {
    return apply_jit_relocs(j, ctx, reloc_start, NULL);
}

int lr_jit_patch_relocs_from_ex(lr_jit_t *j, const struct lr_objfile_ctx *ctx,
                                uint32_t reloc_start,
                                const char **missing_symbol) {
    if (missing_symbol)
        *missing_symbol = NULL;
    return apply_jit_relocs(j, ctx, reloc_start, missing_symbol);
}

void lr_jit_destroy(lr_jit_t *j) {
    if (!j) return;
    lr_llvm_jit_dispose(j);
    for (lr_lib_entry_t *l = j->libs; l; l = l->next) {
        if (l->handle)
            (void)lr_platform_dlclose(l->handle);
    }
    if (j->code_buf)
        (void)lr_platform_free_pages(j->code_buf, j->code_cap);
    if (j->data_buf)
        (void)lr_platform_free_pages(j->data_buf, j->data_cap);
    free(j->lazy_func_buckets);
    free(j->miss_buckets);
    free(j->sym_buckets);
    lr_arena_destroy(j->arena);
    free(j);
}
