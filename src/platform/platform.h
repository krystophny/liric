#ifndef LIRIC_PLATFORM_RUNTIME_H
#define LIRIC_PLATFORM_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Internal runtime intrinsic blob lookup used by direct executable emission. */
bool lr_platform_intrinsic_supported(const char *name);
bool lr_platform_intrinsic_blob_lookup(const char *name,
                                       const uint8_t **begin,
                                       const uint8_t **end);

#endif
