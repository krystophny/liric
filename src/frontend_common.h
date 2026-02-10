#ifndef LIRIC_FRONTEND_COMMON_H
#define LIRIC_FRONTEND_COMMON_H

#include "ir.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void lr_frontend_set_error(char *err, size_t errlen, const char *fmt, ...);

uint32_t lr_frontend_intern_symbol(lr_module_t *m, const char *name);

lr_func_t *lr_frontend_create_function(lr_module_t *m, const char *name,
                                       lr_type_t *ret_type, lr_type_t **params,
                                       uint32_t num_params, bool vararg,
                                       bool is_decl, uint32_t *out_symbol_id);

#endif
