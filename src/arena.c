#include "arena.h"
#include <stdlib.h>
#include <string.h>

static lr_arena_chunk_t *chunk_create(size_t data_size) {
    lr_arena_chunk_t *c = malloc(sizeof(lr_arena_chunk_t) + data_size);
    if (!c) return NULL;
    c->next = NULL;
    c->size = data_size;
    c->used = 0;
    return c;
}

static void *arena_alloc_impl(lr_arena_t *a, size_t size, size_t align,
                              int zero_init) {
    if (!a) return NULL;
    if (align == 0) align = 1;

    lr_arena_chunk_t *c = a->head;
    size_t offset = (c->used + align - 1) & ~(align - 1);
    if (offset + size <= c->size) {
        c->used = offset + size;
        void *p = c->data + offset;
        if (zero_init) memset(p, 0, size);
        return p;
    }

    size_t need = size + align;
    size_t chunk_size = need > a->default_chunk_size ? need : a->default_chunk_size;
    lr_arena_chunk_t *nc = chunk_create(chunk_size);
    if (!nc) return NULL;
    nc->next = a->head;
    a->head = nc;

    size_t off2 = (nc->used + align - 1) & ~(align - 1);
    nc->used = off2 + size;
    void *p = nc->data + off2;
    if (zero_init) memset(p, 0, size);
    return p;
}

lr_arena_t *lr_arena_create(size_t default_chunk_size) {
    if (default_chunk_size == 0) default_chunk_size = 64 * 1024;
    lr_arena_t *a = malloc(sizeof(lr_arena_t));
    if (!a) return NULL;
    a->default_chunk_size = default_chunk_size;
    a->head = chunk_create(default_chunk_size);
    if (!a->head) { free(a); return NULL; }
    return a;
}

void *lr_arena_alloc(lr_arena_t *a, size_t size, size_t align) {
    return arena_alloc_impl(a, size, align, 1);
}

void *lr_arena_alloc_uninit(lr_arena_t *a, size_t size, size_t align) {
    return arena_alloc_impl(a, size, align, 0);
}

char *lr_arena_strdup(lr_arena_t *a, const char *s, size_t len) {
    char *p = lr_arena_alloc_uninit(a, len + 1, 1);
    if (!p) return NULL;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

void lr_arena_destroy(lr_arena_t *a) {
    if (!a) return;
    lr_arena_chunk_t *c = a->head;
    while (c) {
        lr_arena_chunk_t *next = c->next;
        free(c);
        c = next;
    }
    free(a);
}
