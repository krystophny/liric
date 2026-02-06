#ifndef LIRIC_ARENA_H
#define LIRIC_ARENA_H

#include <stddef.h>
#include <stdint.h>

typedef struct lr_arena_chunk {
    struct lr_arena_chunk *next;
    size_t size;
    size_t used;
    uint8_t data[];
} lr_arena_chunk_t;

typedef struct lr_arena {
    lr_arena_chunk_t *head;
    size_t default_chunk_size;
} lr_arena_t;

lr_arena_t *lr_arena_create(size_t default_chunk_size);
void *lr_arena_alloc(lr_arena_t *a, size_t size, size_t align);
char *lr_arena_strdup(lr_arena_t *a, const char *s, size_t len);
void lr_arena_destroy(lr_arena_t *a);

#define lr_arena_new(a, T) ((T *)lr_arena_alloc((a), sizeof(T), _Alignof(T)))
#define lr_arena_array(a, T, n) ((T *)lr_arena_alloc((a), sizeof(T) * (n), _Alignof(T)))

#endif
