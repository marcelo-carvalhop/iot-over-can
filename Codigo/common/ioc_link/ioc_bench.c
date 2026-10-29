#include "ioc_bench.h"

#include <stdio.h>
#include <string.h>

#include "ioc_link.h"
#include "ioc_sha256.h"

static uint32_t g_samples[IOC_BENCH_MAX_N];

static uint32_t rank(const uint32_t *sorted, uint16_t n, uint16_t pct) {
    /* Posto mais próximo: menor valor com pelo menos pct% das amostras à esquerda. */
    uint32_t r = ((uint32_t)pct * n + 99u) / 100u;
    if (r < 1u) r = 1u;
    if (r > n) r = n;
    return sorted[r - 1u];
}

void ioc_bench_summarize(uint32_t *samples, uint16_t n, uint64_t total_us, ioc_bench_stats *out) {
    memset(out, 0, sizeof(*out));
    if (!n) return;
    for (uint16_t i = 1; i < n; ++i) {  /* inserção: n pequeno, sem alocação */
        const uint32_t v = samples[i];
        uint16_t j = i;
        while (j > 0 && samples[j - 1] > v) { samples[j] = samples[j - 1]; --j; }
        samples[j] = v;
    }
    out->n = n;
    out->mean_us = (double)total_us / (double)n;
    out->min_us = samples[0];
    out->max_us = samples[n - 1];
    out->p50_us = rank(samples, n, 50);
    out->p95_us = rank(samples, n, 95);
    out->p99_us = rank(samples, n, 99);
}

typedef struct {
    uint8_t master[IOC_LINK_KEY_LEN];
    uint8_t dev[IOC_LINK_KEY_LEN];
    uint8_t sess[IOC_LINK_KEY_LEN];
    uint64_t uuid;
    uint8_t data[1024];
    uint8_t hello[IOC_HELLO_LEN + IOC_ENV_OVERHEAD];
    uint8_t challenge[IOC_CHALLENGE_LEN + IOC_ENV_OVERHEAD];
    uint8_t confirm[IOC_CONFIRM_LEN + IOC_ENV_OVERHEAD];
    uint8_t datagram[IOC_BENCH_TELEMETRY + IOC_ENV_OVERHEAD];
    uint8_t sensor_nonce[IOC_LINK_NONCE_LEN];
    uint8_t node_nonce[IOC_LINK_NONCE_LEN];
    volatile uint8_t sink;
} bench_ctx;

static bench_ctx g_ctx;

enum { OP_HMAC, OP_SEAL, OP_VERIFY, OP_HS_NODE, OP_HS_SENSOR };

static void run_op(bench_ctx *c, int op, size_t bytes) {
    uint8_t mac[IOC_SHA256_DIGEST_LEN];
    switch (op) {
        case OP_HMAC:
            ioc_hmac_sha256(c->sess, sizeof(c->sess), c->data, bytes, mac);
            c->sink ^= mac[0];
            break;
        case OP_SEAL:
            c->sink ^= (uint8_t)ioc_env_seal(c->sess, IOC_MSG_DATA_UP, 7, c->data, IOC_BENCH_TELEMETRY,
                                              c->datagram, sizeof(c->datagram));
            break;
        case OP_VERIFY:
            c->sink ^= (uint8_t)ioc_env_verify(c->sess, c->datagram, sizeof(c->datagram));
            break;
        case OP_HS_NODE: {
            /* Node: chave do dispositivo, HELLO, chave de sessão, CHALLENGE e CONFIRM. */
            uint8_t dev[IOC_LINK_KEY_LEN], sess[IOC_LINK_KEY_LEN], raw[IOC_CHALLENGE_LEN], out[sizeof(c->challenge)];
            ioc_link_derive_device_key(c->master, c->uuid, dev);
            c->sink ^= (uint8_t)ioc_env_verify(dev, c->hello, sizeof(c->hello));
            ioc_link_derive_session_key(dev, c->uuid, 2, c->sensor_nonce, c->node_nonce, sess);
            ioc_challenge_t ch;
            ch.node_id = 2;
            ch.child_id = 1;
            memcpy(ch.node_nonce, c->node_nonce, sizeof(ch.node_nonce));
            memcpy(ch.sensor_nonce, c->sensor_nonce, sizeof(ch.sensor_nonce));
            ioc_challenge_encode(&ch, raw);
            c->sink ^= (uint8_t)ioc_env_seal(dev, IOC_MSG_CHALLENGE, 0, raw, sizeof(raw), out, sizeof(out));
            c->sink ^= (uint8_t)ioc_env_verify(sess, c->confirm, sizeof(c->confirm));
            ioc_secure_zero(dev, sizeof(dev));
            ioc_secure_zero(sess, sizeof(sess));
            break;
        }
        case OP_HS_SENSOR: {
            /* Sensor: HELLO, CHALLENGE, chave de sessão e CONFIRM (a chave do dispositivo já está gravada). */
            uint8_t sess[IOC_LINK_KEY_LEN], raw[IOC_HELLO_LEN], out[sizeof(c->hello)], cf[IOC_CONFIRM_LEN];
            uint8_t cfo[sizeof(c->confirm)];
            ioc_hello_t h;
            h.uuid = c->uuid;
            h.profile = 1;
            h.protocol = IOC_LINK_VERSION;
            memcpy(h.sensor_nonce, c->sensor_nonce, sizeof(h.sensor_nonce));
            ioc_hello_encode(&h, raw);
            c->sink ^= (uint8_t)ioc_env_seal(c->dev, IOC_MSG_HELLO, 0, raw, sizeof(raw), out, sizeof(out));
            c->sink ^= (uint8_t)ioc_env_verify(c->dev, c->challenge, sizeof(c->challenge));
            ioc_link_derive_session_key(c->dev, c->uuid, 2, c->sensor_nonce, c->node_nonce, sess);
            ioc_confirm_t conf;
            memcpy(conf.node_nonce, c->node_nonce, sizeof(conf.node_nonce));
            memcpy(conf.sensor_nonce, c->sensor_nonce, sizeof(conf.sensor_nonce));
            ioc_confirm_encode(&conf, cf);
            c->sink ^= (uint8_t)ioc_env_seal(sess, IOC_MSG_CONFIRM, 1, cf, sizeof(cf), cfo, sizeof(cfo));
            ioc_secure_zero(sess, sizeof(sess));
            break;
        }
        default:
            break;
    }
}

