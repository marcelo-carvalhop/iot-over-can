/*
 * Testes nativos: duração de quadros CAN, PDU autenticada (predisposição
 * CAN FD), segmentação com quadros maiores e reassociação automática.
 *
 *   test_ioc_net            executa as verificações
 *   test_ioc_net --vectors  imprime vetores para conferência com Python
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ioc_canbits.h"
#include "ioc_failover.h"
#include "ioc_secpdu.h"
#include "ioc_wdata.h"

static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FALHA %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

static uint32_t g_rng = 0xC0FFEEu;
static uint8_t rnd8(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return (uint8_t)(g_rng >> 24);
}

static void test_can_bits(void) {
    uint8_t data[8];

    /* Limites: sem preenchimento possível e pior caso. */
    CHECK(ioc_can_frame_bits_worst(0) == 47 + 8);
    CHECK(ioc_can_frame_bits_worst(8) == 47 + 64 + 24);  /* 135 bits */
    CHECK(ioc_can_frame_bits_worst(1) == 47 + 8 + 10);

    /* Tudo dominante: sequências longas de zeros geram preenchimento máximo no início. */
    memset(data, 0x00, 8);
    CHECK(ioc_can_stuff_bits(0x000, 8, data) >= 15);
    /* Alternância 0x55/0xAA praticamente não gera preenchimento no campo de dados. */
    memset(data, 0x55, 8);
    CHECK(ioc_can_stuff_bits(0x555, 8, data) <= 6);

    for (int i = 0; i < 20000; ++i) {
        uint16_t id = (uint16_t)(((uint16_t)rnd8() << 8 | rnd8()) & 0x7FF);
        uint8_t dlc = (uint8_t)(rnd8() % 9);
        for (int k = 0; k < 8; ++k) data[k] = rnd8();
        uint16_t bits = ioc_can_frame_bits(id, dlc, data);
        CHECK(bits >= 47 + 8 * dlc);
        CHECK(bits <= ioc_can_frame_bits_worst(dlc));
        CHECK(ioc_can_crc15(id, dlc, data) <= 0x7FFF);
    }
    /* DLC maior que 8 no CAN clássico ainda transporta 8 bytes. */
    CHECK(ioc_can_frame_bits(0x123, 15, data) >= 47 + 64);
}

