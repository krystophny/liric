#ifndef LIRIC_SHA256_H
#define LIRIC_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define LR_SHA256_DIGEST_LEN 32
#define LR_SHA256_BLOCK_LEN 64

typedef struct {
    uint32_t state[8];
    uint8_t buf[LR_SHA256_BLOCK_LEN];
    uint64_t total;
    size_t buflen;
} lr_sha256_ctx_t;

void lr_sha256_init(lr_sha256_ctx_t *ctx);
void lr_sha256_update(lr_sha256_ctx_t *ctx, const void *data, size_t len);
void lr_sha256_final(lr_sha256_ctx_t *ctx, uint8_t digest[LR_SHA256_DIGEST_LEN]);
void lr_sha256_oneshot(const void *data, size_t len,
                       uint8_t digest[LR_SHA256_DIGEST_LEN]);

#endif