static void prepare(bench_ctx *c) {
    for (size_t i = 0; i < sizeof(c->master); ++i) c->master[i] = (uint8_t)(0xA0u + i);
    for (size_t i = 0; i < sizeof(c->data); ++i) c->data[i] = (uint8_t)(i * 37u + 11u);
    for (size_t i = 0; i < IOC_LINK_NONCE_LEN; ++i) {
        c->sensor_nonce[i] = (uint8_t)(0x10u + i);
        c->node_nonce[i] = (uint8_t)(0x80u + i);
    }
    c->uuid = 0xE6616408432B6F39ull;
    ioc_link_derive_device_key(c->master, c->uuid, c->dev);
    ioc_link_derive_session_key(c->dev, c->uuid, 2, c->sensor_nonce, c->node_nonce, c->sess);
    uint8_t raw[IOC_CHALLENGE_LEN];
    ioc_hello_t h;
    h.uuid = c->uuid;
    h.profile = 1;
    h.protocol = IOC_LINK_VERSION;
    memcpy(h.sensor_nonce, c->sensor_nonce, sizeof(h.sensor_nonce));
    ioc_hello_encode(&h, raw);
    ioc_env_seal(c->dev, IOC_MSG_HELLO, 0, raw, IOC_HELLO_LEN, c->hello, sizeof(c->hello));
    ioc_challenge_t ch;
    ch.node_id = 2;
    ch.child_id = 1;
    memcpy(ch.node_nonce, c->node_nonce, sizeof(ch.node_nonce));
    memcpy(ch.sensor_nonce, c->sensor_nonce, sizeof(ch.sensor_nonce));
    ioc_challenge_encode(&ch, raw);
    ioc_env_seal(c->dev, IOC_MSG_CHALLENGE, 0, raw, IOC_CHALLENGE_LEN, c->challenge, sizeof(c->challenge));
    ioc_confirm_t cf;
    memcpy(cf.node_nonce, c->node_nonce, sizeof(cf.node_nonce));
    memcpy(cf.sensor_nonce, c->sensor_nonce, sizeof(cf.sensor_nonce));
    ioc_confirm_encode(&cf, raw);
    ioc_env_seal(c->sess, IOC_MSG_CONFIRM, 1, raw, IOC_CONFIRM_LEN, c->confirm, sizeof(c->confirm));
    ioc_env_seal(c->sess, IOC_MSG_DATA_UP, 7, c->data, IOC_BENCH_TELEMETRY, c->datagram, sizeof(c->datagram));
}

int ioc_bench_crypto(const char *domain, uint16_t n, ioc_bench_clock_us clock, ioc_bench_emit emit, void *ctx) {
    static const struct { const char *name; int op; uint16_t bytes; } OPS[] = {
        {"HMAC_SHA256", OP_HMAC, 16},     {"HMAC_SHA256", OP_HMAC, 64},
        {"HMAC_SHA256", OP_HMAC, 256},    {"HMAC_SHA256", OP_HMAC, 1024},
        {"ENV_SEAL", OP_SEAL, IOC_BENCH_TELEMETRY}, {"ENV_VERIFY", OP_VERIFY, IOC_BENCH_TELEMETRY},
        {"HANDSHAKE_NODE", OP_HS_NODE, 0}, {"HANDSHAKE_SENSOR", OP_HS_SENSOR, 0},
    };
    if (!clock || !emit) return 0;
    if (n == 0) n = IOC_BENCH_DEFAULT_N;
    if (n > IOC_BENCH_MAX_N) n = IOC_BENCH_MAX_N;
    bench_ctx *c = &g_ctx;
    prepare(c);
    int done = 0;
    for (size_t k = 0; k < sizeof(OPS) / sizeof(OPS[0]); ++k) {
        for (int w = 0; w < 4; ++w) run_op(c, OPS[k].op, OPS[k].bytes);  /* aquece caches */
        const uint64_t start = clock();
        for (uint16_t i = 0; i < n; ++i) {
            const uint64_t t0 = clock();
            run_op(c, OPS[k].op, OPS[k].bytes);
            const uint64_t dt = clock() - t0;
            g_samples[i] = dt > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)dt;
        }
        const uint64_t total = clock() - start;
        ioc_bench_stats st;
        ioc_bench_summarize(g_samples, n, total, &st);
        char line[200];
        snprintf(line, sizeof(line),
                 "BENCH domain=%s op=%s bytes=%u n=%u mean_us=%.2f min_us=%lu p50_us=%lu p95_us=%lu p99_us=%lu max_us=%lu",
                 domain ? domain : "?", OPS[k].name, (unsigned)OPS[k].bytes, (unsigned)st.n, st.mean_us,
                 (unsigned long)st.min_us, (unsigned long)st.p50_us, (unsigned long)st.p95_us,
                 (unsigned long)st.p99_us, (unsigned long)st.max_us);
        emit(line, ctx);
        ++done;
    }
    ioc_secure_zero(c->dev, sizeof(c->dev));
    ioc_secure_zero(c->sess, sizeof(c->sess));
    return done;
}