static void test_secpdu(void) {
    uint8_t key[32], other[32], pdu[64], payload[40];
    uint64_t tx = 0, rx = 0;
    size_t n, plen = 0;
    for (int i = 0; i < 32; ++i) { key[i] = (uint8_t)(i * 3 + 1); other[i] = (uint8_t)(i * 5 + 2); }
    for (int i = 0; i < 40; ++i) payload[i] = (uint8_t)(0x80 + i);

    /* Perfil compacto: 4 bytes de carga útil + 4 de proteção em um quadro clássico. */
    n = ioc_secpdu_protect(key, 0x304, payload, 4, &tx, IOC_SECPDU_MAC_CLASSIC, pdu, 8);
    CHECK(n == 8 && tx == 1);
    CHECK(ioc_secpdu_verify(key, 0x304, pdu, n, &rx, IOC_SECPDU_MAC_CLASSIC, &plen));
    CHECK(plen == 4 && rx == 1 && memcmp(pdu, payload, 4) == 0);
    CHECK(!ioc_secpdu_verify(key, 0x304, pdu, n, &rx, IOC_SECPDU_MAC_CLASSIC, &plen)); /* repetição */
    CHECK(rx == 1);
    CHECK(ioc_secpdu_protect(key, 0x304, payload, 5, &tx, IOC_SECPDU_MAC_CLASSIC, pdu, 8) == 0);
    CHECK(tx == 1); /* falha não consome contador */

    /* Perfil CAN FD: 40 bytes de carga + 9 de proteção. */
    n = ioc_secpdu_protect(key, 0x381, payload, 40, &tx, IOC_SECPDU_MAC_FD, pdu, sizeof(pdu));
    CHECK(n == 49 && tx == 2);
    uint64_t rx_wrong_id = rx, rx_wrong_key = rx;
    CHECK(!ioc_secpdu_verify(key, 0x382, pdu, n, &rx_wrong_id, IOC_SECPDU_MAC_FD, &plen)); /* outro identificador */
    CHECK(!ioc_secpdu_verify(other, 0x381, pdu, n, &rx_wrong_key, IOC_SECPDU_MAC_FD, &plen));
    for (size_t i = 0; i < n; ++i) {
        uint64_t rx_copy = rx;
        pdu[i] ^= 0x10;
        CHECK(!ioc_secpdu_verify(key, 0x381, pdu, n, &rx_copy, IOC_SECPDU_MAC_FD, &plen));
        CHECK(rx_copy == rx);
        pdu[i] ^= 0x10;
    }
    CHECK(ioc_secpdu_verify(key, 0x381, pdu, n, &rx, IOC_SECPDU_MAC_FD, &plen) && plen == 40 && rx == 2);

    /* Quadros perdidos dentro da janela e virada do byte de atualidade. */
    tx = 0x1FA; rx = 0x1FA;
    for (int i = 0; i < 20; ++i) {
        n = ioc_secpdu_protect(key, 0x10, payload, 8, &tx, IOC_SECPDU_MAC_FD, pdu, sizeof(pdu));
        if (i % 3 == 0) continue; /* perdido */
        CHECK(ioc_secpdu_verify(key, 0x10, pdu, n, &rx, IOC_SECPDU_MAC_FD, &plen));
        CHECK(rx == tx);
    }
    CHECK(tx == 0x1FA + 20);
    /* Além da janela o receptor não aceita (precisaria de ressincronização). */
    tx = rx + 300;
    n = ioc_secpdu_protect(key, 0x10, payload, 8, &tx, IOC_SECPDU_MAC_FD, pdu, sizeof(pdu));
    uint64_t before = rx;
    CHECK(!ioc_secpdu_verify(key, 0x10, pdu, n, &rx, IOC_SECPDU_MAC_FD, &plen) && rx == before);

    tx = UINT64_MAX;
    CHECK(ioc_secpdu_protect(key, 0x10, payload, 8, &tx, IOC_SECPDU_MAC_FD, pdu, sizeof(pdu)) == 0);
    CHECK(!ioc_secpdu_verify(key, 0x10, pdu, 3, &rx, IOC_SECPDU_MAC_FD, &plen)); /* curto demais */

    uint8_t bus_a[32], bus_b[32];
    ioc_secpdu_derive_bus_key(key, bus_a);
    ioc_secpdu_derive_bus_key(other, bus_b);
    CHECK(memcmp(bus_a, bus_b, 32) != 0 && memcmp(bus_a, key, 32) != 0);
}

