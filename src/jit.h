#ifndef LIRIC_JIT_H
#define LIRIC_JIT_H

#include "ir.h"
#include "target.h"
#include <stddef.h>

typedef struct lr_sym_entry {
    char *name;
    uint32_t hash;
    void *addr;
    struct lr_sym_entry *next;        /* insertion order chain */
    struct lr_sym_entry *bucket_next; /* hash bucket chain */
} lr_sym_entry_t;

typedef struct lr_lib_entry {
    void *handle;
    struct lr_lib_entry *next;
} lr_lib_entry_t;

typedef struct lr_sym_miss_entry {
    char *name;
    uint32_t hash;
    struct lr_sym_miss_entry *bucket_next;
} lr_sym_miss_entry_t;

typedef struct lr_jit {
    const lr_target_t *target;
    bool map_jit_enabled;
    bool update_active;
    bool update_dirty;
    uint8_t *code_buf;
    size_t code_size;
    size_t code_cap;
    size_t update_begin_code_size;
    uint8_t *data_buf;
    size_t data_size;
    size_t data_cap;
    lr_sym_entry_t *symbols;
    lr_sym_entry_t **sym_buckets;
    uint32_t sym_bucket_count;
    lr_sym_miss_entry_t **miss_buckets;
    uint32_t miss_bucket_count;
    lr_lib_entry_t *libs;
    lr_arena_t *arena;
} lr_jit_t;

lr_jit_t *lr_jit_create(void);
lr_jit_t *lr_jit_create_for_target(const char *target_name);
const char *lr_jit_host_target_name(void);
const char *lr_jit_target_name(const lr_jit_t *j);
void lr_jit_add_symbol(lr_jit_t *j, const char *name, void *addr);
int lr_jit_load_library(lr_jit_t *j, const char *path);
void lr_jit_begin_update(lr_jit_t *j);
int lr_jit_add_module(lr_jit_t *j, lr_module_t *m);
void lr_jit_end_update(lr_jit_t *j);
void *lr_jit_get_function(lr_jit_t *j, const char *name);
void lr_jit_destroy(lr_jit_t *j);

/*
 * POSIX guarantees void* and function pointers have the same size/representation.
 * Use memcpy to convert without triggering -Wpedantic warnings.
 */
#define LR_JIT_FN(type, jit, name) \
    lr_jit_fn_cast_##type(lr_jit_get_function((jit), (name)))

#include <string.h>
static inline void lr_jit_fn_to_ptr(void *dst, void *src) {
    memcpy(dst, &src, sizeof(src));
}

#define LR_JIT_GET_FN(fn_var, jit, name) \
    do { void *_p = lr_jit_get_function((jit), (name)); \
         lr_jit_fn_to_ptr(&(fn_var), _p); } while (0)

#endif
