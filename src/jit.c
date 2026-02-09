#include "jit.h"
#include "ir.h"
#include "target.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <math.h>

#ifdef LR_JIT_PROFILE
#ifdef __APPLE__
#include <mach/mach_time.h>
static double lr_jit_now_us(void) {
    static mach_timebase_info_data_t info = {0, 0};
    if (info.denom == 0) mach_timebase_info(&info);
    uint64_t t = mach_absolute_time();
    return (double)(t * info.numer / info.denom) / 1e3;
}
#else
#include <time.h>
static double lr_jit_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}
#endif
#define JIT_PROF_START(name) double _prof_##name = lr_jit_now_us()
#define JIT_PROF_END(name) fprintf(stderr, "  jit-prof %-20s %7.2f us\n", #name, lr_jit_now_us() - _prof_##name)
#else
#define JIT_PROF_START(name) ((void)0)
#define JIT_PROF_END(name) ((void)0)
#endif

#if defined(__APPLE__) && defined(__aarch64__) && defined(MAP_JIT)
#include <pthread.h>
#define LR_CAN_USE_MAP_JIT 1
#else
#define LR_CAN_USE_MAP_JIT 0
#endif

#define CODE_PAGE_SIZE (1024 * 1024)
#define DATA_PAGE_SIZE (256 * 1024)
#define SYM_BUCKET_COUNT 8192u
#define MISS_BUCKET_COUNT 4096u

static void register_builtin_symbols(lr_jit_t *j);
static const char *resolve_global_name(lr_module_t *m, uint32_t global_id);

static uint32_t symbol_hash(const char *name) {
    uint32_t h = 2166136261u;
    while (*name) {
        h ^= (uint8_t)*name++;
        h *= 16777619u;
    }
    return h;
}

static float llvm_fabs_f32(float x) { return fabsf(x); }
static double llvm_fabs_f64(double x) { return fabs(x); }
static float llvm_sqrt_f32(float x) { return sqrtf(x); }
static float llvm_exp_f32(float x) { return expf(x); }
static float llvm_copysign_f32(float x, float y) { return copysignf(x, y); }
static float llvm_pow_f32(float x, float y) { return powf(x, y); }
static double llvm_sqrt_f64(double x) { return sqrt(x); }
static double llvm_exp_f64(double x) { return exp(x); }
static double llvm_pow_f64(double x, double y) { return pow(x, y); }
static double llvm_copysign_f64(double x, double y) { return copysign(x, y); }
static float llvm_powi_f32(float x, int32_t e) { return powf(x, (float)e); }
static double llvm_powi_f64(double x, int32_t e) { return pow(x, (double)e); }

static void llvm_memset_p0i8_i64(void *dst, uint64_t val, int64_t len, uint64_t is_volatile) {
    (void)is_volatile;
    if (!dst || len <= 0)
        return;
    memset(dst, (int)(uint8_t)val, (size_t)len);
}

static void llvm_memset_p0i8_i32(void *dst, uint64_t val, int32_t len, uint64_t is_volatile) {
    (void)is_volatile;
    if (!dst || len <= 0)
        return;
    memset(dst, (int)(uint8_t)val, (size_t)len);
}

static void llvm_memcpy_p0i8_p0i8_i32(void *dst, const void *src, int32_t len, uint64_t is_volatile) {
    (void)is_volatile;
    if (!dst || !src || len <= 0)
        return;
    memcpy(dst, src, (size_t)len);
}

static void llvm_memcpy_p0i8_p0i8_i64(void *dst, const void *src, int64_t len, uint64_t is_volatile) {
    (void)is_volatile;
    if (!dst || !src || len <= 0)
        return;
    memcpy(dst, src, (size_t)len);
}

static size_t align_up(size_t value, size_t align) {
    if (align == 0)
        return value;
    size_t mask = align - 1;
    return (value + mask) & ~mask;
}

static int make_writable(lr_jit_t *j) {
    if (j->map_jit_enabled) {
#if LR_CAN_USE_MAP_JIT
        pthread_jit_write_protect_np(0);
        return 0;
#else
        return -1;
#endif
    }
    return mprotect(j->code_buf, j->code_cap, PROT_READ | PROT_WRITE);
}

