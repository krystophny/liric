#ifndef LIRIC_LLVM_BACKEND_H
#define LIRIC_LLVM_BACKEND_H

#include "ir.h"
#include "target.h"
#include <stddef.h>

int lr_llvm_backend_is_available(void);

int lr_llvm_emit_object_path(lr_module_t *m, const lr_target_t *target,
                             const char *path, char *err, size_t err_cap);

int lr_llvm_emit_executable_path(lr_module_t *m, const char *runtime_ll,
                                 size_t runtime_len,
                                 const lr_target_t *target,
                                 const char *path,
                                 const char *entry_symbol,
                                 char *err, size_t err_cap);

#endif
