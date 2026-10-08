#include "ioc_secpdu.h"

#include <string.h>

#include "ioc_sha256.h"

static void compute_mac(const uint8_t key[IOC_LINK_KEY_LEN], uint16_t data_id,
                        const uint8_t *payload, size_t payload_len, uint64_t freshness,
                        uint8_t digest[IOC_SHA256_DIGEST_LEN]) {
    uint8_t id_be[2] = {(uint8_t)(data_id >> 8), (uint8_t)data_id};
    uint8_t fresh_be[8];
    ioc_hmac_ctx ctx;
    for (int i = 0; i < 8; ++i) fresh_be[i] = (uint8_t)(freshness >> (56 - 8 * i));
    ioc_hmac_init(&ctx, key, IOC_LINK_KEY_LEN);
    ioc_hmac_update(&ctx, id_be, sizeof(id_be));
    if (payload_len > 0) ioc_hmac_update(&ctx, payload, payload_len);
    ioc_hmac_update(&ctx, fresh_be, sizeof(fresh_be));
    ioc_hmac_final(&ctx, digest);
}

size_t ioc_secpdu_protect(const uint8_t key[IOC_LINK_KEY_LEN], uint16_t data_id,
                          const uint8_t *payload, size_t payload_len,
                          uint64_t *tx_freshness, uint8_t mac_len,
                          uint8_t *out, size_t out_cap) {
    uint8_t digest[IOC_SHA256_DIGEST_LEN];
    size_t total = payload_len + IOC_SECPDU_OVERHEAD(mac_len);
    if (!key || !tx_freshness || !out || (payload_len > 0 && !payload)) return 0;
    if (mac_len == 0 || mac_len > IOC_SECPDU_MAC_MAX || out_cap < total) return 0;
    if (*tx_freshness == UINT64_MAX) return 0; /* nunca reutiliza um valor */

    uint64_t freshness = *tx_freshness + 1u;
    compute_mac(key, data_id, payload, payload_len, freshness, digest);
    if (payload_len > 0) memmove(out, payload, payload_len);
    out[payload_len] = (uint8_t)freshness;
    memcpy(&out[payload_len + IOC_SECPDU_FRESHNESS_LEN], digest, mac_len);
    ioc_secure_zero(digest, sizeof(digest));
    *tx_freshness = freshness;
    return total;
}

int ioc_secpdu_verify(const uint8_t key[IOC_LINK_KEY_LEN], uint16_t data_id,
                      const uint8_t *pdu, size_t pdu_len,
                      uint64_t *rx_freshness, uint8_t mac_len,
                      size_t *payload_len) {
    uint8_t digest[IOC_SHA256_DIGEST_LEN];
    if (!key || !pdu || !rx_freshness) return 0;
    if (mac_len == 0 || mac_len > IOC_SECPDU_MAC_MAX) return 0;
    if (pdu_len < IOC_SECPDU_OVERHEAD(mac_len)) return 0;

    const size_t n = pdu_len - IOC_SECPDU_OVERHEAD(mac_len);
    const uint8_t truncated = pdu[n];

    /* Menor valor maior que o último aceito cujo byte baixo coincide. */
    uint64_t last = *rx_freshness;
    uint64_t candidate = (last & ~(uint64_t)0xFF) | truncated;
    if (candidate <= last) {
        if (candidate > UINT64_MAX - 0x100u) return 0;
        candidate += 0x100u;
    }
    if (candidate - last > IOC_SECPDU_WINDOW) return 0;

    compute_mac(key, data_id, pdu, n, candidate, digest);
    int ok = ioc_ct_equal(digest, &pdu[n + IOC_SECPDU_FRESHNESS_LEN], mac_len);
    ioc_secure_zero(digest, sizeof(digest));
    if (!ok) return 0;

    *rx_freshness = candidate;
    if (payload_len) *payload_len = n;
    return 1;
}

void ioc_secpdu_derive_bus_key(const uint8_t master[IOC_LINK_KEY_LEN],
                               uint8_t out[IOC_LINK_KEY_LEN]) {
    static const char label[] = "ioc-can-v1";
    ioc_hmac_sha256(master, IOC_LINK_KEY_LEN, (const uint8_t *)label, sizeof(label) - 1, out);
}
