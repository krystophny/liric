#include "jit.h"
#include "ir.h"
#include "target.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include <dlfcn.h>

#if defined(__APPLE__) && defined(__aarch64__) && defined(MAP_JIT)
#include <pthread.h>
#define LR_CAN_USE_MAP_JIT 1
#else
#define LR_CAN_USE_MAP_JIT 0
#endif

#define CODE_PAGE_SIZE (1024 * 1024)
#define DATA_PAGE_SIZE (256 * 1024)

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
        lr_arena_destroy(j->arena);
        free(j);
        return NULL;
    }

    if (make_executable(j) != 0) {
        munmap(j->data_buf, j->data_cap);
        munmap(j->code_buf, j->code_cap);
        lr_arena_destroy(j->arena);
        free(j);
        return NULL;
    }

    return j;
}

lr_jit_t *lr_jit_create(void) {
    const char *host = lr_jit_host_target_name();
    return host ? lr_jit_create_for_target(host) : NULL;
}

void lr_jit_add_symbol(lr_jit_t *j, const char *name, void *addr) {
    lr_sym_entry_t *e = lr_arena_new(j->arena, lr_sym_entry_t);
    e->name = lr_arena_strdup(j->arena, name, strlen(name));
    e->addr = addr;
    e->next = j->symbols;
    j->symbols = e;
}

static void *lookup_symbol(lr_jit_t *j, const char *name) {
    for (lr_sym_entry_t *e = j->symbols; e; e = e->next) {
        if (strcmp(e->name, name) == 0)
            return e->addr;
    }
    void *addr = dlsym(RTLD_DEFAULT, name);
    return addr;
}

static int materialize_module_globals(lr_jit_t *j, lr_module_t *m) {
    for (lr_global_t *g = m->first_global; g; g = g->next) {
        if (!g->name || !g->name[0])
            continue;
        if (lookup_symbol(j, g->name))
            continue;

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
    return 0;
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
                                   void *self_addr) {
    for (lr_block_t *b = f->first_block; b; b = b->next) {
        for (lr_inst_t *inst = b->first; inst; inst = inst->next) {
            for (uint32_t i = 0; i < inst->num_operands; i++) {
                lr_operand_t *op = &inst->operands[i];
                if (op->kind != LR_VAL_GLOBAL)
                    continue;

                const char *name = resolve_global_name(m, op->global_id);
                if (!name || !name[0])
                    return -1;

                void *addr = lookup_symbol(j, name);
                if (!addr && strcmp(name, f->name) == 0)
                    addr = self_addr;
                if (!addr)
                    return 1;

                op->kind = LR_VAL_IMM_I64;
                op->imm_i64 = (int64_t)(intptr_t)addr;
            }
        }
    }
    return 0;
}

int lr_jit_add_module(lr_jit_t *j, lr_module_t *m) {
    if (!j || !j->target || !m) return -1;
    if (make_writable(j) != 0) return -1;
    if (materialize_module_globals(j, m) != 0) return -1;

    uint32_t nfuncs = 0;
    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (!f->is_decl)
            nfuncs++;
    }

    if (nfuncs == 0) {
        if (make_executable(j) != 0)
            return -1;
        return 0;
    }

    lr_func_t **funcs = lr_arena_array(j->arena, lr_func_t *, nfuncs);
    bool *compiled = lr_arena_array(j->arena, bool, nfuncs);
    uint32_t fi = 0;
    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (!f->is_decl)
            funcs[fi++] = f;
    }

    uint32_t remaining = nfuncs;
    while (remaining > 0) {
        bool progress = false;

        for (uint32_t i = 0; i < nfuncs; i++) {
            if (compiled[i])
                continue;

            lr_func_t *f = funcs[i];
            void *self_addr = j->code_buf + j->code_size;
            int resolve_rc = resolve_global_operands(j, m, f, self_addr);
            if (resolve_rc == 1)
                continue;
            if (resolve_rc != 0)
                return -1;

            lr_mfunc_t *mf = lr_arena_new(j->arena, lr_mfunc_t);
            mf->arena = j->arena;

            int rc = j->target->isel_func(f, mf, m);
            if (rc != 0) return rc;

            uint8_t tmp_buf[65536];
            size_t code_len = 0;
            rc = j->target->encode_func(mf, tmp_buf, sizeof(tmp_buf), &code_len);
            if (rc != 0) return rc;

            if (j->code_size + code_len > j->code_cap)
                return -1;

            uint8_t *func_start = j->code_buf + j->code_size;
            memcpy(func_start, tmp_buf, code_len);
            j->code_size += code_len;

            lr_jit_add_symbol(j, f->name, func_start);
            compiled[i] = true;
            remaining--;
            progress = true;
        }

        if (!progress)
            return -1;
    }

    if (make_executable(j) != 0)
        return -1;

    return 0;
}

void *lr_jit_get_function(lr_jit_t *j, const char *name) {
    return lookup_symbol(j, name);
}

void lr_jit_destroy(lr_jit_t *j) {
    if (!j) return;
    if (j->code_buf && j->code_buf != MAP_FAILED)
        munmap(j->code_buf, j->code_cap);
    if (j->data_buf && j->data_buf != MAP_FAILED)
        munmap(j->data_buf, j->data_cap);
    lr_arena_destroy(j->arena);
    free(j);
}
