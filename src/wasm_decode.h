#ifndef LIRIC_WASM_DECODE_H
#define LIRIC_WASM_DECODE_H

#include "arena.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct lr_wasm_functype {
    uint8_t *params;
    uint8_t *results;
    uint32_t num_params;
    uint32_t num_results;
} lr_wasm_functype_t;

typedef struct lr_wasm_import {
    char *module_name;
    char *name;
    uint8_t kind;       /* 0=func, 1=table, 2=memory, 3=global */
    uint32_t type_idx;  /* for kind=0: index into types array */
} lr_wasm_import_t;

typedef struct lr_wasm_export {
    char *name;
    uint8_t kind;
    uint32_t index;
} lr_wasm_export_t;

typedef struct lr_wasm_local_group {
    uint32_t count;
    uint8_t type;
} lr_wasm_local_group_t;

typedef struct lr_wasm_code {
    lr_wasm_local_group_t *local_groups;
    uint32_t num_local_groups;
    const uint8_t *body;
    size_t body_len;
} lr_wasm_code_t;

typedef struct lr_wasm_memory {
    uint32_t min_pages;
    uint32_t max_pages;
    bool has_max;
} lr_wasm_memory_t;

typedef struct lr_wasm_global {
    uint8_t type;
    bool mutable_;
    int64_t init_i64;
} lr_wasm_global_t;

typedef struct lr_wasm_data {
    uint32_t memory_idx;
    uint32_t offset;
    const uint8_t *bytes;
    uint32_t size;
} lr_wasm_data_t;

typedef struct lr_wasm_module {
    lr_wasm_functype_t *types;      uint32_t num_types;
    lr_wasm_import_t *imports;      uint32_t num_imports;
    uint32_t *func_type_indices;    uint32_t num_funcs;
    lr_wasm_export_t *exports;      uint32_t num_exports;
    lr_wasm_code_t *codes;          uint32_t num_codes;
    lr_wasm_memory_t *memories;     uint32_t num_memories;
    lr_wasm_global_t *globals;      uint32_t num_globals;
    lr_wasm_data_t *data;           uint32_t num_data;
    uint32_t num_func_imports;
    lr_arena_t *arena;
} lr_wasm_module_t;

lr_wasm_module_t *lr_wasm_decode(const uint8_t *data, size_t len,
                                  lr_arena_t *arena, char *err, size_t errlen);

/* Exposed for testing */
size_t lr_wasm_read_leb_u32(const uint8_t *buf, size_t len, uint32_t *out);
size_t lr_wasm_read_leb_i32(const uint8_t *buf, size_t len, int32_t *out);
size_t lr_wasm_read_leb_i64(const uint8_t *buf, size_t len, int64_t *out);

#endif
