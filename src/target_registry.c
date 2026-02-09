#include "target.h"
#include <string.h>

typedef struct lr_target_entry {
    const char *name;
    const lr_target_t *(*get_target)(void);
} lr_target_entry_t;

static const lr_target_entry_t g_targets[] = {
    { "x86_64", lr_target_x86_64 },
    { "aarch64", lr_target_aarch64 },
    { "arm64", lr_target_aarch64 },
};

const lr_target_t *lr_target_by_name(const char *name) {
    if (!name || !name[0])
        return NULL;

    size_t n = sizeof(g_targets) / sizeof(g_targets[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(g_targets[i].name, name) == 0)
            return g_targets[i].get_target();
    }
    return NULL;
}

const lr_target_t *lr_target_host(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return lr_target_by_name("x86_64");
#elif defined(__aarch64__) || defined(_M_ARM64)
    return lr_target_by_name("aarch64");
#else
    return NULL;
#endif
}

bool lr_target_is_host_compatible(const lr_target_t *t) {
    const lr_target_t *host = lr_target_host();
    return host && t && strcmp(host->name, t->name) == 0;
}
