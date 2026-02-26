/* FIPS 180-4 SHA-256 -- minimal, no-dependency implementation. */
#include "sha256.h"
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

#define RR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(e, f, g) (((e) & (f)) ^ (~(e) & (g)))
#define MAJ(a, b, c) (((a) & (b)) ^ ((a) & (c)) ^ ((b) & (c)))
#define S0(a) (RR(a, 2) ^ RR(a, 13) ^ RR(a, 22))
#define S1(e) (RR(e, 6) ^ RR(e, 11) ^ RR(e, 25))
#define SIG0(x) (RR(x, 7) ^ RR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (RR(x, 17) ^ RR(x, 19) ^ ((x) >> 10))

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static void put_be64(uint8_t *p, uint64_t v) {
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

static void compress(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = be32(block + i * 4);
    for (int i = 16; i < 64; i++)
        w[i] = SIG1(w[i - 2]) + w[i - 7] + SIG0(w[i - 15]) + w[i - 16];
    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + S1(e) + CH(e, f, g) + K[i] + w[i];
        uint32_t t2 = S0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void lr_sha256_init(lr_sha256_ctx_t *ctx) {
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->total = 0;
    ctx->buflen = 0;
}

void lr_sha256_update(lr_sha256_ctx_t *ctx, const void *data, size_t len) {
    const uint8_t *src = (const uint8_t *)data;
    ctx->total += len;
    if (ctx->buflen > 0) {
        size_t fill = LR_SHA256_BLOCK_LEN - ctx->buflen;
        if (len < fill) {
            memcpy(ctx->buf + ctx->buflen, src, len);
            ctx->buflen += len;
            return;
        }
        memcpy(ctx->buf + ctx->buflen, src, fill);
        compress(ctx->state, ctx->buf);
        src += fill;
        len -= fill;
        ctx->buflen = 0;
    }
    while (len >= LR_SHA256_BLOCK_LEN) {
        compress(ctx->state, src);
        src += LR_SHA256_BLOCK_LEN;
        len -= LR_SHA256_BLOCK_LEN;
    }
    if (len > 0) {
        memcpy(ctx->buf, src, len);
        ctx->buflen = len;
    }
}

void lr_sha256_final(lr_sha256_ctx_t *ctx, uint8_t digest[LR_SHA256_DIGEST_LEN]) {
    uint64_t bits = ctx->total * 8;
    uint8_t pad = (uint8_t)(1u + (size_t)(119 - ctx->buflen) % 64);
    uint8_t zeros[72];
    memset(zeros, 0, sizeof(zeros));
    zeros[0] = 0x80;
    lr_sha256_update(ctx, zeros, pad);
    uint8_t tail[8];
    put_be64(tail, bits);
    lr_sha256_update(ctx, tail, 8);
    for (int i = 0; i < 8; i++)
        put_be32(digest + i * 4, ctx->state[i]);
}

void lr_sha256_oneshot(const void *data, size_t len,
                       uint8_t digest[LR_SHA256_DIGEST_LEN]) {
    lr_sha256_ctx_t ctx;
    lr_sha256_init(&ctx);
    lr_sha256_update(&ctx, data, len);
    lr_sha256_final(&ctx, digest);
}
