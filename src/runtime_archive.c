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
                             const char *target_name,
                             uint32_t backend,
                             const char *ir_text,
                             size_t ir_len,
                             const uint8_t *blob_pkg,
                             size_t blob_pkg_len) {
    size_t target_name_len = 0;
    uint8_t header[8 + 4 + 4 + 4 + 8 + 8];
    uint8_t *p = header;

    if (!out || !target_name || !target_name[0] || backend == 0 ||
        !ir_text || ir_len == 0 || !blob_pkg || blob_pkg_len == 0) {
        return -1;
    }
    target_name_len = strlen(target_name);
    if (target_name_len == 0 || target_name_len > UINT32_MAX)
        return -1;

    memcpy(p, k_runtime_archive_magic, sizeof(k_runtime_archive_magic));
    p += sizeof(k_runtime_archive_magic);
    runtime_archive_w32(&p, 2u);
    runtime_archive_w32(&p, backend);
    runtime_archive_w32(&p, (uint32_t)target_name_len);
    runtime_archive_w64(&p, (uint64_t)ir_len);
    runtime_archive_w64(&p, (uint64_t)blob_pkg_len);

    if (fwrite(header, 1, sizeof(header), out) != sizeof(header))
        return -1;
    if (fwrite(target_name, 1, target_name_len, out) != target_name_len)
        return -1;
    if (fwrite(ir_text, 1, ir_len, out) != ir_len)
        return -1;
    if (fwrite(blob_pkg, 1, blob_pkg_len, out) != blob_pkg_len)
        return -1;
    return 0;
}

int lr_runtime_archive_parse(const uint8_t *data,
                             size_t len,
                             lr_runtime_archive_info_t *out_info) {
    const uint8_t *p = data;
    const uint8_t *end = data + len;
    uint32_t version = 0;
    uint32_t backend = 0;
    uint32_t target_name_len_u32 = 0;
    uint64_t ir_len_u64 = 0;
    uint64_t blob_len_u64 = 0;

    if (!data || len < 32 || !out_info) {
        return -1;
    }
    memset(out_info, 0, sizeof(*out_info));
    if (memcmp(p, k_runtime_archive_magic, sizeof(k_runtime_archive_magic)) != 0)
        return -1;
    p += sizeof(k_runtime_archive_magic);
    if (runtime_archive_r32(&p, end, &version) != 0 || version != 2u)
        return -1;
    if (runtime_archive_r32(&p, end, &backend) != 0 || backend == 0)
        return -1;
    if (runtime_archive_r32(&p, end, &target_name_len_u32) != 0 ||
        target_name_len_u32 == 0) {
        return -1;
    }
    if (runtime_archive_r64(&p, end, &ir_len_u64) != 0 ||
        runtime_archive_r64(&p, end, &blob_len_u64) != 0) {
        return -1;
    }
    if (ir_len_u64 == 0 || blob_len_u64 == 0)
        return -1;
    if ((uint64_t)target_name_len_u32 > (uint64_t)(end - p))
        return -1;
    out_info->version = version;
    out_info->backend = backend;
    out_info->target_name = (const char *)p;
    out_info->target_name_len = (size_t)target_name_len_u32;
    p += (size_t)target_name_len_u32;
    if (ir_len_u64 > (uint64_t)(end - p))
        return -1;
    out_info->ir_text = (const char *)p;
    out_info->ir_len = (size_t)ir_len_u64;
    p += (size_t)ir_len_u64;
    if (blob_len_u64 > (uint64_t)(end - p))
        return -1;
    out_info->blob_pkg = p;
    out_info->blob_pkg_len = (size_t)blob_len_u64;
    p += (size_t)blob_len_u64;
    if (p != end)
        return -1;
    return 0;
}
