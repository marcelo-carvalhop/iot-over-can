/*
 * Fuzz determinístico dos decodificadores de Codigo/common/ioc_link.
 *
 * Tudo o que chega pelo ar (oferta BLE, envelope UDP, HELLO/CHALLENGE/
 * CONFIRM, relatórios do plano de dados) ou pelo barramento (segmentos,
 * comandos, PDU autenticada) passa por estas funções antes de qualquer
 * conferência de origem. Aqui elas recebem entradas aleatórias e mutações de
 * mensagens válidas, sempre em buffers alocados no tamanho exato: com
 * AddressSanitizer/UBSan (test_native_firmware.sh compila assim quando o
 * compilador oferece), qualquer leitura fora do buffer ou comportamento
 * indefinido interrompe o teste. Sem sanitizadores, o teste ainda confere as
 * invariantes de cada decodificador.
 *
 * Uso: fuzz_ioc_decoders [iterações] [semente]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ioc_bindstore.h"
#include "ioc_canbits.h"
#include "ioc_link.h"
#include "ioc_secpdu.h"
#include "ioc_wdata.h"

static uint64_t rng_state;

static uint32_t rnd(void) {
    /* xorshift64*: reprodutível em qualquer plataforma */
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 2685821657736338717ULL) >> 32);
}

static void fill(uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; ++i) buf[i] = (uint8_t)rnd();
}

/* Cópia exata em memória nova, para o ASan enxergar o limite verdadeiro. */
static uint8_t *exact_copy(const uint8_t *src, size_t len) {
    uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
    if (!buf) {
        fprintf(stderr, "sem memória\n");
        exit(2);
    }
    if (len) memcpy(buf, src, len);
    return buf;
}

/* Muta uma mensagem válida: troca bits, bytes, trunca ou estende. */
static size_t mutate(uint8_t *buf, size_t len, size_t cap) {
    switch (rnd() % 5) {
    case 0:
        if (len) buf[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8));
        break;
    case 1:
        if (len) buf[rnd() % len] = (uint8_t)rnd();
        break;
    case 2:
        return len ? rnd() % len : 0;
    case 3: {
        size_t extra = rnd() % 16;
        if (len + extra > cap) extra = cap - len;
        fill(buf + len, extra);
        return len + extra;
    }
    default:
        for (size_t i = 0; i < len && i < 4; ++i) buf[rnd() % len] = (uint8_t)rnd();
        break;
    }
    return len;
}

