#include "frontend_registry.h"
#include "ll_parser.h"
#include "wasm_decode.h"
#include "wasm_to_ir.h"
#include <string.h>

static bool match_wasm_magic(const uint8_t *data, size_t len) {
    return data && len >= 4 &&
           data[0] == 0x00 &&
           data[1] == 'a' &&
           data[2] == 's' &&
           data[3] == 'm';
}

static bool match_ll_fallback(const uint8_t *data, size_t len) {
    (void)data;
    (void)len;
    return true;
}

static lr_module_t *parse_ll_with_arena(const uint8_t *data, size_t len,
                                        lr_arena_t *arena, char *err, size_t errlen) {
    if (!data && len != 0)
        return NULL;
    return lr_parse_ll_text((const char *)data, len, arena, err, errlen);
}

static lr_module_t *parse_wasm_with_arena(const uint8_t *data, size_t len,
                                          lr_arena_t *arena, char *err, size_t errlen) {
    lr_wasm_module_t *wmod = lr_wasm_decode(data, len, arena, err, errlen);
    if (!wmod)
        return NULL;
    return lr_wasm_to_ir(wmod, arena, err, errlen);
}

static const lr_frontend_t g_frontends[] = {
    {
        .name = "wasm",
        .matches_input = match_wasm_magic,
        .parse_with_arena = parse_wasm_with_arena,
    },
    {
        .name = "ll",
        .matches_input = match_ll_fallback,
        .parse_with_arena = parse_ll_with_arena,
    },
};

const lr_frontend_t *lr_frontend_by_name(const char *name) {
    if (!name || !name[0])
        return NULL;

    if (strcmp(name, "llvm-ir") == 0)
        name = "ll";

    size_t n = sizeof(g_frontends) / sizeof(g_frontends[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(g_frontends[i].name, name) == 0)
            return &g_frontends[i];
    }
    return NULL;
}

const lr_frontend_t *lr_frontend_detect(const uint8_t *data, size_t len) {
    size_t n = sizeof(g_frontends) / sizeof(g_frontends[0]);
    for (size_t i = 0; i < n; i++) {
        if (g_frontends[i].matches_input(data, len))
            return &g_frontends[i];
    }
    return NULL;
}
