/*
 * Testes nativos da biblioteca ioc_link (executados no computador de
 * desenvolvimento e na integração contínua).
 *
 *   test_ioc_link            executa as verificações
 *   test_ioc_link --vectors  imprime vetores determinísticos para a
 *                            conferência cruzada com hashlib/hmac do Python
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ioc_link.h"
#include "ioc_sha256.h"
#include "ioc_wdata.h"

static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FALHA %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

static void to_hex(const uint8_t *in, size_t len, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = digits[in[i] >> 4];
        out[i * 2 + 1] = digits[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

static void fill(uint8_t *out, size_t len, uint8_t seed) {
    for (size_t i = 0; i < len; ++i) out[i] = (uint8_t)(seed + i * 7u);
}

static void test_sha256_vectors(void) {
    uint8_t digest[32];
    char hex[65];
    ioc_sha256_ctx ctx;

    ioc_sha256_init(&ctx);
    ioc_sha256_update(&ctx, (const uint8_t *)"abc", 3);
    ioc_sha256_final(&ctx, digest);
    to_hex(digest, 32, hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

    ioc_sha256_init(&ctx);
    ioc_sha256_final(&ctx, digest);
    to_hex(digest, 32, hex);
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);

    const char *two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    ioc_sha256_init(&ctx);
    ioc_sha256_update(&ctx, (const uint8_t *)two_blocks, strlen(two_blocks));
    ioc_sha256_final(&ctx, digest);
    to_hex(digest, 32, hex);
    CHECK(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
}

static void test_hmac_rfc4231(void) {
    uint8_t key[131];
    uint8_t digest[32];
    char hex[65];

    memset(key, 0x0b, 20);
    ioc_hmac_sha256(key, 20, (const uint8_t *)"Hi There", 8, digest);
    to_hex(digest, 32, hex);
    CHECK(strcmp(hex, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7") == 0);

    ioc_hmac_sha256((const uint8_t *)"Jefe", 4,
                    (const uint8_t *)"what do ya want for nothing?", 28, digest);
    to_hex(digest, 32, hex);
    CHECK(strcmp(hex, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") == 0);

    /* Caso 6: chave maior que o bloco. */
    memset(key, 0xaa, 131);
    const char *msg = "Test Using Larger Than Block-Size Key - Hash Key First";
    ioc_hmac_sha256(key, 131, (const uint8_t *)msg, strlen(msg), digest);
    to_hex(digest, 32, hex);
    CHECK(strcmp(hex, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54") == 0);
}

static void test_keys(void) {
    uint8_t master[32], other[32], dev_a[32], dev_b[32], net[32];
    char psk1[IOC_LINK_PSK_LEN], psk2[IOC_LINK_PSK_LEN], ssid[IOC_LINK_SSID_LEN];

    CHECK(ioc_link_hex_to_key("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", master));
    CHECK(master[0] == 0x00 && master[31] == 0x1f);
    CHECK(ioc_link_hex_to_key("0x000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", other));
    CHECK(memcmp(master, other, 32) == 0);
    CHECK(!ioc_link_hex_to_key("1234", other));
    CHECK(!ioc_link_hex_to_key("zz0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", other));
    CHECK(ioc_link_key_is_zero(other));
    CHECK(!ioc_link_key_is_zero(master));

    ioc_link_derive_device_key(master, 0xE6616408432B6F39ull, dev_a);
    ioc_link_derive_device_key(master, 0xE6616408432B6F3Aull, dev_b);
    CHECK(memcmp(dev_a, dev_b, 32) != 0);
    ioc_link_derive_network_key(master, net);
    CHECK(memcmp(net, dev_a, 32) != 0);

    ioc_link_wifi_psk(net, 1, psk1);
    ioc_link_wifi_psk(net, 2, psk2);
    CHECK(strlen(psk1) == 32);
    CHECK(strcmp(psk1, psk2) != 0);
    ioc_link_wifi_ssid(4, ssid);
    CHECK(strcmp(ssid, "IOC-04") == 0);
    ioc_link_wifi_ssid(31, ssid);
    CHECK(strcmp(ssid, "IOC-31") == 0);
    CHECK(ioc_link_wifi_channel(1) == 6 && ioc_link_wifi_channel(2) == 11 && ioc_link_wifi_channel(3) == 1);
}

static void test_offer(void) {
    uint8_t dev[32], wrong[32], mfg[IOC_OFFER_MFG_LEN];
    ioc_offer_t offer = {0xE6616408432B6F39ull, 3, 1, 0x01020304u};
    ioc_offer_t parsed;
    fill(dev, 32, 0x11);
    fill(wrong, 32, 0x12);

    CHECK(ioc_offer_build(dev, &offer, mfg) == IOC_OFFER_MFG_LEN);
    CHECK(ioc_offer_peek(mfg, sizeof(mfg), &parsed));
    CHECK(parsed.uuid == offer.uuid && parsed.node_id == 3 && parsed.channel == 1 && parsed.nonce == offer.nonce);
    CHECK(ioc_offer_verify(dev, mfg, sizeof(mfg), &parsed));
    CHECK(!ioc_offer_verify(wrong, mfg, sizeof(mfg), &parsed));
    CHECK(!ioc_offer_verify(dev, mfg, sizeof(mfg) - 1, &parsed));

    mfg[5] = 4; /* troca do Node responsável por um atacante */
    CHECK(!ioc_offer_verify(dev, mfg, sizeof(mfg), &parsed));
    mfg[5] = 3;
    mfg[14] = 6; /* troca de canal */
    CHECK(!ioc_offer_verify(dev, mfg, sizeof(mfg), &parsed));
    mfg[14] = 1;
    CHECK(ioc_offer_verify(dev, mfg, sizeof(mfg), &parsed));
    mfg[2] = 'X';
    CHECK(!ioc_offer_peek(mfg, sizeof(mfg), &parsed));
}

static void test_envelope(void) {
    uint8_t key[32], wrong[32], payload[40], buf[80];
    ioc_env_view view;
    fill(key, 32, 0x21);
    fill(wrong, 32, 0x22);
    fill(payload, sizeof(payload), 0x55);

    size_t n = ioc_env_seal(key, IOC_MSG_DATA_UP, 7, payload, sizeof(payload), buf, sizeof(buf));
    CHECK(n == sizeof(payload) + IOC_ENV_OVERHEAD);
    CHECK(ioc_env_parse(buf, n, &view));
    CHECK(view.type == IOC_MSG_DATA_UP && view.counter == 7 && view.payload_len == sizeof(payload));
    CHECK(memcmp(view.payload, payload, sizeof(payload)) == 0);
    CHECK(ioc_env_verify(key, buf, n));
    CHECK(!ioc_env_verify(wrong, buf, n));

    for (size_t i = 0; i < n; ++i) { /* qualquer bit alterado invalida a tag */
        buf[i] ^= 0x01;
        CHECK(!ioc_env_verify(key, buf, n) || !ioc_env_parse(buf, n, &view));
        buf[i] ^= 0x01;
    }
    CHECK(ioc_env_verify(key, buf, n));
    CHECK(!ioc_env_verify(key, buf, n - 1)); /* truncado */

    CHECK(ioc_env_seal(key, IOC_MSG_DATA_UP, 1, payload, sizeof(payload), buf, 20) == 0);
    n = ioc_env_seal(key, IOC_MSG_CONFIRM, 1, NULL, 0, buf, sizeof(buf));
    CHECK(n == IOC_ENV_OVERHEAD && ioc_env_verify(key, buf, n));
    buf[2] = 0x05; /* versão anterior do protocolo */
    CHECK(!ioc_env_parse(buf, n, &view));

    uint32_t last = 0;
    CHECK(!ioc_counter_accept(&last, 0));
    CHECK(ioc_counter_accept(&last, 1));
    CHECK(!ioc_counter_accept(&last, 1)); /* replay */
    CHECK(ioc_counter_accept(&last, 5));
    CHECK(!ioc_counter_accept(&last, 4)); /* fora de ordem */
    CHECK(ioc_counter_accept(&last, 0xFFFFFFFFu));
    CHECK(!ioc_counter_accept(&last, 0));
}

/* Simula a troca completa entre sensor e Node. */
static void test_handshake(void) {
    const uint64_t uuid = 0xE6616408432B6F39ull;
    uint8_t master[32], dev_node[32], dev_sensor[32];
    uint8_t wire[64], raw[IOC_HELLO_LEN];
    ioc_env_view view;
    fill(master, 32, 0x31);
    ioc_link_derive_device_key(master, uuid, dev_sensor); /* gravada no sensor */

    /* sensor -> HELLO */
    ioc_hello_t hello = {uuid, 0x01, IOC_LINK_VERSION, {1, 2, 3, 4, 5, 6, 7, 8}};
    ioc_hello_encode(&hello, raw);
    size_t n = ioc_env_seal(dev_sensor, IOC_MSG_HELLO, 0, raw, sizeof(raw), wire, sizeof(wire));

    /* Node: lê o UUID, deriva a chave e só então autentica */
    ioc_hello_t rx_hello;
    CHECK(ioc_env_parse(wire, n, &view) && view.type == IOC_MSG_HELLO);
    CHECK(ioc_hello_decode(view.payload, view.payload_len, &rx_hello));
    ioc_link_derive_device_key(master, rx_hello.uuid, dev_node);
    CHECK(ioc_env_verify(dev_node, wire, n));

    /* Node -> CHALLENGE */
    ioc_challenge_t ch = {2, 1, {9, 9, 9, 9, 8, 8, 8, 8}, {0}};
    memcpy(ch.sensor_nonce, rx_hello.sensor_nonce, 8);
    uint8_t raw_ch[IOC_CHALLENGE_LEN];
    ioc_challenge_encode(&ch, raw_ch);
    n = ioc_env_seal(dev_node, IOC_MSG_CHALLENGE, 0, raw_ch, sizeof(raw_ch), wire, sizeof(wire));

    /* sensor: confere o eco do próprio nonce */
    ioc_challenge_t rx_ch;
    CHECK(ioc_env_verify(dev_sensor, wire, n));
    CHECK(ioc_env_parse(wire, n, &view) && ioc_challenge_decode(view.payload, view.payload_len, &rx_ch));
    CHECK(memcmp(rx_ch.sensor_nonce, hello.sensor_nonce, 8) == 0);

    uint8_t sess_sensor[32], sess_node[32];
    ioc_link_derive_session_key(dev_sensor, uuid, rx_ch.node_id, hello.sensor_nonce, rx_ch.node_nonce, sess_sensor);
    ioc_link_derive_session_key(dev_node, uuid, ch.node_id, rx_hello.sensor_nonce, ch.node_nonce, sess_node);
    CHECK(memcmp(sess_sensor, sess_node, 32) == 0);
    CHECK(memcmp(sess_sensor, dev_sensor, 32) != 0);

    /* sensor -> CONFIRM com a chave de sessão */
    ioc_confirm_t cf;
    memcpy(cf.node_nonce, rx_ch.node_nonce, 8);
    memcpy(cf.sensor_nonce, hello.sensor_nonce, 8);
    uint8_t raw_cf[IOC_CONFIRM_LEN];
    ioc_confirm_encode(&cf, raw_cf);
    n = ioc_env_seal(sess_sensor, IOC_MSG_CONFIRM, 1, raw_cf, sizeof(raw_cf), wire, sizeof(wire));
    CHECK(ioc_env_verify(sess_node, wire, n));
    CHECK(!ioc_env_verify(dev_node, wire, n)); /* K_dev não autentica dados de sessão */

    /* Outro nonce de Node gera outra chave: um CONFIRM capturado não serve depois. */
    uint8_t other_nonce[8] = {7, 7, 7, 7, 7, 7, 7, 7}, sess_other[32];
    ioc_link_derive_session_key(dev_node, uuid, ch.node_id, rx_hello.sensor_nonce, other_nonce, sess_other);
    CHECK(!ioc_env_verify(sess_other, wire, n));

    /* Sensor com chave de outro dispositivo não passa pelo Node. */
    uint8_t dev_intruder[32];
    ioc_link_derive_device_key(master, uuid + 1, dev_intruder);
    n = ioc_env_seal(dev_intruder, IOC_MSG_HELLO, 0, raw, sizeof(raw), wire, sizeof(wire));
    CHECK(!ioc_env_verify(dev_node, wire, n));
}

static void test_segmentation(void) {
    uint8_t payload[IOC_WD_MAX_PAYLOAD], frame[8];
    ioc_wd_rx rx;
    const ioc_wd_rx_slot *done;

    CHECK(ioc_wd_segment_count(0) == 0);
    CHECK(ioc_wd_segment_count(1) == 1);
    CHECK(ioc_wd_segment_count(4) == 1);
    CHECK(ioc_wd_segment_count(5) == 2);
    CHECK(ioc_wd_segment_count(10) == 2);
    CHECK(ioc_wd_segment_count(11) == 3);
    CHECK(ioc_wd_segment_count(IOC_WD_MAX_PAYLOAD) == IOC_WD_MAX_SEGMENTS);
    CHECK(ioc_wd_segment_count(IOC_WD_MAX_PAYLOAD + 1) == 0);

    for (size_t len = 1; len <= IOC_WD_MAX_PAYLOAD; ++len) {
        fill(payload, len, (uint8_t)len);
        ioc_wd_rx_reset(&rx);
        uint8_t count = ioc_wd_segment_count(len);
        ioc_wd_rx_result r = IOC_WD_RX_PENDING;
        for (uint8_t i = 0; i < count; ++i) {
            CHECK(ioc_wd_build_segment(3, IOC_WD_KIND_SPECTRUM, 5, payload, len, i, frame));
            r = ioc_wd_rx_push(&rx, frame, 100, &done);
            if (i + 1 < count) CHECK(r == IOC_WD_RX_PENDING);
        }
        CHECK(r == IOC_WD_RX_COMPLETE);
        CHECK(done && done->total_len == len && done->child == 3 && done->kind == IOC_WD_KIND_SPECTRUM);
        CHECK(done && memcmp(done->data, payload, len) == 0);
    }

    /* segmento perdido aborta a remontagem */
    fill(payload, 30, 1);
    ioc_wd_rx_reset(&rx);
    ioc_wd_build_segment(1, IOC_WD_KIND_TELEMETRY, 0, payload, 30, 0, frame);
    CHECK(ioc_wd_rx_push(&rx, frame, 0, &done) == IOC_WD_RX_PENDING);
    ioc_wd_build_segment(1, IOC_WD_KIND_TELEMETRY, 0, payload, 30, 2, frame);
    CHECK(ioc_wd_rx_push(&rx, frame, 0, &done) == IOC_WD_RX_ERROR);

    /* corrupção detectada pelo CRC */
    ioc_wd_rx_reset(&rx);
    ioc_wd_build_segment(1, IOC_WD_KIND_TELEMETRY, 1, payload, 10, 0, frame);
    CHECK(ioc_wd_rx_push(&rx, frame, 0, &done) == IOC_WD_RX_PENDING);
    ioc_wd_build_segment(1, IOC_WD_KIND_TELEMETRY, 1, payload, 10, 1, frame);
    frame[4] ^= 0x40;
    CHECK(ioc_wd_rx_push(&rx, frame, 0, &done) == IOC_WD_RX_ERROR);

    /* transferência antiga expira */
    ioc_wd_rx_reset(&rx);
    ioc_wd_build_segment(1, IOC_WD_KIND_TELEMETRY, 2, payload, 10, 0, frame);
    CHECK(ioc_wd_rx_push(&rx, frame, 0, &done) == IOC_WD_RX_PENDING);
    ioc_wd_build_segment(1, IOC_WD_KIND_TELEMETRY, 2, payload, 10, 1, frame);
    CHECK(ioc_wd_rx_push(&rx, frame, IOC_WD_RX_TIMEOUT_MS + 1, &done) == IOC_WD_RX_ERROR);

    /* dois filhos intercalados no mesmo identificador CAN */
    uint8_t a[20], b[20];
    fill(a, 20, 0xA0);
    fill(b, 20, 0xB0);
    ioc_wd_rx_reset(&rx);
    int complete = 0;
    for (uint8_t i = 0; i < ioc_wd_segment_count(20); ++i) {
        ioc_wd_build_segment(1, IOC_WD_KIND_TELEMETRY, 0, a, 20, i, frame);
        if (ioc_wd_rx_push(&rx, frame, 0, &done) == IOC_WD_RX_COMPLETE) {
            CHECK(done->child == 1 && memcmp(done->data, a, 20) == 0);
            complete++;
        }
        ioc_wd_build_segment(2, IOC_WD_KIND_TELEMETRY, 0, b, 20, i, frame);
        if (ioc_wd_rx_push(&rx, frame, 0, &done) == IOC_WD_RX_COMPLETE) {
            CHECK(done->child == 2 && memcmp(done->data, b, 20) == 0);
            complete++;
        }
    }
    CHECK(complete == 2);

    CHECK(!ioc_wd_build_segment(8, IOC_WD_KIND_TELEMETRY, 0, a, 20, 0, frame));
    CHECK(!ioc_wd_build_segment(1, IOC_WD_KIND_TELEMETRY, 0, a, 20, 9, frame));
}

static void test_codecs(void) {
    uint8_t buf[IOC_WD_MAX_PAYLOAD];

    ioc_wd_telemetry t = {0};
    t.seq = 0xBEEF; t.mode = 1; t.acquisition = 2; t.axis_mask = 8;
    t.flags = IOC_WD_TEL_FLAG_FFT_VALID | IOC_WD_TEL_FLAG_CLIPPING;
    t.window_size = 512; t.rate_req_hz = 1000.0f; t.rate_eff_hz = 999.5f;
    t.rms = 0.1242f; t.kurtosis = -0.208f; t.crest = 2.796f; t.peak_hz = 31.75f;
    t.peak_amp = 0.02045f; t.entropy = 0.45f; t.ppv_mm_s = 0.4f;
    t.battery_pct = 93; t.battery_mv = 3980; t.dtc_count = 1; t.dtc_code = 0x2002;
    ioc_wd_telemetry back;
    CHECK(ioc_wd_telemetry_encode(&t, buf) == IOC_WD_TELEMETRY_LEN);
    CHECK(ioc_wd_telemetry_decode(buf, IOC_WD_TELEMETRY_LEN, &back));
    CHECK(back.seq == t.seq && back.window_size == 512 && back.flags == t.flags);
    CHECK(back.rms == t.rms && back.peak_hz == t.peak_hz && back.kurtosis == t.kurtosis);
    CHECK(back.battery_mv == 3980 && back.dtc_code == 0x2002);
    CHECK(!ioc_wd_telemetry_decode(buf, IOC_WD_TELEMETRY_LEN - 1, &back));
    CHECK(ioc_wd_segment_count(IOC_WD_TELEMETRY_LEN) == 9);

    float mags[256];
    uint16_t reduced[64];
    float scale = 0;
    for (int i = 0; i < 256; ++i) mags[i] = (i == 40) ? 10.0f : (i == 200 ? 5.0f : 0.1f);
    mags[3] = NAN;
    mags[5] = INFINITY; /* não pode virar o pico nem gerar valor indefinido */
    ioc_wd_spectrum_reduce(mags, 256, reduced, 64, &scale);
    CHECK(scale == 10.0f);
    CHECK(reduced[10] == 65535);                 /* faixa que contém o bin 40 */
    CHECK(reduced[50] > 32000 && reduced[50] < 33500); /* bin 200: metade do pico */
    CHECK(reduced[0] < 1000);

    ioc_wd_spectrum_header h = {7, 64, 0, 64, scale, 1000.0f, 512, 1}, hb;
    const uint8_t *bins = NULL;
    size_t n = ioc_wd_spectrum_encode(&h, reduced, buf, sizeof(buf));
    CHECK(n == IOC_WD_SPECTRUM_HEADER_LEN + 128u);
    CHECK(ioc_wd_spectrum_decode(buf, n, &hb, &bins));
    CHECK(hb.spectrum_id == 7 && hb.total_bins == 64 && hb.count == 64 && hb.fft_size == 512);
    CHECK(bins && (uint16_t)(bins[20] | (bins[21] << 8)) == reduced[10]);
    CHECK(!ioc_wd_spectrum_decode(buf, n - 1, &hb, &bins));
    h.count = IOC_WD_SPECTRUM_CHUNK_BINS + 10; /* não cabe em uma transferência */
    CHECK(ioc_wd_spectrum_encode(&h, reduced, buf, sizeof(buf)) == 0);
    CHECK(IOC_WD_SPECTRUM_HEADER_LEN + IOC_WD_SPECTRUM_CHUNK_BINS * 2 <= IOC_WD_MAX_PAYLOAD);

    ioc_wd_ack ack = {9, IOC_WD_OP_APPLY, IOC_WD_ACK_APPLIED, 0x1234}, ack2;
    CHECK(ioc_wd_ack_encode(&ack, buf) == IOC_WD_ACK_LEN && ioc_wd_ack_decode(buf, IOC_WD_ACK_LEN, &ack2));
    CHECK(ack2.seq == 9 && ack2.status == IOC_WD_ACK_APPLIED && ack2.detail == 0x1234);

    ioc_wd_dtc dtc = {0x4003, 1, 1, 123456u}, dtc2;
    CHECK(ioc_wd_dtc_encode(&dtc, buf) == IOC_WD_DTC_LEN && ioc_wd_dtc_decode(buf, IOC_WD_DTC_LEN, &dtc2));
    CHECK(dtc2.code == 0x4003 && dtc2.timestamp_ms == 123456u);

    ioc_wd_link link = {IOC_WD_LINK_SECURE, -52, 120, 400, 2, 1, 3, 1, 1000}, link2;
    CHECK(ioc_wd_link_encode(&link, buf) == IOC_WD_LINK_LEN && ioc_wd_link_decode(buf, IOC_WD_LINK_LEN, &link2));
    CHECK(link2.state == IOC_WD_LINK_SECURE && link2.wifi_rssi == -52 && link2.lost_datagrams == 3);
    CHECK(link2.stream_enabled == 1 && link2.stream_period_ms == 1000);

    ioc_wd_config cfg = {1, 1, 512, 1000.0f, 1000.0f, 4.0f, 1.0f, 1}, cfg2;
    CHECK(ioc_wd_config_encode(&cfg, buf) == IOC_WD_CONFIG_LEN && ioc_wd_config_decode(buf, IOC_WD_CONFIG_LEN, &cfg2));
    CHECK(cfg2.window_size == 512 && cfg2.stalta == 4.0f && cfg2.status == 1);

    ioc_wd_command cmd = {3, 2, 1, IOC_WD_OP_SET_RATE, {0}}, cmd2;
    uint8_t frame[8];
    ioc_wd_put_arg_f32(cmd.arg, 250.0f);
    ioc_wd_command_encode(&cmd, frame);
    ioc_wd_command_decode(frame, &cmd2);
    CHECK(cmd2.seq == 3 && cmd2.node_id == 2 && cmd2.child_id == 1 && cmd2.opcode == IOC_WD_OP_SET_RATE);
    CHECK(ioc_wd_arg_f32(cmd2.arg) == 250.0f);
    ioc_wd_put_arg_u16(cmd.arg, 1000);
    CHECK(ioc_wd_arg_u16(cmd.arg) == 1000);
}

static void print_vectors(void) {
    uint8_t master[32], dev[32], net[32], sess[32], mfg[IOC_OFFER_MFG_LEN], wire[64];
    uint8_t sensor_nonce[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t node_nonce[8] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7};
    const uint64_t uuid = 0xE6616408432B6F39ull;
    char hex[160], psk[IOC_LINK_PSK_LEN];
    for (int i = 0; i < 32; ++i) master[i] = (uint8_t)i;

    ioc_link_derive_device_key(master, uuid, dev);
    to_hex(dev, 32, hex); printf("device_key=%s\n", hex);
    ioc_link_derive_network_key(master, net);
    to_hex(net, 32, hex); printf("network_key=%s\n", hex);
    ioc_link_wifi_psk(net, 3, psk); printf("wifi_psk_node3=%s\n", psk);
    ioc_link_derive_session_key(dev, uuid, 3, sensor_nonce, node_nonce, sess);
    to_hex(sess, 32, hex); printf("session_key=%s\n", hex);

    ioc_offer_t offer = {uuid, 3, 1, 0x01020304u};
    ioc_offer_build(dev, &offer, mfg);
    to_hex(mfg, sizeof(mfg), hex); printf("offer=%s\n", hex);

    const uint8_t payload[5] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01};
    size_t n = ioc_env_seal(sess, IOC_MSG_DATA_UP, 0x01020304u, payload, sizeof(payload), wire, sizeof(wire));
    to_hex(wire, n, hex); printf("envelope=%s\n", hex);
    printf("crc8=%02x\n", ioc_wd_crc8((const uint8_t *)"123456789", 9));
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--vectors") == 0) {
        print_vectors();
        return 0;
    }
    test_sha256_vectors();
    test_hmac_rfc4231();
    test_keys();
    test_offer();
    test_envelope();
    test_handshake();
    test_segmentation();
    test_codecs();
    if (g_failures) {
        fprintf(stderr, "ioc_link: %d falha(s)\n", g_failures);
        return 1;
    }
    printf("ioc_link native tests: PASS\n");
    return 0;
}
