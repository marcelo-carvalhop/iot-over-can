#include "ioc_link.h"

#include <string.h>

#include "ioc_sha256.h"

static void put_u64_be(uint8_t *out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out[i] = (uint8_t)(v >> (56 - 8 * i));
}

static uint64_t get_u64_be(const uint8_t *in) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | in[i];
    return v;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int ioc_link_hex_to_key(const char *hex, uint8_t out[IOC_LINK_KEY_LEN]) {
    if (!hex) return 0;
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2;
    if (strlen(hex) != IOC_LINK_KEY_LEN * 2u) return 0;
    for (unsigned i = 0; i < IOC_LINK_KEY_LEN; ++i) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            memset(out, 0, IOC_LINK_KEY_LEN);
            return 0;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

int ioc_link_key_is_zero(const uint8_t key[IOC_LINK_KEY_LEN]) {
    uint8_t acc = 0;
    for (unsigned i = 0; i < IOC_LINK_KEY_LEN; ++i) acc |= key[i];
    return acc == 0;
}

void ioc_link_derive_device_key(const uint8_t master[IOC_LINK_KEY_LEN], uint64_t uuid,
                                uint8_t out[IOC_LINK_KEY_LEN]) {
    static const char label[] = "ioc-dev-v1";
    uint8_t uuid_be[8];
    ioc_hmac_ctx ctx;
    put_u64_be(uuid_be, uuid);
    ioc_hmac_init(&ctx, master, IOC_LINK_KEY_LEN);
    ioc_hmac_update(&ctx, (const uint8_t *)label, sizeof(label) - 1);
    ioc_hmac_update(&ctx, uuid_be, sizeof(uuid_be));
    ioc_hmac_final(&ctx, out);
}

void ioc_link_derive_network_key(const uint8_t master[IOC_LINK_KEY_LEN],
                                 uint8_t out[IOC_LINK_KEY_LEN]) {
    static const char label[] = "ioc-net-v1";
    ioc_hmac_sha256(master, IOC_LINK_KEY_LEN, (const uint8_t *)label, sizeof(label) - 1, out);
}

void ioc_link_derive_session_key(const uint8_t dev_key[IOC_LINK_KEY_LEN], uint64_t uuid,
                                 uint8_t node_id,
                                 const uint8_t sensor_nonce[IOC_LINK_NONCE_LEN],
                                 const uint8_t node_nonce[IOC_LINK_NONCE_LEN],
                                 uint8_t out[IOC_LINK_KEY_LEN]) {
    static const char label[] = "ioc-sess-v1";
    uint8_t uuid_be[8];
    ioc_hmac_ctx ctx;
    put_u64_be(uuid_be, uuid);
    ioc_hmac_init(&ctx, dev_key, IOC_LINK_KEY_LEN);
    ioc_hmac_update(&ctx, (const uint8_t *)label, sizeof(label) - 1);
    ioc_hmac_update(&ctx, uuid_be, sizeof(uuid_be));
    ioc_hmac_update(&ctx, &node_id, 1);
    ioc_hmac_update(&ctx, sensor_nonce, IOC_LINK_NONCE_LEN);
    ioc_hmac_update(&ctx, node_nonce, IOC_LINK_NONCE_LEN);
    ioc_hmac_final(&ctx, out);
}

void ioc_link_wifi_ssid(uint8_t node_id, char out[IOC_LINK_SSID_LEN]) {
    static const char digits[] = "0123456789";
    out[0] = 'I'; out[1] = 'O'; out[2] = 'C'; out[3] = '-';
    out[4] = digits[(node_id / 10u) % 10u];
    out[5] = digits[node_id % 10u];
    out[6] = '\0';
    out[7] = '\0';
}

void ioc_link_wifi_psk(const uint8_t net_key[IOC_LINK_KEY_LEN], uint8_t node_id,
                       char out[IOC_LINK_PSK_LEN]) {
    static const char label[] = "ioc-psk-v1";
    static const char hex[] = "0123456789abcdef";
    uint8_t digest[IOC_SHA256_DIGEST_LEN];
    ioc_hmac_ctx ctx;
    ioc_hmac_init(&ctx, net_key, IOC_LINK_KEY_LEN);
    ioc_hmac_update(&ctx, (const uint8_t *)label, sizeof(label) - 1);
    ioc_hmac_update(&ctx, &node_id, 1);
    ioc_hmac_final(&ctx, digest);
    for (unsigned i = 0; i < 16; ++i) {
        out[i * 2]     = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0Fu];
    }
    out[32] = '\0';
    ioc_secure_zero(digest, sizeof(digest));
}

