#include "arena.h"
#include "ir.h"
#include "ll_parser.h"
#include "jit.h"
#include <stdlib.h>
#include <string.h>

/* Wrapper that owns the arena */
typedef struct lr_module_wrapper {
    lr_arena_t *arena;
    lr_module_t *module;
} lr_module_wrapper_t;

lr_module_t *lr_parse_ll(const char *src, size_t len, char *err, size_t errlen) {
    lr_arena_t *arena = lr_arena_create(0);
    if (!arena) return NULL;

    lr_module_t *m = lr_parse_ll_text(src, len, arena, err, errlen);
    if (!m) {
        lr_arena_destroy(arena);
        return NULL;
    }

    /* Stash arena pointer in the module for lr_module_free.
       We use the fact that arena is the first allocated thing and the module
       already has a pointer to it. */
    return m;
}

void lr_module_free(lr_module_t *m) {
    if (!m) return;
    lr_arena_destroy(m->arena);
}
