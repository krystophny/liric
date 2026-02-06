#include "target.h"
#include <string.h>

static const lr_target_t *target_from_name(const char *name) {
    if (!name || !name[0]) return NULL;

    if (strcmp(name, "x86_64") == 0)
        return lr_target_x86_64();

    if (strcmp(name, "aarch64") == 0 || strcmp(name, "arm64") == 0)
        return lr_target_aarch64();

    return NULL;
}

const lr_target_t *lr_target_by_name(const char *name) {
    return target_from_name(name);
}

const lr_target_t *lr_target_host(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return lr_target_x86_64();
#elif defined(__aarch64__) || defined(_M_ARM64)
    return lr_target_aarch64();
#else
    return NULL;
#endif
}

bool lr_target_is_host_compatible(const lr_target_t *t) {
    const lr_target_t *host = lr_target_host();
    return host && t && strcmp(host->name, t->name) == 0;
}
