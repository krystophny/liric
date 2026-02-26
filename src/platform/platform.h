#ifndef LIRIC_PLATFORM_RUNTIME_H
#define LIRIC_PLATFORM_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum lr_platform_intrinsic_strategy {
    LR_PLATFORM_INTRINSIC_UNSUPPORTED = 0,
    LR_PLATFORM_INTRINSIC_BLOB = 1,
    LR_PLATFORM_INTRINSIC_LIBC = 2,
    LR_PLATFORM_INTRINSIC_BUILTIN = 3,
    LR_PLATFORM_INTRINSIC_TARGET_LOWER = 4,
} lr_platform_intrinsic_strategy_t;

typedef struct lr_platform_intrinsic_info {
    const char *canonical_name;
    const char *libc_name;
    const uint8_t *blob_begin;
    const uint8_t *blob_end;
    lr_platform_intrinsic_strategy_t preferred_strategy;
    bool known;
    bool has_blob;
    bool has_builtin;
} lr_platform_intrinsic_info_t;

/* Canonicalize an intrinsic symbol name (strips linker decoration prefixes). */
const char *lr_platform_intrinsic_canonical_name(const char *name);

/* Query registry metadata for an intrinsic symbol name. */
int lr_platform_intrinsic_lookup(const char *name,
                                 lr_platform_intrinsic_info_t *out_info);

/* Resolve intrinsic symbol address via libc/builtin/runtime-handle fallback. */
void *lr_platform_intrinsic_resolve_addr(const char *name, void *runtime_handle);

/* True when intrinsic is known by the platform compatibility layer. */
bool lr_platform_intrinsic_is_supported(const char *name);

/* Enumerate registered exact intrinsic names. */
size_t lr_platform_intrinsic_registry_count(void);
const char *lr_platform_intrinsic_registry_name(size_t idx);

/* Legacy wrappers kept for existing users. */
bool lr_platform_intrinsic_supported(const char *name);
bool lr_platform_intrinsic_blob_lookup(const char *name,
                                       const uint8_t **begin,
                                       const uint8_t **end);

size_t lr_platform_intrinsic_count(void);
const char *lr_platform_intrinsic_name(size_t idx);

/* Map an LLVM intrinsic name to its libc equivalent (e.g. "llvm.fabs.f32" ->
   "fabsf").  Returns the original name when no mapping exists. */
const char *lr_platform_intrinsic_libc_name(const char *name);

/* Target-aware blob lookup: retrieve blobs matching a specific target ISA
   (e.g. "riscv64gc", "aarch64", "x86_64") rather than the host platform.
   Returns true when a blob is available for the named target. */
bool lr_platform_intrinsic_supported_for_target(const char *name,
                                                 const char *target_name);
bool lr_platform_intrinsic_blob_lookup_for_target(const char *name,
                                                   const char *target_name,
                                                   const uint8_t **begin,
                                                   const uint8_t **end);

#endif
