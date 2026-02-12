#ifndef LIRIC_COMPILE_MODE_H
#define LIRIC_COMPILE_MODE_H

#include "target.h"

/* Parse mode text into enum; returns 0 on success. */
int lr_compile_mode_parse(const char *text, lr_compile_mode_t *out_mode);

/* Parse LIRIC_COMPILE_MODE from env; defaults to LR_COMPILE_ISEL. */
lr_compile_mode_t lr_compile_mode_from_env(void);

const char *lr_compile_mode_name(lr_compile_mode_t mode);

#endif
