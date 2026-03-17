#ifndef LIRIC_RUNTIME_ARCHIVE_H
#define LIRIC_RUNTIME_ARCHIVE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

int lr_runtime_archive_write(FILE *out,
                             const char *ir_text,
                             size_t ir_len,
                             const uint8_t *blob_pkg,
                             size_t blob_pkg_len);

int lr_runtime_archive_parse(const uint8_t *data,
                             size_t len,
                             const char **out_ir_text,
                             size_t *out_ir_len,
                             const uint8_t **out_blob_pkg,
                             size_t *out_blob_pkg_len);

#endif