static int make_executable(lr_jit_t *j) {
    __builtin___clear_cache((char *)j->code_buf, (char *)(j->code_buf + j->code_size));

    if (j->map_jit_enabled) {
#if LR_CAN_USE_MAP_JIT
        pthread_jit_write_protect_np(1);
        return 0;
#else
        return -1;
#endif
    }
    return mprotect(j->code_buf, j->code_cap, PROT_READ | PROT_EXEC);
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
    j->arena = lr_arena_create(0);
    if (!j->arena) {
        free(j);
        return NULL;
    }
    j->sym_bucket_count = SYM_BUCKET_COUNT;
    j->miss_bucket_count = MISS_BUCKET_COUNT;
    j->sym_buckets = calloc(j->sym_bucket_count, sizeof(*j->sym_buckets));
    j->miss_buckets = calloc(j->miss_bucket_count, sizeof(*j->miss_buckets));
    if (!j->sym_buckets || !j->miss_buckets) {
        free(j->miss_buckets);
        free(j->sym_buckets);
        lr_arena_destroy(j->arena);
        free(j);
        return NULL;
    }

    j->code_cap = CODE_PAGE_SIZE;
    int code_prot = PROT_READ | PROT_WRITE;
    int code_flags = MAP_PRIVATE | MAP_ANONYMOUS;

#if LR_CAN_USE_MAP_JIT
    code_prot |= PROT_EXEC;
    code_flags |= MAP_JIT;
#endif

    j->code_buf = mmap(NULL, j->code_cap, code_prot, code_flags, -1, 0);
    if (j->code_buf == MAP_FAILED) {
#if LR_CAN_USE_MAP_JIT
        j->code_buf = mmap(NULL, j->code_cap, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        j->map_jit_enabled = false;
        if (j->code_buf == MAP_FAILED)
#endif
        {
            free(j->miss_buckets);
            free(j->sym_buckets);
            lr_arena_destroy(j->arena);
            free(j);
            return NULL;
        }
    } else {
#if LR_CAN_USE_MAP_JIT
        j->map_jit_enabled = true;
        pthread_jit_write_protect_np(0);
#endif
    }

    j->data_cap = DATA_PAGE_SIZE;
    j->data_buf = mmap(NULL, j->data_cap, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (j->data_buf == MAP_FAILED) {
        munmap(j->code_buf, j->code_cap);
        free(j->miss_buckets);
        free(j->sym_buckets);
        lr_arena_destroy(j->arena);
        free(j);
        return NULL;
    }

    if (make_executable(j) != 0) {
        munmap(j->data_buf, j->data_cap);
        munmap(j->code_buf, j->code_cap);
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
    uint32_t bucket = hash & (j->miss_bucket_count - 1u);
    lr_sym_miss_entry_t *m = lr_arena_new(j->arena, lr_sym_miss_entry_t);
    m->name = lr_arena_strdup(j->arena, name, strlen(name));
    m->hash = hash;
    m->bucket_next = j->miss_buckets[bucket];
    j->miss_buckets[bucket] = m;
}

void lr_jit_add_symbol(lr_jit_t *j, const char *name, void *addr) {
    if (!j || !name || !name[0])
        return;
    uint32_t hash = symbol_hash(name);
    lr_sym_entry_t *existing = find_symbol_entry(j, name, hash);
    if (existing) {
        existing->addr = addr;
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
}

static void register_builtin_symbols(lr_jit_t *j) {
    lr_jit_add_symbol(j, "llvm.fabs.f32", (void *)(uintptr_t)&llvm_fabs_f32);
    lr_jit_add_symbol(j, "llvm.fabs.f64", (void *)(uintptr_t)&llvm_fabs_f64);
    lr_jit_add_symbol(j, "llvm.sqrt.f32", (void *)(uintptr_t)&llvm_sqrt_f32);
    lr_jit_add_symbol(j, "llvm.sqrt.f64", (void *)(uintptr_t)&llvm_sqrt_f64);
    lr_jit_add_symbol(j, "llvm.exp.f32", (void *)(uintptr_t)&llvm_exp_f32);
    lr_jit_add_symbol(j, "llvm.exp.f64", (void *)(uintptr_t)&llvm_exp_f64);
    lr_jit_add_symbol(j, "llvm.pow.f32", (void *)(uintptr_t)&llvm_pow_f32);
    lr_jit_add_symbol(j, "llvm.pow.f64", (void *)(uintptr_t)&llvm_pow_f64);
    lr_jit_add_symbol(j, "llvm.copysign.f32", (void *)(uintptr_t)&llvm_copysign_f32);
    lr_jit_add_symbol(j, "llvm.copysign.f64", (void *)(uintptr_t)&llvm_copysign_f64);
    lr_jit_add_symbol(j, "llvm.powi.f32", (void *)(uintptr_t)&llvm_powi_f32);
    lr_jit_add_symbol(j, "llvm.powi.f64", (void *)(uintptr_t)&llvm_powi_f64);
    lr_jit_add_symbol(j, "llvm.memset.p0i8.i32", (void *)(uintptr_t)&llvm_memset_p0i8_i32);
    lr_jit_add_symbol(j, "llvm.memset.p0i8.i64", (void *)(uintptr_t)&llvm_memset_p0i8_i64);
    lr_jit_add_symbol(j, "llvm.memcpy.p0i8.p0i8.i32", (void *)(uintptr_t)&llvm_memcpy_p0i8_p0i8_i32);
    lr_jit_add_symbol(j, "llvm.memcpy.p0i8.p0i8.i64", (void *)(uintptr_t)&llvm_memcpy_p0i8_p0i8_i64);
}

int lr_jit_load_library(lr_jit_t *j, const char *path) {
    if (!j || !path || !path[0])
        return -1;
    void *handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
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

static void *lookup_symbol(lr_jit_t *j, const char *name) {
    uint32_t hash = symbol_hash(name);
    lr_sym_entry_t *local = find_symbol_entry(j, name, hash);
    if (local)
        return local->addr;

    if (miss_cache_contains(j, name, hash))
        return NULL;

    for (lr_lib_entry_t *l = j->libs; l; l = l->next) {
        void *addr = dlsym(l->handle, name);
        if (addr) {
            lr_jit_add_symbol(j, name, addr);
            return addr;
        }
    }
    void *addr = dlsym(RTLD_DEFAULT, name);
    if (addr) {
        lr_jit_add_symbol(j, name, addr);
        return addr;
    }
    miss_cache_add(j, name, hash);
    return addr;
}

static int apply_module_global_relocs(lr_jit_t *j, lr_module_t *m) {
    for (lr_global_t *g = m->first_global; g; g = g->next) {
        if (!g->relocs)
            continue;
        uint8_t *base = lookup_symbol(j, g->name);
        if (!base)
            continue;
        for (lr_reloc_t *r = g->relocs; r; r = r->next) {
            void *target = lookup_symbol(j, r->symbol_name);
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

static int materialize_module_globals(lr_jit_t *j, lr_module_t *m) {
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

static const char *resolve_global_name(lr_module_t *m, uint32_t global_id) {
    const char *name = lr_module_symbol_name(m, global_id);
    if (name && name[0])
        return name;

    /* Backward-compatible fallback for modules that encoded function index. */
    uint32_t idx = 0;
    for (lr_func_t *fn = m->first_func; fn; fn = fn->next, idx++) {
        if (idx == global_id)
            return fn->name;
    }

    return NULL;
}

/*
 * Resolve global/function symbol operands to concrete addresses.
 * Returns:
 *   0  success
 *   1  unresolved symbol (retry later)
 *  -1  malformed IR/symbol table state
 */
static int resolve_global_operands(lr_jit_t *j, lr_module_t *m, lr_func_t *f,
                                   void *self_addr, const char **missing_symbol) {
    for (lr_block_t *b = f->first_block; b; b = b->next) {
        for (lr_inst_t *inst = b->first; inst; inst = inst->next) {
            for (uint32_t i = 0; i < inst->num_operands; i++) {
                lr_operand_t *op = &inst->operands[i];
                if (op->kind != LR_VAL_GLOBAL)
                    continue;

                const char *name = resolve_global_name(m, op->global_id);
                if (!name || !name[0])
                    return -1;

                /* For CALL callee (operand 0), capture ABI metadata
                   before converting to IMM_I64 (which loses global_id). */
                if (inst->op == LR_OP_CALL && i == 0) {
                    lr_func_t *callee = NULL;
                    for (lr_func_t *fn = m->first_func; fn; fn = fn->next) {
                        if (strcmp(fn->name, name) == 0) {
                            callee = fn;
                            break;
                        }
                    }
                    if (callee) {
                        inst->call_external_abi = (callee->first_block == NULL);
                        inst->call_vararg = callee->vararg;
                    } else {
                        inst->call_external_abi = true;
                        inst->call_vararg = false;
                    }
                }

                void *addr = lookup_symbol(j, name);
                if (!addr && strcmp(name, f->name) == 0)
                    addr = self_addr;
                if (!addr) {
                    if (missing_symbol)
                        *missing_symbol = name;
                    return 1;
                }

                op->kind = LR_VAL_IMM_I64;
                op->imm_i64 = (int64_t)(intptr_t)addr + op->global_offset;
                op->global_offset = 0;
            }
        }
    }
    return 0;
}

int lr_jit_add_module(lr_jit_t *j, lr_module_t *m) {
    if (!j || !j->target || !m) return -1;

    JIT_PROF_START(make_writable);
    if (make_writable(j) != 0) return -1;
    JIT_PROF_END(make_writable);

    JIT_PROF_START(globals);
    if (materialize_module_globals(j, m) != 0) return -1;
    JIT_PROF_END(globals);

    uint32_t nfuncs = 0;
    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (!f->is_decl)
            nfuncs++;
    }

    if (nfuncs == 0) {
        if (apply_module_global_relocs(j, m) != 0)
            return -1;
        if (make_executable(j) != 0)
            return -1;
        return 0;
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

    for (uint32_t i = 0; i < nfuncs; i++) {
        const char *name = funcs[i]->name;
        if (name && name[0]) {
            uint32_t hash = symbol_hash(name);
            if (!find_symbol_entry(j, name, hash))
                miss_cache_add(j, name, hash);
        }
    }

    /* Resolve function declarations eagerly — these are external symbols
       that will be needed during compilation (e.g., runtime functions). */
    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (f->is_decl && f->name && f->name[0])
            lookup_symbol(j, f->name);
    }
    JIT_PROF_END(pre_register);

    JIT_PROF_START(compile_loop);
    bool *compiled = lr_arena_array(j->arena, bool, nfuncs);
    uint32_t remaining = nfuncs;
    while (remaining > 0) {
        bool progress = false;
        const char *last_missing = NULL;

        for (uint32_t i = 0; i < nfuncs; i++) {
            if (compiled[i])
                continue;

            lr_func_t *f = funcs[i];
            void *self_addr = j->code_buf + j->code_size;
            JIT_PROF_START(resolve);
            int resolve_rc = resolve_global_operands(j, m, f, self_addr, &last_missing);
            JIT_PROF_END(resolve);
            if (resolve_rc == 1)
                continue;
            if (resolve_rc != 0)
                return -1;

            uint8_t *func_start = j->code_buf + j->code_size;
            size_t free_space = j->code_cap - j->code_size;
            size_t code_len = 0;
            JIT_PROF_START(compile);
            int rc = j->target->compile_func(f, m, func_start, free_space, &code_len, j->arena);
            JIT_PROF_END(compile);
            if (rc != 0) return rc;

            if (code_len > free_space)
                return -1;

            j->code_size += code_len;

            lr_jit_add_symbol(j, f->name, func_start);
            compiled[i] = true;
            remaining--;
            progress = true;
        }

        if (!progress) {
            if (last_missing)
                fprintf(stderr, "unresolved symbol: %s\n", last_missing);
            return -1;
        }
    }
    JIT_PROF_END(compile_loop);

    /* Re-apply relocations after module-defined function symbols exist.
       This fixes globals (e.g. vtables) referencing internal functions. */
    if (apply_module_global_relocs(j, m) != 0)
        return -1;

    JIT_PROF_START(make_exec);
    if (make_executable(j) != 0)
        return -1;
    JIT_PROF_END(make_exec);

    return 0;
}

void *lr_jit_get_function(lr_jit_t *j, const char *name) {
    return lookup_symbol(j, name);
}

void lr_jit_destroy(lr_jit_t *j) {
    if (!j) return;
    for (lr_lib_entry_t *l = j->libs; l; l = l->next) {
        if (l->handle)
            dlclose(l->handle);
    }
    if (j->code_buf && j->code_buf != MAP_FAILED)
        munmap(j->code_buf, j->code_cap);
    if (j->data_buf && j->data_buf != MAP_FAILED)
        munmap(j->data_buf, j->data_cap);
    free(j->miss_buckets);
    free(j->sym_buckets);
    lr_arena_destroy(j->arena);
    free(j);
}
