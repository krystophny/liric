#include "frontend_common.h"
#include <stdarg.h>
#include <stdio.h>

void lr_frontend_set_error(char *err, size_t errlen, const char *fmt, ...) {
    if (!err || errlen == 0 || !fmt)
        return;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

uint32_t lr_frontend_intern_symbol(lr_module_t *m, const char *name) {
    if (!m || !name || name[0] == '\0')
        return UINT32_MAX;
    return lr_module_intern_symbol(m, name);
}

lr_func_t *lr_frontend_create_function(lr_module_t *m, const char *name,
                                       lr_type_t *ret_type, lr_type_t **params,
                                       uint32_t num_params, bool vararg,
                                       bool is_decl, uint32_t *out_symbol_id) {
    lr_func_t *f;

    if (!m || !name || !ret_type)
        return NULL;

    if (out_symbol_id)
        *out_symbol_id = lr_frontend_intern_symbol(m, name);

    if (is_decl)
        f = lr_func_declare(m, name, ret_type, params, num_params, vararg);
    else
        f = lr_func_create(m, name, ret_type, params, num_params, vararg);
    return f;
}
