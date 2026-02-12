#include "arena.h"
#include "frontend_registry.h"
#include "ir.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ---- Module lifecycle -------------------------------------------------- */

lr_module_t *lr_module_create_new(void) {
    lr_arena_t *arena = lr_arena_create(0);
    if (!arena) return NULL;
    return lr_module_create(arena);
}

void lr_module_dump_to(lr_module_t *m, void *file_handle) {
    lr_module_dump(m, (FILE *)file_handle);
}

/* ---- Type constructors ------------------------------------------------- */

lr_type_t *lr_type_void_get(lr_module_t *m)   { return m->type_void; }
lr_type_t *lr_type_i1_get(lr_module_t *m)     { return m->type_i1; }
lr_type_t *lr_type_i8_get(lr_module_t *m)     { return m->type_i8; }
lr_type_t *lr_type_i16_get(lr_module_t *m)    { return m->type_i16; }
lr_type_t *lr_type_i32_get(lr_module_t *m)    { return m->type_i32; }
lr_type_t *lr_type_i64_get(lr_module_t *m)    { return m->type_i64; }
lr_type_t *lr_type_float_get(lr_module_t *m)  { return m->type_float; }
lr_type_t *lr_type_double_get(lr_module_t *m) { return m->type_double; }
lr_type_t *lr_type_ptr_get(lr_module_t *m)    { return m->type_ptr; }

lr_type_t *lr_type_array_new(lr_module_t *m, lr_type_t *elem, uint64_t count) {
    return lr_type_array(m->arena, elem, count);
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

/* ---- Function / block / vreg ------------------------------------------- */

lr_func_t *lr_func_define(lr_module_t *m, const char *name, lr_type_t *ret,
                           lr_type_t **params, uint32_t num_params, bool vararg) {
    return lr_func_create(m, name, ret, params, num_params, vararg);
}

lr_func_t *lr_func_declare_ext(lr_module_t *m, const char *name, lr_type_t *ret,
                                lr_type_t **params, uint32_t num_params,
                                bool vararg) {
    return lr_func_declare(m, name, ret, params, num_params, vararg);
}

uint32_t lr_func_param_vreg(lr_func_t *f, uint32_t param_idx) {
    return f->param_vregs[param_idx];
}

uint32_t lr_func_num_params(lr_func_t *f) {
    return f->num_params;
}

lr_block_t *lr_block_new(lr_func_t *f, lr_module_t *m, const char *name) {
    return lr_block_create(f, m->arena, name);
}

uint32_t lr_block_id(lr_block_t *b) {
    return b->id;
}

uint32_t lr_vreg_alloc(lr_func_t *f) {
    return lr_vreg_new(f);
}

/* ---- Global variables -------------------------------------------------- */

lr_global_t *lr_global_define(lr_module_t *m, const char *name, lr_type_t *type,
                               bool is_const, const void *init_data,
                               size_t init_size) {
    lr_global_t *g = lr_global_create(m, name, type, is_const);
    if (init_data && init_size > 0) {
        g->init_data = lr_arena_alloc(m->arena, init_size, 1);
        memcpy(g->init_data, init_data, init_size);
        g->init_size = init_size;
    }
    return g;
}

lr_global_t *lr_global_declare_ext(lr_module_t *m, const char *name,
                                    lr_type_t *type) {
    lr_global_t *g = lr_global_create(m, name, type, false);
    g->is_external = true;
    return g;
}

uint32_t lr_global_id(lr_global_t *g) {
    return g->id;
}

void lr_global_add_reloc(lr_module_t *m, lr_global_t *g, size_t offset,
                          const char *symbol_name) {
    lr_reloc_t *r = lr_arena_new(m->arena, lr_reloc_t);
    r->offset = offset;
    r->symbol_name = lr_arena_strdup(m->arena, symbol_name, strlen(symbol_name));
    r->next = g->relocs;
    g->relocs = r;
}

/* ---- Symbol interning -------------------------------------------------- */

uint32_t lr_symbol_intern(lr_module_t *m, const char *name) {
    return lr_module_intern_symbol(m, name);
}
