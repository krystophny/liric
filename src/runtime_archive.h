#ifndef LIRIC_RUNTIME_ARCHIVE_H
#define LIRIC_RUNTIME_ARCHIVE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct lr_runtime_archive_info {
    uint32_t version;
    uint32_t backend;
    const char *target_name;
    size_t target_name_len;
    const char *ir_text;
    size_t ir_len;
    const uint8_t *blob_pkg;
    size_t blob_pkg_len;
} lr_runtime_archive_info_t;

int lr_runtime_archive_write(FILE *out,
                             const char *target_name,
                             uint32_t backend,
                             const char *ir_text,
                             size_t ir_len,
                             const uint8_t *blob_pkg,
                             size_t blob_pkg_len);

int lr_runtime_archive_parse(const uint8_t *data,
                             size_t len,
                             lr_runtime_archive_info_t *out_info);

#endif
