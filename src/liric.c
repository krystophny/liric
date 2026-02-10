#include "arena.h"
#include "frontend_registry.h"
#include "ir.h"
#include "jit.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void set_err(char *err, size_t errlen, const char *msg) {
    if (!err || errlen == 0)
        return;
    snprintf(err, errlen, "%s", msg);
}

static lr_module_t *parse_with_frontend(const lr_frontend_t *frontend,
                                        const uint8_t *data, size_t len,
                                        char *err, size_t errlen) {
    if (!frontend || !frontend->parse_with_arena) {
        set_err(err, errlen, "no matching frontend for input");
        return NULL;
    }

    lr_arena_t *arena = lr_arena_create(0);
    if (!arena) {
        set_err(err, errlen, "arena allocation failed");
        return NULL;
    }

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

lr_module_t *lr_parse_bc(const uint8_t *data, size_t len, char *err, size_t errlen) {
    const lr_frontend_t *frontend = lr_frontend_by_name("bc");
    return parse_with_frontend(frontend, data, len, err, errlen);
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
