#include "arena.h"
#include "frontend_registry.h"
#include "ir.h"
#include "jit.h"
#include <stdlib.h>
#include <stdint.h>

static lr_module_t *parse_with_frontend(const lr_frontend_t *frontend,
                                        const uint8_t *data, size_t len,
                                        char *err, size_t errlen) {
    if (!frontend || !frontend->parse_with_arena)
        return NULL;

    lr_arena_t *arena = lr_arena_create(0);
    if (!arena)
        return NULL;

    lr_module_t *m = frontend->parse_with_arena(data, len, arena, err, errlen);
    if (!m) {
        lr_arena_destroy(arena);
        return NULL;
    }

    return m;
}

lr_module_t *lr_parse_ll(const char *src, size_t len, char *err, size_t errlen) {
    const lr_frontend_t *frontend = lr_frontend_by_name("ll");
    return parse_with_frontend(frontend, (const uint8_t *)src, len, err, errlen);
}

lr_module_t *lr_parse_wasm(const uint8_t *data, size_t len, char *err, size_t errlen) {
    const lr_frontend_t *frontend = lr_frontend_by_name("wasm");
    return parse_with_frontend(frontend, data, len, err, errlen);
}

lr_module_t *lr_parse_auto(const uint8_t *data, size_t len, char *err, size_t errlen) {
    const lr_frontend_t *frontend = lr_frontend_detect(data, len);
    return parse_with_frontend(frontend, data, len, err, errlen);
}

void lr_module_free(lr_module_t *m) {
    if (!m) return;
    lr_arena_destroy(m->arena);
}
