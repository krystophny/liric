#include "arena.h"
#include "frontend_registry.h"
#include "ir.h"
#include "ll_parser.h"
#include <stdio.h>

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

typedef struct {
    int (*on_func)(lr_func_t *func, lr_module_t *mod, void *ctx);
    void *ctx;
} ll_stream_cb_ctx_t;

static int ll_stream_cb_adapter(lr_func_t *func, lr_module_t *mod, void *ctx) {
    ll_stream_cb_ctx_t *adapt = (ll_stream_cb_ctx_t *)ctx;
    if (!adapt || !adapt->on_func)
        return 0;
    return adapt->on_func(func, mod, adapt->ctx);
}

lr_module_t *lr_parse_ll_streaming(const char *src, size_t len,
                                   int (*on_func)(lr_func_t *func,
                                                  lr_module_t *mod,
                                                  void *ctx),
                                   void *ctx,
                                   char *err, size_t errlen) {
    lr_arena_t *arena = lr_arena_create(0);
    if (!arena) {
        set_err(err, errlen, "arena allocation failed");
        return NULL;
    }

    ll_stream_cb_ctx_t cb_ctx = { on_func, ctx };
    lr_module_t *m = lr_parse_ll_text_streaming(
        src, len, arena,
        on_func ? ll_stream_cb_adapter : NULL,
        &cb_ctx, err, errlen);
    if (!m) {
        lr_arena_destroy(arena);
        return NULL;
    }
    return m;
}

lr_module_t *lr_parse_ll(const char *src, size_t len, char *err, size_t errlen) {
    return lr_parse_ll_streaming(src, len, NULL, NULL, err, errlen);
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

/* ---- Composite type constructors --------------------------------------- */

lr_type_t *lr_type_array_new(lr_module_t *m, lr_type_t *elem, uint64_t count) {
    return lr_type_array(m->arena, elem, count);
}

lr_type_t *lr_type_vector_new(lr_module_t *m, lr_type_t *elem, uint64_t count) {
    return lr_type_vector(m->arena, elem, count);
}

lr_type_t *lr_type_struct_new(lr_module_t *m, lr_type_t **fields,
                               uint32_t num_fields, bool packed) {
    return lr_type_struct(m->arena, fields, num_fields, packed, NULL);
}

lr_type_t *lr_type_func_new(lr_module_t *m, lr_type_t *ret,
                              lr_type_t **params, uint32_t num_params,
                              bool vararg) {
    return lr_type_func(m->arena, ret, params, num_params, vararg);
}