static void test_segmentation_fd(void) {
    static const uint8_t sizes[] = {8, 12, 16, 20, 24, 32, 48, 64};
    uint8_t payload[IOC_WD_MAX_PAYLOAD], frame[64];
    ioc_wd_rx rx;
    const ioc_wd_rx_slot *done;

    CHECK(ioc_wd_fd_frame_len(1) == 8 && ioc_wd_fd_frame_len(9) == 12 && ioc_wd_fd_frame_len(33) == 48);
    CHECK(ioc_wd_fd_frame_len(64) == 64 && ioc_wd_fd_frame_len(65) == 0);
    CHECK(ioc_wd_segment_count_ex(10, 10) == 0); /* tamanho de quadro inválido */
    CHECK(ioc_wd_segment_count_ex(IOC_WD_TELEMETRY_LEN, 64) == 1);  /* telemetria em um quadro FD */
    CHECK(ioc_wd_segment_count_ex(IOC_WD_TELEMETRY_LEN, 8) == 9);
    CHECK(ioc_wd_segment_count_ex(177, 64) == 3);                   /* bloco de espectro */
    CHECK(ioc_wd_segment_count_ex(177, 8) == 30);

    for (unsigned s = 0; s < sizeof(sizes); ++s) {
        const uint8_t frame_len = sizes[s];
        for (size_t len = 1; len <= IOC_WD_MAX_PAYLOAD; len += (len < 70 ? 1 : 7)) {
            for (size_t i = 0; i < len; ++i) payload[i] = (uint8_t)(len * 3 + i);
            const uint8_t count = ioc_wd_segment_count_ex(len, frame_len);
            CHECK(count > 0);
            ioc_wd_rx_reset(&rx);
            ioc_wd_rx_result r = IOC_WD_RX_PENDING;
            for (uint8_t i = 0; i < count; ++i) {
                uint8_t n = ioc_wd_build_segment_ex(2, IOC_WD_KIND_SPECTRUM, 1, payload, len, i, frame_len, frame);
                CHECK(n >= 8 && n <= frame_len && ioc_wd_fd_frame_len(n) == n);
                r = ioc_wd_rx_push_ex(&rx, frame, n, 10, &done);
            }
            CHECK(r == IOC_WD_RX_COMPLETE && done && done->total_len == len);
            CHECK(done && memcmp(done->data, payload, len) == 0);
        }
    }
    CHECK(ioc_wd_rx_push_ex(&rx, frame, 7, 0, &done) == IOC_WD_RX_ERROR);
    CHECK(ioc_wd_rx_push_ex(&rx, frame, 65, 0, &done) == IOC_WD_RX_ERROR);
}

static void test_assoc_flags(void) {
    uint8_t state;
    int a, s;
    ioc_assoc_unpack(ioc_assoc_pack(IOC_ASSOC_ONLINE, 1, 1), &state, &a, &s);
    CHECK(state == IOC_ASSOC_ONLINE && a == 1 && s == 1);
    ioc_assoc_unpack(ioc_assoc_pack(IOC_ASSOC_LOST, 0, 0), &state, &a, &s);
    CHECK(state == IOC_ASSOC_LOST && a == 0 && s == 0);
    CHECK(ioc_assoc_pack(IOC_ASSOC_ONLINE, 0, 0) == IOC_ASSOC_ONLINE); /* compatível com o formato anterior */
    ioc_assoc_unpack(ioc_assoc_pack(IOC_ASSOC_REJECTED, 1, 1), &state, &a, &s);
    CHECK(state == IOC_ASSOC_REJECTED && a == 0 && s == 0);
    ioc_assoc_unpack(ioc_assoc_pack(IOC_ASSOC_UNBOUND, 1, 0), &state, &a, &s);
    CHECK(state == IOC_ASSOC_UNBOUND && a == 1);
}

