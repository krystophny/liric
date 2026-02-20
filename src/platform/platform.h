#ifndef LIRIC_PLATFORM_RUNTIME_H
#define LIRIC_PLATFORM_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Internal runtime intrinsic blob lookup used by executable and JIT emission. */
bool lr_platform_intrinsic_supported(const char *name);
bool lr_platform_intrinsic_blob_lookup(const char *name,
                                       const uint8_t **begin,
                                       const uint8_t **end);

size_t lr_platform_intrinsic_count(void);
const char *lr_platform_intrinsic_name(size_t idx);

/* Map an LLVM intrinsic name to its libc equivalent (e.g. "llvm.fabs.f32" ->
   "fabsf").  Returns the original name when no mapping exists. */
const char *lr_platform_intrinsic_libc_name(const char *name);

#endif
