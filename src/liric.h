#ifndef LIRIC_LIRIC_H
#define LIRIC_LIRIC_H

#include "ir.h"
#include <stddef.h>
#include <stdint.h>

lr_module_t *lr_parse_ll(const char *src, size_t len, char *err, size_t errlen);
lr_module_t *lr_parse_wasm(const uint8_t *data, size_t len, char *err, size_t errlen);
void lr_module_free(lr_module_t *m);

#endif
