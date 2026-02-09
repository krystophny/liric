#ifndef LIRIC_FRONTEND_REGISTRY_H
#define LIRIC_FRONTEND_REGISTRY_H

#include "arena.h"
#include "ir.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct lr_frontend {
    const char *name;
    bool (*matches_input)(const uint8_t *data, size_t len);
    lr_module_t *(*parse_with_arena)(const uint8_t *data, size_t len,
                                     lr_arena_t *arena, char *err, size_t errlen);
} lr_frontend_t;

const lr_frontend_t *lr_frontend_by_name(const char *name);
const lr_frontend_t *lr_frontend_detect(const uint8_t *data, size_t len);

#endif
