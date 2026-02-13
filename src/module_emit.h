#ifndef LIRIC_MODULE_EMIT_H
#define LIRIC_MODULE_EMIT_H

#include "ir.h"
#include <stddef.h>
#include <stdio.h>

int lr_emit_module_object_path(lr_module_t *module,
                               const char *target_name,
                               const char *path,
                               char *err,
                               size_t err_cap);

int lr_emit_module_object_stream(lr_module_t *module,
                                 const char *target_name,
                                 FILE *out,
                                 char *err,
                                 size_t err_cap);

int lr_emit_module_executable_path(lr_module_t *module,
                                   const char *target_name,
                                   const char *path,
                                   const char *entry,
                                   const char *runtime_ll,
                                   size_t runtime_len,
                                   char *err,
                                   size_t err_cap);

#endif
