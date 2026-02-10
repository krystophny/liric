#include "jit.h"
#include "ir.h"
#include "objfile.h"
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
static int register_default_symbol_providers(lr_jit_t *j);
static void *resolve_symbol_from_jit_table(lr_jit_t *j, const char *name);
static void *resolve_symbol_from_loaded_libraries(lr_jit_t *j, const char *name);
static void *resolve_symbol_from_process(lr_jit_t *j, const char *name);

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

static void llvm_memmove_p0i8_p0i8_i32(void *dst, const void *src, int32_t len, uint64_t is_volatile) {
    (void)is_volatile;
    if (!dst || !src || len <= 0)
        return;
    memmove(dst, src, (size_t)len);
}

static void llvm_memmove_p0i8_p0i8_i64(void *dst, const void *src, int64_t len, uint64_t is_volatile) {
    (void)is_volatile;
    if (!dst || !src || len <= 0)
        return;
    memmove(dst, src, (size_t)len);
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

static int make_executable_from(lr_jit_t *j, size_t clear_from) {
    if (clear_from > j->code_size)
        clear_from = j->code_size;
    if (clear_from < j->code_size) {
        __builtin___clear_cache((char *)(j->code_buf + clear_from),
                                (char *)(j->code_buf + j->code_size));
    }

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

    if (register_default_symbol_providers(j) != 0) {
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
    if (register_symbol_provider(j, "jit-table", resolve_symbol_from_jit_table,
                                 false, false) != 0)
        return -1;
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
    lr_jit_add_symbol(j, "llvm.memmove.p0i8.p0i8.i32", (void *)(uintptr_t)&llvm_memmove_p0i8_p0i8_i32);
    lr_jit_add_symbol(j, "llvm.memmove.p0i8.p0i8.i64", (void *)(uintptr_t)&llvm_memmove_p0i8_p0i8_i64);
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

static void *resolve_symbol_from_jit_table(lr_jit_t *j, const char *name) {
    if (!j || !name || !name[0])
        return NULL;
    uint32_t hash = symbol_hash(name);
    lr_sym_entry_t *entry = find_symbol_entry(j, name, hash);
    return entry ? entry->addr : NULL;
}

static void *resolve_symbol_from_loaded_libraries(lr_jit_t *j, const char *name) {
    if (!j || !name || !name[0])
        return NULL;
    for (lr_lib_entry_t *l = j->libs; l; l = l->next) {
        void *addr = dlsym(l->handle, name);
        if (addr)
            return addr;
    }
    return NULL;
}

static void *resolve_symbol_from_process(lr_jit_t *j, const char *name) {
    (void)j;
    if (!name || !name[0])
        return NULL;
    return dlsym(RTLD_DEFAULT, name);
}

static void *lookup_symbol(lr_jit_t *j, const char *name) {
    if (!j || !name || !name[0])
        return NULL;

    uint32_t hash = symbol_hash(name);
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

static int finalize_module_functions(lr_module_t *m, lr_func_t **funcs, uint32_t nfuncs,
                                     lr_arena_t *fallback_arena) {
    lr_arena_t *layout_arena = (m && m->arena) ? m->arena : fallback_arena;
    for (uint32_t i = 0; i < nfuncs; i++) {
        if (lr_func_finalize(funcs[i], layout_arena) != 0)
            return -1;
    }
    return 0;
}

static int jit_build_module_symbol_cache(lr_objfile_ctx_t *oc, lr_module_t *m) {
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

static int apply_jit_relocs(lr_jit_t *j, const lr_objfile_ctx_t *ctx, const char **missing_symbol) {
    if (!j || !ctx)
        return -1;
    void **got_slots = NULL;
    if (ctx->num_symbols > 0) {
        got_slots = (void **)calloc(ctx->num_symbols, sizeof(void *));
        if (!got_slots)
            return -1;
    }

    int rc = 0;
    for (uint32_t i = 0; i < ctx->num_relocs; i++) {
        const lr_obj_reloc_t *rel = &ctx->relocs[i];
        if (rel->symbol_idx >= ctx->num_symbols) {
            rc = -1;
            break;
        }

        const lr_obj_symbol_t *sym = &ctx->symbols[rel->symbol_idx];
        const char *sym_name = sym->name;
        void *target_addr = NULL;
        if (sym->is_defined && sym->section == 1) {
            if ((size_t)sym->offset >= j->code_size) {
                rc = -1;
                break;
            }
            target_addr = (void *)(j->code_buf + sym->offset);
        } else {
            target_addr = lookup_symbol(j, sym_name);
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
    }

    free(got_slots);
    return rc;
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
    int rc = j->target->compile_func(f, m, func_start, free_space, &code_len, j->arena);
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

int lr_jit_add_module(lr_jit_t *j, lr_module_t *m) {
    if (!j || !j->target || !m) return -1;

    bool own_wx_transition = !j->update_active;
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
    if (materialize_module_globals(j, m) != 0) goto done;
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
    if (finalize_module_functions(m, funcs, nfuncs, j->arena) != 0)
        goto done;

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
    if (apply_jit_relocs(j, &fixup_ctx, &missing_symbol) != 0) {
        if (missing_symbol)
            fprintf(stderr, "unresolved symbol: %s\n", missing_symbol);
        goto done;
    }
    JIT_PROF_END(patch_fixups);

    for (uint32_t i = 0; i < nfuncs; i++) {
        if (funcs[i]->name && funcs[i]->name[0] && func_addrs[i])
            lr_jit_add_symbol(j, funcs[i]->name, func_addrs[i]);
    }

    /* Re-apply relocations after module-defined function symbols exist.
       This fixes globals (e.g. vtables) referencing internal functions. */
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
    if (!j || j->update_active)
        return;
    if (make_writable(j) != 0)
        return;
    j->update_active = true;
    j->update_dirty = false;
    j->update_begin_code_size = j->code_size;
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