uint8_t ioc_link_wifi_channel(uint8_t node_id) {
    static const uint8_t channels[3] = {1, 6, 11};
    return channels[node_id % 3u];
}

/* ------------------------------------------------------------------ oferta */

#define OFFER_SIGNED_OFFSET 2
#define OFFER_SIGNED_LEN    17 /* 'I' 'O' ver node uuid(8) canal nonce(4) */

static void offer_tag(const uint8_t dev_key[IOC_LINK_KEY_LEN], const uint8_t *signed_bytes,
                      uint8_t tag[IOC_LINK_TAG_LEN]) {
    static const char label[] = "ioc-offer-v1";
    uint8_t digest[IOC_SHA256_DIGEST_LEN];
    ioc_hmac_ctx ctx;
    ioc_hmac_init(&ctx, dev_key, IOC_LINK_KEY_LEN);
    ioc_hmac_update(&ctx, (const uint8_t *)label, sizeof(label) - 1);
    ioc_hmac_update(&ctx, signed_bytes, OFFER_SIGNED_LEN);
    ioc_hmac_final(&ctx, digest);
    memcpy(tag, digest, IOC_LINK_TAG_LEN);
    ioc_secure_zero(digest, sizeof(digest));
}

size_t ioc_offer_build(const uint8_t dev_key[IOC_LINK_KEY_LEN], const ioc_offer_t *offer,
                       uint8_t out[IOC_OFFER_MFG_LEN]) {
    if (!offer) return 0;
    out[0] = 0xFF;
    out[1] = 0xFF;
    out[2] = 'I';
    out[3] = 'O';
    out[4] = IOC_OFFER_VERSION;
    out[5] = offer->node_id;
    put_u64_be(&out[6], offer->uuid);
    out[14] = offer->channel;
    out[15] = (uint8_t)(offer->nonce >> 24);
    out[16] = (uint8_t)(offer->nonce >> 16);
    out[17] = (uint8_t)(offer->nonce >> 8);
    out[18] = (uint8_t)(offer->nonce);
    offer_tag(dev_key, &out[OFFER_SIGNED_OFFSET], &out[19]);
    return IOC_OFFER_MFG_LEN;
}

int ioc_offer_peek(const uint8_t *mfg, size_t len, ioc_offer_t *out) {
    if (!mfg || !out || len < IOC_OFFER_MFG_LEN) return 0;
    if (mfg[0] != 0xFF || mfg[1] != 0xFF || mfg[2] != 'I' || mfg[3] != 'O') return 0;
    if (mfg[4] != IOC_OFFER_VERSION) return 0;
    out->node_id = mfg[5];
    out->uuid = get_u64_be(&mfg[6]);
    out->channel = mfg[14];
    out->nonce = ((uint32_t)mfg[15] << 24) | ((uint32_t)mfg[16] << 16) |
                 ((uint32_t)mfg[17] << 8) | (uint32_t)mfg[18];
    return out->uuid != 0 && out->node_id != 0;
}

int ioc_offer_verify(const uint8_t dev_key[IOC_LINK_KEY_LEN], const uint8_t *mfg, size_t len,
                     ioc_offer_t *out) {
    uint8_t expected[IOC_LINK_TAG_LEN];
    ioc_offer_t parsed;
    if (!ioc_offer_peek(mfg, len, &parsed)) return 0;
    offer_tag(dev_key, &mfg[OFFER_SIGNED_OFFSET], expected);
    if (!ioc_ct_equal(expected, &mfg[19], IOC_LINK_TAG_LEN)) return 0;
    if (out) *out = parsed;
    return 1;
}

/* ---------------------------------------------------------------- envelope */

static void env_tag(const uint8_t key[IOC_LINK_KEY_LEN], const uint8_t *buf, size_t signed_len,
                    uint8_t tag[IOC_LINK_TAG_LEN]) {
    uint8_t digest[IOC_SHA256_DIGEST_LEN];
    ioc_hmac_sha256(key, IOC_LINK_KEY_LEN, buf, signed_len, digest);
    memcpy(tag, digest, IOC_LINK_TAG_LEN);
    ioc_secure_zero(digest, sizeof(digest));
}

