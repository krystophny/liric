#ifndef LIRIC_LOW_LEVEL_H
#define LIRIC_LOW_LEVEL_H

#include <liric/liric_ir_shared.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*lr_ll_func_cb_t)(lr_func_t *func, lr_module_t *mod, void *ctx);

lr_module_t *lr_parse_ll(const char *src, size_t len, char *err, size_t errlen);
lr_module_t *lr_parse_ll_streaming(const char *src, size_t len,
                                   lr_ll_func_cb_t on_func, void *ctx,
                                   char *err, size_t errlen);
lr_module_t *lr_parse_bc(const uint8_t *data, size_t len, char *err, size_t errlen);
lr_module_t *lr_parse_wasm(const uint8_t *data, size_t len, char *err, size_t errlen);
lr_module_t *lr_parse_auto(const uint8_t *data, size_t len, char *err, size_t errlen);
void lr_module_free(lr_module_t *m);
int lr_module_merge(lr_module_t *dest, lr_module_t *src);

lr_type_t *lr_type_array_new(lr_module_t *m, lr_type_t *elem, uint64_t count);
lr_type_t *lr_type_vector_new(lr_module_t *m, lr_type_t *elem, uint64_t count);
lr_type_t *lr_type_struct_new(lr_module_t *m, lr_type_t **fields,
                              uint32_t num_fields, bool packed);
lr_type_t *lr_type_func_new(lr_module_t *m, lr_type_t *ret,
                            lr_type_t **params, uint32_t num_params,
                            bool vararg);

lr_jit_t *lr_jit_create(void);
lr_jit_t *lr_jit_create_for_target(const char *target_name);
const char *lr_jit_host_target_name(void);
const char *lr_jit_target_name(const lr_jit_t *j);
void lr_jit_add_symbol(lr_jit_t *j, const char *name, void *addr);
int lr_jit_load_library(lr_jit_t *j, const char *path);
int lr_jit_set_runtime_bc(lr_jit_t *j, const uint8_t *bc_data, size_t bc_len);
void lr_jit_begin_update(lr_jit_t *j);
int lr_jit_add_module(lr_jit_t *j, lr_module_t *m);
void lr_jit_end_update(lr_jit_t *j);
void *lr_jit_get_function(lr_jit_t *j, const char *name);
void lr_jit_destroy(lr_jit_t *j);

#ifdef __cplusplus
}
#endif

#endif
