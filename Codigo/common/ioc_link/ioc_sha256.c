#include "ioc_sha256.h"

#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_transform(ioc_sha256_ctx *ctx, const uint8_t block[IOC_SHA256_BLOCK_LEN]) {
    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (unsigned i = 0; i < 64; ++i) {
        uint32_t s1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + K[i] + w[i];
        uint32_t s0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void ioc_sha256_init(ioc_sha256_ctx *ctx) {
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->bit_len = 0;
    ctx->block_len = 0;
}

void ioc_sha256_update(ioc_sha256_ctx *ctx, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->block[ctx->block_len++] = data[i];
        if (ctx->block_len == IOC_SHA256_BLOCK_LEN) {
            sha256_transform(ctx, ctx->block);
            ctx->bit_len += 512u;
            ctx->block_len = 0;
        }
    }
}

void ioc_sha256_final(ioc_sha256_ctx *ctx, uint8_t out[IOC_SHA256_DIGEST_LEN]) {
    uint32_t i = ctx->block_len;
    ctx->bit_len += (uint64_t)ctx->block_len * 8u;

    ctx->block[i++] = 0x80u;
    if (i > 56) {
        while (i < IOC_SHA256_BLOCK_LEN) ctx->block[i++] = 0;
        sha256_transform(ctx, ctx->block);
        i = 0;
    }
    while (i < 56) ctx->block[i++] = 0;
    for (unsigned b = 0; b < 8; ++b) {
        ctx->block[56 + b] = (uint8_t)(ctx->bit_len >> (56 - 8 * b));
    }
    sha256_transform(ctx, ctx->block);

    for (unsigned w = 0; w < 8; ++w) {
        out[w * 4]     = (uint8_t)(ctx->state[w] >> 24);
        out[w * 4 + 1] = (uint8_t)(ctx->state[w] >> 16);
        out[w * 4 + 2] = (uint8_t)(ctx->state[w] >> 8);
        out[w * 4 + 3] = (uint8_t)(ctx->state[w]);
    }
    ioc_secure_zero(ctx, sizeof(*ctx));
}

void ioc_hmac_init(ioc_hmac_ctx *ctx, const uint8_t *key, size_t key_len) {
    uint8_t k[IOC_SHA256_BLOCK_LEN];
    uint8_t ipad[IOC_SHA256_BLOCK_LEN];
    memset(k, 0, sizeof(k));
    if (key_len > IOC_SHA256_BLOCK_LEN) {
        ioc_sha256_ctx h;
        ioc_sha256_init(&h);
        ioc_sha256_update(&h, key, key_len);
        ioc_sha256_final(&h, k);
    } else if (key_len > 0) {
        memcpy(k, key, key_len);
    }
    for (unsigned i = 0; i < IOC_SHA256_BLOCK_LEN; ++i) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36u);
        ctx->opad[i] = (uint8_t)(k[i] ^ 0x5cu);
    }
    ioc_sha256_init(&ctx->inner);
    ioc_sha256_update(&ctx->inner, ipad, sizeof(ipad));
    ioc_secure_zero(k, sizeof(k));
    ioc_secure_zero(ipad, sizeof(ipad));
}

void ioc_hmac_update(ioc_hmac_ctx *ctx, const uint8_t *data, size_t len) {
    ioc_sha256_update(&ctx->inner, data, len);
}

void ioc_hmac_final(ioc_hmac_ctx *ctx, uint8_t out[IOC_SHA256_DIGEST_LEN]) {
    uint8_t inner_digest[IOC_SHA256_DIGEST_LEN];
    ioc_sha256_ctx outer;
    ioc_sha256_final(&ctx->inner, inner_digest);
    ioc_sha256_init(&outer);
    ioc_sha256_update(&outer, ctx->opad, sizeof(ctx->opad));
    ioc_sha256_update(&outer, inner_digest, sizeof(inner_digest));
    ioc_sha256_final(&outer, out);
    ioc_secure_zero(inner_digest, sizeof(inner_digest));
    ioc_secure_zero(ctx, sizeof(*ctx));
}

void ioc_hmac_sha256(const uint8_t *key, size_t key_len,
                     const uint8_t *msg, size_t msg_len,
                     uint8_t out[IOC_SHA256_DIGEST_LEN]) {
    ioc_hmac_ctx ctx;
    ioc_hmac_init(&ctx, key, key_len);
    ioc_hmac_update(&ctx, msg, msg_len);
    ioc_hmac_final(&ctx, out);
}

int ioc_ct_equal(const uint8_t *a, const uint8_t *b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

void ioc_secure_zero(void *ptr, size_t len) {
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len--) *p++ = 0;
}