size_t ioc_env_seal(const uint8_t key[IOC_LINK_KEY_LEN], uint8_t type, uint32_t counter,
                    const uint8_t *payload, size_t payload_len,
                    uint8_t *out, size_t out_cap) {
    size_t total = IOC_ENV_OVERHEAD + payload_len;
    if (!out || out_cap < total || (payload_len > 0 && !payload)) return 0;
    out[0] = IOC_ENV_MAGIC_0;
    out[1] = IOC_ENV_MAGIC_1;
    out[2] = IOC_LINK_VERSION;
    out[3] = type;
    out[4] = (uint8_t)(counter);
    out[5] = (uint8_t)(counter >> 8);
    out[6] = (uint8_t)(counter >> 16);
    out[7] = (uint8_t)(counter >> 24);
    if (payload_len > 0) memmove(&out[IOC_ENV_HEADER_LEN], payload, payload_len);
    env_tag(key, out, IOC_ENV_HEADER_LEN + payload_len, &out[IOC_ENV_HEADER_LEN + payload_len]);
    return total;
}

int ioc_env_parse(const uint8_t *buf, size_t len, ioc_env_view *view) {
    if (!buf || !view || len < IOC_ENV_OVERHEAD) return 0;
    if (buf[0] != IOC_ENV_MAGIC_0 || buf[1] != IOC_ENV_MAGIC_1) return 0;
    if (buf[2] != IOC_LINK_VERSION) return 0;
    view->type = buf[3];
    view->counter = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
                    ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    view->payload = &buf[IOC_ENV_HEADER_LEN];
    view->payload_len = len - IOC_ENV_OVERHEAD;
    return 1;
}

int ioc_env_verify(const uint8_t key[IOC_LINK_KEY_LEN], const uint8_t *buf, size_t len) {
    uint8_t expected[IOC_LINK_TAG_LEN];
    if (!buf || len < IOC_ENV_OVERHEAD) return 0;
    env_tag(key, buf, len - IOC_LINK_TAG_LEN, expected);
    return ioc_ct_equal(expected, &buf[len - IOC_LINK_TAG_LEN], IOC_LINK_TAG_LEN);
}

int ioc_counter_accept(uint32_t *last_accepted, uint32_t counter) {
    if (!last_accepted || counter == 0 || counter <= *last_accepted) return 0;
    *last_accepted = counter;
    return 1;
}

/* --------------------------------------------------------------- handshake */

void ioc_hello_encode(const ioc_hello_t *in, uint8_t out[IOC_HELLO_LEN]) {
    put_u64_be(&out[0], in->uuid);
    out[8] = in->profile;
    out[9] = in->protocol;
    memcpy(&out[10], in->sensor_nonce, IOC_LINK_NONCE_LEN);
}

int ioc_hello_decode(const uint8_t *buf, size_t len, ioc_hello_t *out) {
    if (!buf || !out || len != IOC_HELLO_LEN) return 0;
    out->uuid = get_u64_be(&buf[0]);
    out->profile = buf[8];
    out->protocol = buf[9];
    memcpy(out->sensor_nonce, &buf[10], IOC_LINK_NONCE_LEN);
    return out->uuid != 0;
}

void ioc_challenge_encode(const ioc_challenge_t *in, uint8_t out[IOC_CHALLENGE_LEN]) {
    out[0] = in->node_id;
    out[1] = in->child_id;
    memcpy(&out[2], in->node_nonce, IOC_LINK_NONCE_LEN);
    memcpy(&out[10], in->sensor_nonce, IOC_LINK_NONCE_LEN);
}

int ioc_challenge_decode(const uint8_t *buf, size_t len, ioc_challenge_t *out) {
    if (!buf || !out || len != IOC_CHALLENGE_LEN) return 0;
    out->node_id = buf[0];
    out->child_id = buf[1];
    memcpy(out->node_nonce, &buf[2], IOC_LINK_NONCE_LEN);
    memcpy(out->sensor_nonce, &buf[10], IOC_LINK_NONCE_LEN);
    return out->node_id != 0;
}

void ioc_confirm_encode(const ioc_confirm_t *in, uint8_t out[IOC_CONFIRM_LEN]) {
    memcpy(&out[0], in->node_nonce, IOC_LINK_NONCE_LEN);
    memcpy(&out[8], in->sensor_nonce, IOC_LINK_NONCE_LEN);
}

int ioc_confirm_decode(const uint8_t *buf, size_t len, ioc_confirm_t *out) {
    if (!buf || !out || len != IOC_CONFIRM_LEN) return 0;
    memcpy(out->node_nonce, &buf[0], IOC_LINK_NONCE_LEN);
    memcpy(out->sensor_nonce, &buf[8], IOC_LINK_NONCE_LEN);
    return 1;
}
