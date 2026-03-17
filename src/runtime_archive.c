#include "runtime_archive.h"

#include <string.h>

static const uint8_t k_runtime_archive_magic[8] = {
    'L', 'R', 'A', 'R', 'C', 'H', '1', '\0'
};

static void runtime_archive_w32(uint8_t **p, uint32_t v) {
    (*p)[0] = (uint8_t)(v);
    (*p)[1] = (uint8_t)(v >> 8);
    (*p)[2] = (uint8_t)(v >> 16);
    (*p)[3] = (uint8_t)(v >> 24);
    *p += 4;
}

static void runtime_archive_w64(uint8_t **p, uint64_t v) {
    for (int i = 0; i < 8; i++)
        (*p)[i] = (uint8_t)(v >> (i * 8));
    *p += 8;
}

static int runtime_archive_r32(const uint8_t **p,
                               const uint8_t *end,
                               uint32_t *out) {
    if (!p || !*p || !out || !end || (size_t)(end - *p) < 4)
        return -1;
    *out = (uint32_t)(*p)[0] |
           ((uint32_t)(*p)[1] << 8) |
           ((uint32_t)(*p)[2] << 16) |
           ((uint32_t)(*p)[3] << 24);
    *p += 4;
    return 0;
}

static int runtime_archive_r64(const uint8_t **p,
                               const uint8_t *end,
                               uint64_t *out) {
    uint64_t v = 0;
    if (!p || !*p || !out || !end || (size_t)(end - *p) < 8)
        return -1;
    for (int i = 0; i < 8; i++)
        v |= ((uint64_t)(*p)[i]) << (i * 8);
    *out = v;
    *p += 8;
    return 0;
}

int lr_runtime_archive_write(FILE *out,
                             const char *ir_text,
                             size_t ir_len,
                             const uint8_t *blob_pkg,
                             size_t blob_pkg_len) {
    uint8_t header[8 + 4 + 8 + 8];
    uint8_t *p = header;

    if (!out || !ir_text || ir_len == 0 || !blob_pkg || blob_pkg_len == 0)
        return -1;

    memcpy(p, k_runtime_archive_magic, sizeof(k_runtime_archive_magic));
    p += sizeof(k_runtime_archive_magic);
    runtime_archive_w32(&p, 1u);
    runtime_archive_w64(&p, (uint64_t)ir_len);
    runtime_archive_w64(&p, (uint64_t)blob_pkg_len);

    if (fwrite(header, 1, sizeof(header), out) != sizeof(header))
        return -1;
    if (fwrite(ir_text, 1, ir_len, out) != ir_len)
        return -1;
    if (fwrite(blob_pkg, 1, blob_pkg_len, out) != blob_pkg_len)
        return -1;
    return 0;
}

int lr_runtime_archive_parse(const uint8_t *data,
                             size_t len,
                             const char **out_ir_text,
                             size_t *out_ir_len,
                             const uint8_t **out_blob_pkg,
                             size_t *out_blob_pkg_len) {
    const uint8_t *p = data;
    const uint8_t *end = data + len;
    uint32_t version = 0;
    uint64_t ir_len_u64 = 0;
    uint64_t blob_len_u64 = 0;

    if (!data || len < 28 || !out_ir_text || !out_ir_len ||
        !out_blob_pkg || !out_blob_pkg_len) {
        return -1;
    }
    if (memcmp(p, k_runtime_archive_magic, sizeof(k_runtime_archive_magic)) != 0)
        return -1;
    p += sizeof(k_runtime_archive_magic);
    if (runtime_archive_r32(&p, end, &version) != 0 || version != 1u)
        return -1;
    if (runtime_archive_r64(&p, end, &ir_len_u64) != 0 ||
        runtime_archive_r64(&p, end, &blob_len_u64) != 0) {
        return -1;
    }
    if (ir_len_u64 == 0 || blob_len_u64 == 0)
        return -1;
    if (ir_len_u64 > (uint64_t)(end - p))
        return -1;
    *out_ir_text = (const char *)p;
    *out_ir_len = (size_t)ir_len_u64;
    p += (size_t)ir_len_u64;
    if (blob_len_u64 > (uint64_t)(end - p))
        return -1;
    *out_blob_pkg = p;
    *out_blob_pkg_len = (size_t)blob_len_u64;
    p += (size_t)blob_len_u64;
    if (p != end)
        return -1;
    return 0;
}
