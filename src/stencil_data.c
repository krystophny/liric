#include "stencil_data.h"

#include <string.h>

#ifdef LIRIC_HAVE_GENERATED_STENCILS
#include "stencil_data_x86_64.h"
#endif

size_t lr_stencil_count_generated(void) {
#ifdef LIRIC_HAVE_GENERATED_STENCILS
    return lr_generated_stencils_count;
#else
    return 0;
#endif
}

const lr_stencil_t *lr_stencil_at_generated(size_t index) {
#ifdef LIRIC_HAVE_GENERATED_STENCILS
    if (index >= lr_generated_stencils_count) {
        return NULL;
    }
    return lr_generated_stencils[index];
#else
    (void)index;
    return NULL;
#endif
}

const lr_stencil_t *lr_stencil_lookup_generated(const char *name) {
#ifdef LIRIC_HAVE_GENERATED_STENCILS
    size_t i;
    if (!name) {
        return NULL;
    }
    for (i = 0; i < lr_generated_stencils_count; i++) {
        const lr_stencil_t *st = lr_generated_stencils[i];
        if (st && st->name && strcmp(st->name, name) == 0) {
            return st;
        }
    }
#else
    (void)name;
#endif
    return NULL;
}