static void test_failover(void) {
    const uint64_t U = 0xE6616408432B6F39ull;
    ioc_fo_table t;
    ioc_fo_decision d;
    uint32_t now = 1000;

    /* Política MANUAL: nunca reassocia sozinho. */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, now, 2, 1, U, IOC_ASSOC_ONLINE, 0, 1);
    ioc_fo_on_observation(&t, now, 3, U, -50);
    now += 60000;
    ioc_fo_on_observation(&t, now, 3, U, -50);
    CHECK(!ioc_fo_poll(&t, now, &d));

    /* Política AUTO, responsável ativo: nada a fazer. */
    ioc_fo_init(&t);
    now = 1000;
    ioc_fo_on_status(&t, now, 2, 1, U, IOC_ASSOC_ONLINE, 1, 1);
    for (int i = 0; i < 6; ++i) {
        now += 10000;
        ioc_fo_on_status(&t, now, 2, 1, U, IOC_ASSOC_ONLINE, 1, 1);
        ioc_fo_on_observation(&t, now, 3, U, -60);
        ioc_fo_on_observation(&t, now, 4, U, -48);
        CHECK(!ioc_fo_poll(&t, now, &d));
    }

    /* O responsável silencia: após 25 s o melhor observador assume. */
    now += 20000;
    ioc_fo_on_observation(&t, now, 3, U, -60);
    ioc_fo_on_observation(&t, now, 4, U, -48);
    CHECK(!ioc_fo_poll(&t, now, &d));
    now += 6000;
    ioc_fo_on_observation(&t, now, 3, U, -60);
    ioc_fo_on_observation(&t, now, 4, U, -48);
    CHECK(ioc_fo_poll(&t, now, &d));
    CHECK(d.uuid == U && d.from_node == 2 && d.to_node == 4 && d.reason == IOC_FO_REASON_OWNER_LOST);
    CHECK(d.rssi == -48);
    CHECK(!ioc_fo_poll(&t, now + 1000, &d)); /* aguarda o intervalo mínimo */

    /* O novo responsável confirma: a tabela passa a apontar para ele. */
    ioc_fo_on_status(&t, now + 2000, 4, 1, U, IOC_ASSOC_ASSOCIATING, 1, 0);
    CHECK(ioc_fo_find(&t, U)->owner == 4);
    ioc_fo_on_status(&t, now + 8000, 4, 1, U, IOC_ASSOC_ONLINE, 1, 1);
    ioc_fo_on_status(&t, now + 38000, 4, 1, U, IOC_ASSOC_ONLINE, 1, 1);
    ioc_fo_on_observation(&t, now + 39000, 3, U, -60);
    CHECK(!ioc_fo_poll(&t, now + 40000, &d)); /* novo responsável ativo: nada a decidir */

    /* O antigo responsável volta sem sessão: não desloca quem tem sessão. */
    ioc_fo_on_status(&t, now + 41000, 2, 1, U, IOC_ASSOC_ONLINE, 1, 0);
    CHECK(ioc_fo_find(&t, U)->owner == 4);
    CHECK(ioc_fo_should_yield(2, IOC_ASSOC_ONLINE, 0, 4, IOC_ASSOC_ONLINE, 1) == 1);
    CHECK(ioc_fo_should_yield(4, IOC_ASSOC_ONLINE, 1, 2, IOC_ASSOC_ONLINE, 0) == 0);

    /* Se o novo responsável não confirmar, o líder tenta de novo após o intervalo. */
    ioc_fo_init(&t);
    now = 1000;
    ioc_fo_on_status(&t, now, 2, 1, U, IOC_ASSOC_ONLINE, 1, 1);
    now += 30000;
    ioc_fo_on_observation(&t, now, 3, U, -70);
    CHECK(ioc_fo_poll(&t, now, &d) && d.to_node == 3);
    now += IOC_FO_COOLDOWN_MS - 1;
    ioc_fo_on_observation(&t, now, 3, U, -70);
    CHECK(!ioc_fo_poll(&t, now, &d));
    now += 2;
    ioc_fo_on_observation(&t, now, 5, U, -55);
    CHECK(ioc_fo_poll(&t, now, &d) && d.to_node == 5);

    /* Ausência detectada pelo líder antecipa a decisão. */
    ioc_fo_init(&t);
    now = 1000;
    ioc_fo_on_status(&t, now, 2, 1, U, IOC_ASSOC_ONLINE, 1, 1);
    ioc_fo_on_observation(&t, now, 3, U, -70);
    CHECK(!ioc_fo_poll(&t, now + 3000, &d));
    ioc_fo_on_node_fault(&t, 2);
    ioc_fo_on_observation(&t, now + 5000, 3, U, -70);
    CHECK(ioc_fo_poll(&t, now + 5000, &d) && d.reason == IOC_FO_REASON_OWNER_LOST && d.to_node == 3);

    /* Sensor fora do alcance do responsável, que continua ativo. */
    ioc_fo_init(&t);
    now = 1000;
    ioc_fo_on_status(&t, now, 2, 1, U, IOC_ASSOC_STALE, 1, 1);
    ioc_fo_on_observation(&t, now, 3, U, -65);
    CHECK(!ioc_fo_poll(&t, now, &d)); /* sem anúncios, mas com sessão: o sensor fica */
    ioc_fo_on_status(&t, now + 100, 2, 1, U, IOC_ASSOC_STALE, 1, 0);
    CHECK(ioc_fo_poll(&t, now + 100, &d));
    CHECK(d.reason == IOC_FO_REASON_SENSOR_LOST && d.from_node == 2 && d.to_node == 3);

    /* Sem outro observador, ou com sinal fraco demais, ninguém assume. */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, 1000, 2, 1, U, IOC_ASSOC_LOST, 1, 0);
    CHECK(!ioc_fo_poll(&t, 1000, &d));
    ioc_fo_on_observation(&t, 1000, 3, U, -95);
    CHECK(!ioc_fo_poll(&t, 1000, &d));
    ioc_fo_on_observation(&t, 1000, 3, U, -80);
    CHECK(!ioc_fo_poll(&t, 1000 + IOC_FO_OBSERVATION_AGE_MS + 1, &d)); /* observação vencida */

    /* UNBOUND do responsável encerra; de outro Node é ignorado. */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, 1000, 2, 1, U, IOC_ASSOC_ONLINE, 1, 1);
    ioc_fo_on_status(&t, 1100, 3, 1, U, IOC_ASSOC_UNBOUND, 0, 0);
    CHECK(ioc_fo_find(&t, U) != NULL);
    ioc_fo_on_status(&t, 1200, 2, 1, U, IOC_ASSOC_UNBOUND, 0, 0);
    CHECK(ioc_fo_find(&t, U) == NULL);
    ioc_fo_on_status(&t, 1300, 2, 0, U, IOC_ASSOC_REJECTED, 0, 0);
    CHECK(ioc_fo_find(&t, U) == NULL);

    /* Empate de RSSI: menor identificador. Conflito sem sessão: maior identificador fica. */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, 1000, 2, 1, U, IOC_ASSOC_LOST, 1, 0);
    ioc_fo_on_observation(&t, 1000, 5, U, -60);
    ioc_fo_on_observation(&t, 1000, 3, U, -60);
    CHECK(ioc_fo_poll(&t, 1000, &d) && d.to_node == 3);
    CHECK(ioc_fo_should_yield(2, IOC_ASSOC_ONLINE, 0, 4, IOC_ASSOC_BOUND, 0) == 1);
    CHECK(ioc_fo_should_yield(4, IOC_ASSOC_ONLINE, 0, 2, IOC_ASSOC_BOUND, 0) == 0);
    CHECK(ioc_fo_should_yield(2, IOC_ASSOC_ONLINE, 0, 4, IOC_ASSOC_LOST, 0) == 0);
    CHECK(ioc_fo_should_yield(2, IOC_ASSOC_ONLINE, 1, 4, IOC_ASSOC_ONLINE, 1) == 0);
    /* Quem já declarou o sensor LOST cede a quem está assumindo, qualquer que seja o identificador. */
    CHECK(ioc_fo_should_yield(4, IOC_ASSOC_LOST, 0, 2, IOC_ASSOC_ASSOCIATING, 0) == 1);
    CHECK(ioc_fo_should_yield(4, IOC_ASSOC_STALE, 0, 2, IOC_ASSOC_ASSOCIATING, 0) == 1);
    CHECK(ioc_fo_should_yield(4, IOC_ASSOC_STALE, 1, 2, IOC_ASSOC_ASSOCIATING, 0) == 0);

    /* LOST pelo rádio BLE, mas com sessão de dados ativa: o sensor fica onde está. */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, 1000, 2, 1, U, IOC_ASSOC_LOST, 1, 1);
    ioc_fo_on_observation(&t, 1000, 3, U, -50);
    CHECK(!ioc_fo_poll(&t, 1000, &d));

    /*
     * O responsável reiniciou: perdeu o vínculo, mas voltou a observar o
     * sensor. Sem outro observador, o vínculo é devolvido a ele.
     */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, 1000, 2, 1, U, IOC_ASSOC_ONLINE, 1, 1);
    ioc_fo_on_observation(&t, 30000, 2, U, -55);
    CHECK(ioc_fo_poll(&t, 30000, &d));
    CHECK(d.reason == IOC_FO_REASON_OWNER_LOST && d.from_node == 2 && d.to_node == 2);
    /* A observação que o responsável fez antes de sair do ar não o torna candidato. */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, 1000, 2, 1, U, IOC_ASSOC_ONLINE, 1, 1);
    ioc_fo_on_observation(&t, 4000, 2, U, -40);
    ioc_fo_on_node_fault(&t, 2);
    CHECK(!ioc_fo_poll(&t, 6000, &d));
    ioc_fo_on_observation(&t, 6000, 3, U, -75);
    CHECK(ioc_fo_poll(&t, 6000, &d) && d.to_node == 3);
    /* Com outro observador melhor, ele é o escolhido. */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, 1000, 2, 1, U, IOC_ASSOC_ONLINE, 1, 1);
    ioc_fo_on_observation(&t, 30000, 2, U, -70);
    ioc_fo_on_observation(&t, 30000, 3, U, -50);
    CHECK(ioc_fo_poll(&t, 30000, &d) && d.to_node == 3);

    /*
     * O intervalo mínimo entre decisões vale também depois da troca de
     * responsável: o novo Node não consegue o sensor e o declara STALE logo
     * em seguida; a próxima decisão espera o intervalo.
     */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, 1000, 2, 1, U, IOC_ASSOC_STALE, 1, 0);
    ioc_fo_on_observation(&t, 1000, 3, U, -60);
    ioc_fo_on_observation(&t, 1000, 4, U, -70);
    CHECK(ioc_fo_poll(&t, 1000, &d) && d.to_node == 3);
    ioc_fo_on_status(&t, 1200, 3, 1, U, IOC_ASSOC_ASSOCIATING, 1, 0);
    CHECK(ioc_fo_find(&t, U)->owner == 3);
    ioc_fo_on_status(&t, 9000, 3, 1, U, IOC_ASSOC_STALE, 1, 0);
    ioc_fo_on_observation(&t, 9000, 4, U, -70);
    CHECK(!ioc_fo_poll(&t, 9000, &d));
    ioc_fo_on_observation(&t, 1000 + IOC_FO_COOLDOWN_MS, 4, U, -70);
    CHECK(ioc_fo_poll(&t, 1000 + IOC_FO_COOLDOWN_MS, &d));
    CHECK(d.from_node == 3 && d.to_node == 4 && d.reason == IOC_FO_REASON_SENSOR_LOST);

    /* O destino recusa ou não responde: o vínculo anterior continua na tabela e há nova tentativa. */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, 1000, 2, 1, U, IOC_ASSOC_LOST, 1, 0);
    ioc_fo_on_observation(&t, 1000, 3, U, -60);
    CHECK(ioc_fo_poll(&t, 1000, &d) && d.to_node == 3);
    ioc_fo_on_status(&t, 1100, 3, 0, U, IOC_ASSOC_REJECTED, 0, 0);
    ioc_fo_on_status(&t, 11000, 2, 1, U, IOC_ASSOC_LOST, 1, 0);
    CHECK(ioc_fo_find(&t, U) != NULL && ioc_fo_find(&t, U)->owner == 2);
    ioc_fo_on_observation(&t, 1000 + IOC_FO_COOLDOWN_MS, 4, U, -72);
    CHECK(ioc_fo_poll(&t, 1000 + IOC_FO_COOLDOWN_MS, &d) && d.from_node == 2 && d.to_node == 4);

    /* Um Node declarado ausente deixa de ser destino, mesmo com observação recente. */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, 1000, 2, 1, U, IOC_ASSOC_STALE, 1, 0);
    ioc_fo_on_observation(&t, 1000, 3, U, -50);
    ioc_fo_on_observation(&t, 1000, 4, U, -80);
    ioc_fo_on_node_fault(&t, 3);
    CHECK(ioc_fo_poll(&t, 1500, &d) && d.to_node == 4);

    /*
     * O próprio Node ficou parado: ao retomar, o silêncio dos demais não
     * conta e as observações antigas não servem de base para decidir.
     */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, 1000, 2, 1, U, IOC_ASSOC_ONLINE, 1, 1);
    ioc_fo_on_observation(&t, 1000, 3, U, -40);
    ioc_fo_on_local_stall(&t, 45000);
    CHECK(!ioc_fo_poll(&t, 45000, &d));
    ioc_fo_on_observation(&t, 46000, 3, U, -40);
    CHECK(!ioc_fo_poll(&t, 46000, &d));          /* o responsável ainda está dentro do prazo */
    ioc_fo_on_status(&t, 50000, 2, 1, U, IOC_ASSOC_ONLINE, 1, 1);
    CHECK(!ioc_fo_poll(&t, 60000, &d));

    /* Tabela cheia e vínculos sem notícias há muito tempo. */
    ioc_fo_init(&t);
    for (unsigned i = 0; i < IOC_FO_MAX_ENTRIES + 3; ++i) {
        ioc_fo_on_status(&t, 1000 + i, 2, (uint8_t)(1 + i % 4), U + i, IOC_ASSOC_ONLINE, 0, 1);
    }
    CHECK(ioc_fo_find(&t, U + IOC_FO_MAX_ENTRIES + 2) != NULL);
    CHECK(ioc_fo_find(&t, U) == NULL); /* o mais antigo cedeu lugar */
    CHECK(!ioc_fo_poll(&t, 1000 + IOC_FO_PURGE_MS + 100, &d));
    CHECK(ioc_fo_find(&t, U + 5) == NULL);
}

