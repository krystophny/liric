#include "compile_mode.h"

#include <stdlib.h>
#include <string.h>

int lr_compile_mode_parse(const char *text, lr_compile_mode_t *out_mode) {
    if (!text || !out_mode)
        return -1;

    if (strcmp(text, "isel") == 0) {
        *out_mode = LR_COMPILE_ISEL;
        return 0;
    }
    if (strcmp(text, "copy_patch") == 0) {
        *out_mode = LR_COMPILE_COPY_PATCH;
        return 0;
    }
    if (strcmp(text, "stencil") == 0) {
        *out_mode = LR_COMPILE_COPY_PATCH;
        return 0;
    }
    if (strcmp(text, "llvm") == 0) {
        *out_mode = LR_COMPILE_LLVM;
        return 0;
    }
    return -1;
}

lr_compile_mode_t lr_compile_mode_from_env(void) {
    lr_compile_mode_t mode = LR_COMPILE_ISEL;
    const char *env = getenv("LIRIC_COMPILE_MODE");
    if (env)
        (void)lr_compile_mode_parse(env, &mode);
    return mode;
}

const char *lr_compile_mode_name(lr_compile_mode_t mode) {
    switch (mode) {
    case LR_COMPILE_ISEL:
        return "isel";
    case LR_COMPILE_COPY_PATCH:
        return "copy_patch";
    case LR_COMPILE_LLVM:
        return "llvm";
    default:
        return "unknown";
    }
}
