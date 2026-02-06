#ifndef LIRIC_H
#define LIRIC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lr_module lr_module_t;
typedef struct lr_jit lr_jit_t;

lr_module_t *lr_parse_ll(const char *src, size_t len, char *err, size_t errlen);
void lr_module_free(lr_module_t *m);

lr_jit_t *lr_jit_create(void);
lr_jit_t *lr_jit_create_for_target(const char *target_name);
const char *lr_jit_host_target_name(void);
const char *lr_jit_target_name(const lr_jit_t *j);
void lr_jit_add_symbol(lr_jit_t *j, const char *name, void *addr);
int lr_jit_add_module(lr_jit_t *j, lr_module_t *m);
void *lr_jit_get_function(lr_jit_t *j, const char *name);
void lr_jit_destroy(lr_jit_t *j);

#ifdef __cplusplus
}
#endif

#endif