static uint32_t checks;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        ++checks;                                                                   \
        if (!(cond)) {                                                              \
            fprintf(stderr, "%s:%d: invariante violada: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                                \
        }                                                                           \
    } while (0)

static volatile uint32_t sink; /* impede que o compilador descarte leituras */

static void decode_all(const uint8_t *src, size_t len, const uint8_t key[IOC_LINK_KEY_LEN]) {
    uint8_t *buf = exact_copy(src, len);

    ioc_env_view env;
    if (ioc_env_parse(buf, len, &env) == 1) {
        CHECK(env.payload_len <= len);
        CHECK(env.payload_len == 0 || (env.payload >= buf && env.payload + env.payload_len <= buf + len));
        for (size_t i = 0; i < env.payload_len; ++i) sink += env.payload[i];
    }
    sink += (uint32_t)ioc_env_verify(key, buf, len);

    ioc_offer_t offer;
    sink += (uint32_t)ioc_offer_peek(buf, len, &offer);
    sink += (uint32_t)ioc_offer_verify(key, buf, len, &offer);

    ioc_hello_t hello;
    ioc_challenge_t challenge;
    ioc_confirm_t confirm;
    sink += (uint32_t)ioc_hello_decode(buf, len, &hello);
    sink += (uint32_t)ioc_challenge_decode(buf, len, &challenge);
    sink += (uint32_t)ioc_confirm_decode(buf, len, &confirm);

    ioc_wd_telemetry tel;
    ioc_wd_acoustic acoustic;
    ioc_wd_ack ack;
    ioc_wd_dtc dtc;
    ioc_wd_link link;
    ioc_wd_config config;
    ioc_wd_link_event link_event;
    sink += (uint32_t)ioc_wd_link_event_decode(buf, len, &link_event);
    sink += (uint32_t)ioc_wd_telemetry_decode(buf, len, &tel);
    sink += (uint32_t)ioc_wd_acoustic_decode(buf, len, &acoustic);
    sink += (uint32_t)ioc_wd_ack_decode(buf, len, &ack);
    sink += (uint32_t)ioc_wd_dtc_decode(buf, len, &dtc);
    sink += (uint32_t)ioc_wd_link_decode(buf, len, &link);
    ioc_wd_linkstats linkstats;
    sink += (uint32_t)ioc_wd_linkstats_decode(buf, len, &linkstats);
    sink += (uint32_t)ioc_wd_config_decode(buf, len, &config);

    ioc_wd_spectrum_header hdr;
    const uint8_t *bins = NULL;
    if (ioc_wd_spectrum_decode(buf, len, &hdr, &bins) == 1) {
        /* Os bins anunciados precisam estar dentro do buffer recebido. */
        CHECK(bins != NULL && bins >= buf);
        CHECK((size_t)(bins - buf) + 2u * hdr.count <= len);
        for (size_t i = 0; i < 2u * hdr.count; ++i) sink += bins[i];
    }

    ioc_bs_entry stored[IOC_BS_MAX];
    uint8_t stored_count = 0;
    if (ioc_bs_decode(buf, len, (uint8_t)(rnd() % 4), stored, IOC_BS_MAX, &stored_count) == 1) {
        CHECK(stored_count <= IOC_BS_MAX);
        for (uint8_t i = 0; i < stored_count; ++i) CHECK(stored[i].uuid != 0 && stored[i].child_id >= 1);
    }

    uint64_t freshness = rnd();
    size_t payload_len = 0;
    if (ioc_secpdu_verify(key, (uint16_t)rnd(), buf, len, &freshness, (uint8_t)(4 + rnd() % 13), &payload_len) == 1) {
        CHECK(payload_len <= len);
    }
    free(buf);
}

static void fuzz_reassembly(void) {
    static ioc_wd_rx rx;
    ioc_wd_rx_reset(&rx);
    uint32_t now = 0;
    for (int i = 0; i < 64; ++i) {
        uint8_t frame_len = (rnd() % 4 == 0) ? (uint8_t)rnd() : (uint8_t)(8 + rnd() % 57);
        uint8_t raw[256];
        fill(raw, sizeof(raw));
        uint8_t *frame = exact_copy(raw, frame_len ? frame_len : 1);
        if (rnd() % 3 == 0) frame[0] = (uint8_t)(rnd() % 4); /* filho e tipo plausíveis */
        now += rnd() % 400;
        const ioc_wd_rx_slot *done = NULL;
        ioc_wd_rx_result result;
        if (frame_len == IOC_WD_FRAME_LEN && rnd() % 2) {
            result = ioc_wd_rx_push(&rx, frame, now, &done);
        } else {
            result = ioc_wd_rx_push_ex(&rx, frame, frame_len, now, &done);
        }
        if (result == IOC_WD_RX_COMPLETE) {
            CHECK(done != NULL);
            CHECK(done->total_len <= IOC_WD_MAX_PAYLOAD);
            for (size_t k = 0; k < done->total_len; ++k) sink += done->data[k];
        }
        free(frame);
    }
}

static void fuzz_valid_then_mutated(const uint8_t key[IOC_LINK_KEY_LEN]) {
    uint8_t buf[512];
    size_t len = 0;
    switch (rnd() % 8) {
    case 0: {
        uint8_t payload[64];
        size_t plen = rnd() % sizeof(payload);
        fill(payload, plen);
        len = ioc_env_seal(key, (uint8_t)rnd(), rnd() | 1u, payload, plen, buf, sizeof(buf));
        break;
    }
    case 1: {
        ioc_offer_t offer;
        memset(&offer, 0, sizeof(offer));
        fill((uint8_t *)&offer, sizeof(offer));
        len = ioc_offer_build(key, &offer, buf);
        break;
    }
    case 2: {
        ioc_hello_t hello;
        fill((uint8_t *)&hello, sizeof(hello));
        ioc_hello_encode(&hello, buf);
        len = IOC_HELLO_LEN;
        break;
    }
    case 3: {
        ioc_wd_telemetry tel;
        fill((uint8_t *)&tel, sizeof(tel));
        len = ioc_wd_telemetry_encode(&tel, buf);
        break;
    }
    case 4: {
        ioc_wd_acoustic acoustic;
        fill((uint8_t *)&acoustic, sizeof(acoustic));
        len = ioc_wd_acoustic_encode(&acoustic, buf);
        break;
    }
    case 5: {
        ioc_wd_spectrum_header hdr;
        uint16_t bins[IOC_WD_SPECTRUM_CHUNK_BINS];
        fill((uint8_t *)&hdr, sizeof(hdr));
        hdr.count = (uint8_t)(rnd() % (IOC_WD_SPECTRUM_CHUNK_BINS + 1));
        fill((uint8_t *)bins, sizeof(bins));
        len = ioc_wd_spectrum_encode(&hdr, bins, buf, sizeof(buf));
        break;
    }
    case 6: {
        ioc_bs_entry entries[IOC_BS_MAX];
        uint8_t count = (uint8_t)(rnd() % (IOC_BS_MAX + 1));
        for (uint8_t i = 0; i < count; ++i) {
            entries[i].uuid = ((uint64_t)rnd() << 32) | rnd() | 1u;
            entries[i].child_id = (uint8_t)(i + 1);
            entries[i].profile = (uint8_t)rnd();
            entries[i].protocol = (uint8_t)rnd();
            entries[i].flags = (uint8_t)(rnd() & 1u);
        }
        len = ioc_bs_encode((uint8_t)(rnd() % 4), entries, count, buf, sizeof(buf));
        break;
    }
    default: {
        ioc_wd_config config;
        fill((uint8_t *)&config, sizeof(config));
        len = ioc_wd_config_encode(&config, buf);
        break;
    }
    }
    if (len > sizeof(buf)) len = sizeof(buf);
    len = mutate(buf, len, sizeof(buf));
    decode_all(buf, len, key);
}

static void fuzz_can_frames(void) {
    uint8_t data[8];
    fill(data, sizeof(data));
    uint8_t dlc = (uint8_t)(rnd() % 9);
    uint8_t *exact = exact_copy(data, dlc);
    sink += ioc_can_crc15((uint16_t)(rnd() & 0x7FF), dlc, exact);
    uint16_t bits = ioc_can_frame_bits((uint16_t)(rnd() & 0x7FF), dlc, exact);
    CHECK(bits <= ioc_can_frame_bits_worst(dlc));
    free(exact);

    ioc_wd_command command;
    ioc_wd_command_decode(data, &command);
    sink += (uint32_t)command.opcode;
}

int main(int argc, char **argv) {
    unsigned long iterations = argc > 1 ? strtoul(argv[1], NULL, 10) : 20000ul;
    rng_state = argc > 2 ? strtoull(argv[2], NULL, 10) : 0x10C0FFEEull;
    if (rng_state == 0) rng_state = 1;

    uint8_t key[IOC_LINK_KEY_LEN];
    fill(key, sizeof(key));

    uint8_t raw[300];
    for (unsigned long i = 0; i < iterations; ++i) {
        size_t len = rnd() % sizeof(raw);
        fill(raw, len);
        decode_all(raw, len, key);
        fuzz_valid_then_mutated(key);
        fuzz_can_frames();
        if (i % 64 == 0) fuzz_reassembly();
    }
    printf("fuzz_ioc_decoders: %lu iterações, %u invariantes conferidas: PASS\n", iterations, checks);
    return 0;
}
