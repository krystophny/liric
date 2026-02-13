#ifndef LIRIC_PLATFORM_OS_H
#define LIRIC_PLATFORM_OS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void *lr_platform_alloc_jit_code(size_t len, bool *out_map_jit_enabled);
void *lr_platform_alloc_rw(size_t len);
int lr_platform_free_pages(void *ptr, size_t len);

int lr_platform_jit_make_writable(void *code, size_t len, bool map_jit_enabled);
int lr_platform_jit_make_executable(void *code, size_t len, bool map_jit_enabled,
                                    const void *clear_begin, const void *clear_end);

uint64_t lr_platform_time_ns(void);

void *lr_platform_dlopen(const char *path);
int lr_platform_dlclose(void *handle);
void *lr_platform_dlsym(void *handle, const char *name);
void *lr_platform_dlsym_default(const char *name);

int lr_platform_run_process(char *const argv[], bool quiet, int *out_status);

#endif
