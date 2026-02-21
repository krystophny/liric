#ifndef LIRIC_H
#define LIRIC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Opaque compiler handle -------------------------------------------- */

typedef struct lr_compiler lr_compiler_t;

/* ---- Unified Streaming Compiler API ------------------------------------ */

typedef enum lr_policy {
    LR_POLICY_DIRECT = 0,
    LR_POLICY_IR = 1,
} lr_policy_t;

typedef enum lr_backend {
    LR_BACKEND_ISEL = 0,
    LR_BACKEND_COPY_PATCH = 1,
    LR_BACKEND_LLVM = 2,
} lr_backend_t;

typedef struct lr_compiler_error {
    int code;
    char msg[256];
} lr_compiler_error_t;

enum {
    LR_COMPILER_OK = 0,
    LR_COMPILER_ERR_ARGUMENT = 1,
    LR_COMPILER_ERR_STATE = 2,
    LR_COMPILER_ERR_UNSUPPORTED = 3,
    LR_COMPILER_ERR_BACKEND = 4,
    LR_COMPILER_ERR_PARSE = 5,
    LR_COMPILER_ERR_NOT_FOUND = 6,
};

typedef struct lr_compiler_config {
    lr_policy_t policy;       /* default: LR_POLICY_DIRECT */
    lr_backend_t backend;     /* default: LR_BACKEND_ISEL */
    const char *target;       /* NULL = host */
} lr_compiler_config_t;

lr_compiler_t *lr_compiler_create(const lr_compiler_config_t *cfg,
                                  lr_compiler_error_t *err);
void lr_compiler_destroy(lr_compiler_t *c);

int lr_compiler_add_symbol(lr_compiler_t *c, const char *name, void *addr);
int lr_compiler_load_library(lr_compiler_t *c, const char *path,
                             lr_compiler_error_t *err);
int lr_compiler_set_runtime_bc(lr_compiler_t *c, const uint8_t *bc_data,
                               size_t bc_len, lr_compiler_error_t *err);

int lr_compiler_feed_ll(lr_compiler_t *c, const char *src, size_t len,
                        lr_compiler_error_t *err);
int lr_compiler_feed_bc(lr_compiler_t *c, const uint8_t *data, size_t len,
                        lr_compiler_error_t *err);
int lr_compiler_feed_wasm(lr_compiler_t *c, const uint8_t *data, size_t len,
                          lr_compiler_error_t *err);
int lr_compiler_feed_auto(lr_compiler_t *c, const uint8_t *data, size_t len,
                          lr_compiler_error_t *err);

void *lr_compiler_lookup(lr_compiler_t *c, const char *name);

int lr_compiler_emit_object(lr_compiler_t *c, const char *path,
                            lr_compiler_error_t *err);
int lr_compiler_emit_exe(lr_compiler_t *c, const char *path,
                         lr_compiler_error_t *err);
int lr_compiler_emit_exe_with_runtime(lr_compiler_t *c, const char *path,
                                      const char *runtime_ll, size_t runtime_len,
                                      lr_compiler_error_t *err);
lr_policy_t lr_compiler_policy(const lr_compiler_t *c);
lr_backend_t lr_compiler_backend(const lr_compiler_t *c);

#ifdef __cplusplus
}
#endif

#endif
