/**
 * @file ioc_sha256.h
 * @brief SHA-256 e HMAC-SHA256 portáteis (C11, sem dependências).
 *
 * Usado pelo sensor wireless (Pico SDK) e pelos Nodes CAN (ESP32/Arduino)
 * para que os dois lados do enlace calculem exatamente o mesmo código de
 * autenticação. A implementação é validada por vetores do NIST/RFC 4231
 * em tests/test_ioc_link.c e cruzada com hashlib/hmac do Python.
 */
#ifndef IOC_SHA256_H
#define IOC_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IOC_SHA256_DIGEST_LEN 32
#define IOC_SHA256_BLOCK_LEN  64

typedef struct {
    uint32_t state[8];
    uint64_t bit_len;
    uint8_t  block[IOC_SHA256_BLOCK_LEN];
    uint32_t block_len;
} ioc_sha256_ctx;

void ioc_sha256_init(ioc_sha256_ctx *ctx);
void ioc_sha256_update(ioc_sha256_ctx *ctx, const uint8_t *data, size_t len);
void ioc_sha256_final(ioc_sha256_ctx *ctx, uint8_t out[IOC_SHA256_DIGEST_LEN]);

typedef struct {
    ioc_sha256_ctx inner;
    uint8_t        opad[IOC_SHA256_BLOCK_LEN];
} ioc_hmac_ctx;

void ioc_hmac_init(ioc_hmac_ctx *ctx, const uint8_t *key, size_t key_len);
void ioc_hmac_update(ioc_hmac_ctx *ctx, const uint8_t *data, size_t len);
void ioc_hmac_final(ioc_hmac_ctx *ctx, uint8_t out[IOC_SHA256_DIGEST_LEN]);

void ioc_hmac_sha256(const uint8_t *key, size_t key_len,
                     const uint8_t *msg, size_t msg_len,
                     uint8_t out[IOC_SHA256_DIGEST_LEN]);

/** Comparação em tempo constante. Retorna 1 quando os buffers são iguais. */
int ioc_ct_equal(const uint8_t *a, const uint8_t *b, size_t len);

/** Zera material sensível sem que o compilador elimine a escrita. */
void ioc_secure_zero(void *ptr, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* IOC_SHA256_H */
