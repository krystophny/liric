#include "jit.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <dlfcn.h>

#define CODE_PAGE_SIZE (1024 * 1024)
#define DATA_PAGE_SIZE (256 * 1024)

lr_jit_t *lr_jit_create(void) {
    lr_jit_t *j = calloc(1, sizeof(lr_jit_t));
    if (!j) return NULL;

    j->target = lr_target_x86_64();
    j->arena = lr_arena_create(0);

    j->code_cap = CODE_PAGE_SIZE;
    j->code_buf = mmap(NULL, j->code_cap,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (j->code_buf == MAP_FAILED) {
        lr_arena_destroy(j->arena);
        free(j);
        return NULL;
    }

    j->data_cap = DATA_PAGE_SIZE;
    j->data_buf = mmap(NULL, j->data_cap,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (j->data_buf == MAP_FAILED) {
        munmap(j->code_buf, j->code_cap);
        lr_arena_destroy(j->arena);
        free(j);
        return NULL;
    }

    return j;
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

int lr_jit_add_module(lr_jit_t *j, lr_module_t *m) {
    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (f->is_decl) continue;

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

        /* Register this function as a symbol */
        lr_jit_add_symbol(j, f->name, func_start);
    }

    /* Make code executable */
    mprotect(j->code_buf, j->code_cap, PROT_READ | PROT_EXEC);

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