static void print_vectors(void) {
    uint8_t data[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    uint8_t key[32], pdu[32];
    uint64_t tx = 0x0102030405u;
    char hex[80];
    printf("crc15_0x123_8=%04x\n", ioc_can_crc15(0x123, 8, data));
    printf("bits_0x123_8=%u\n", ioc_can_frame_bits(0x123, 8, data));
    printf("crc15_0x100_5=%04x\n", ioc_can_crc15(0x100, 5, data));
    printf("bits_0x100_5=%u\n", ioc_can_frame_bits(0x100, 5, data));
    printf("bits_0x000_0=%u\n", ioc_can_frame_bits(0x000, 0, data));
    printf("bits_0x7ff_8_ff=%u\n", ioc_can_frame_bits(0x7FF, 8, (const uint8_t *)"\xff\xff\xff\xff\xff\xff\xff\xff"));
    for (unsigned dlc = 0; dlc <= 8; ++dlc) printf("worst_%u=%u\n", dlc, ioc_can_frame_bits_worst((uint8_t)dlc));
    for (int i = 0; i < 32; ++i) key[i] = (uint8_t)i;
    size_t n = ioc_secpdu_protect(key, 0x304, data, 8, &tx, IOC_SECPDU_MAC_FD, pdu, sizeof(pdu));
    for (size_t i = 0; i < n; ++i) sprintf(&hex[i * 2], "%02x", pdu[i]);
    printf("secpdu=%s\n", hex);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--vectors") == 0) {
        print_vectors();
        return 0;
    }
    test_can_bits();
    test_secpdu();
    test_segmentation_fd();
    test_assoc_flags();
    test_failover();
    if (g_failures) {
        fprintf(stderr, "ioc_net: %d falha(s)\n", g_failures);
        return 1;
    }
    printf("ioc_net native tests: PASS\n");
    return 0;
}
