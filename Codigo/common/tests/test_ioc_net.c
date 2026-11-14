/*
 * Testes nativos: duração de quadros CAN, PDU autenticada (predisposição
 * CAN FD), segmentação com quadros maiores, reassociação automática e a
 * tabela de vínculos gravada pelos Nodes.
 *
 *   test_ioc_net            executa as verificações
 *   test_ioc_net --vectors  imprime vetores para conferência com Python
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ioc_bindstore.h"
#include "ioc_canbits.h"
#include "ioc_failover.h"
#include "ioc_secpdu.h"
#include "ioc_time.h"
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

/* Observações a cada 5 s de `from` até `to` (o reenvio dos Nodes). Com
 * from = to - IOC_FO_SETTLE_MS, o candidato observa o sensor há um ciclo. */
static void observe_since(ioc_fo_table *t, uint64_t uuid, uint8_t node, int8_t rssi, uint32_t from, uint32_t to) {
    for (uint32_t x = from; (int32_t)(to - x) > 0; x += 5000u) ioc_fo_on_observation(t, x, node, uuid, rssi);
    ioc_fo_on_observation(t, to, node, uuid, rssi);
}

typedef struct {
    uint8_t node;
    int8_t rssi;
} fo_observer;

/*
 * Avança o tempo de `*clock` até `to`, como a rede faria: cada observador
 * reenvia a observação a cada 5 s e o responsável (se `owner` != 0) republica o
 * estado a cada 10 s, nos múltiplos desses períodos a partir de `origin`. As
 * chamadas seguem a ordem do tempo: a tabela não aceita eventos do futuro.
 */
static void fo_run(ioc_fo_table *t, uint64_t uuid, uint32_t origin, uint32_t *clock, uint32_t to, uint8_t owner,
                   uint8_t state, int secure, const fo_observer *obs, unsigned count) {
    for (uint32_t k = (*clock - origin) / 5000u + 1u;; ++k) {
        const uint32_t x = origin + k * 5000u;
        if ((int32_t)(to - x) < 0) break;
        for (unsigned i = 0; i < count; ++i) ioc_fo_on_observation(t, x, obs[i].node, uuid, obs[i].rssi);
        if (owner != 0 && k % 2u == 0) ioc_fo_on_status(t, x, owner, 1, uuid, state, 1, secure);
    }
    *clock = to;
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

    /* Sensor fora do alcance do responsável, que continua ativo. O outro Node
     * já observava o sensor havia mais de um ciclo de reenvio. */
    ioc_fo_init(&t);
    now = 1000;
    ioc_fo_on_status(&t, now, 2, 1, U, IOC_ASSOC_ONLINE, 1, 1);
    ioc_fo_on_observation(&t, now, 3, U, -65);
    now += IOC_FO_SETTLE_MS;
    ioc_fo_on_status(&t, now, 2, 1, U, IOC_ASSOC_STALE, 1, 1);
    ioc_fo_on_observation(&t, now, 3, U, -65);
    CHECK(!ioc_fo_poll(&t, now, &d)); /* sem anúncios, mas com sessão: o sensor fica */
    ioc_fo_on_status(&t, now + 100, 2, 1, U, IOC_ASSOC_STALE, 1, 0);
    CHECK(ioc_fo_poll(&t, now + 100, &d));
    CHECK(d.reason == IOC_FO_REASON_SENSOR_LOST && d.from_node == 2 && d.to_node == 3);

    /*
     * O sensor reaparece depois de ficar LOST: todos os Nodes o observam quase
     * ao mesmo tempo. O líder não o move enquanto o candidato não o observar
     * por um ciclo inteiro, nem quando o responsável já voltou a ouvi-lo, ainda
     * que o novo estado dele (ONLINE) não tenha chegado.
     */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, 55000, 1, 1, U, IOC_ASSOC_LOST, 1, 0);
    ioc_fo_on_observation(&t, 60002, 2, U, -68);
    CHECK(!ioc_fo_poll(&t, 60003, &d));         /* candidato recém-chegado */
    ioc_fo_on_observation(&t, 60004, 1, U, -50); /* o responsável também ouve */
    ioc_fo_on_observation(&t, 65002, 2, U, -68);
    CHECK(!ioc_fo_poll(&t, 66500, &d));         /* responsável voltando: fica com ele */
    ioc_fo_on_status(&t, 66600, 1, 1, U, IOC_ASSOC_ONLINE, 1, 0);
    CHECK(!ioc_fo_poll(&t, 67000, &d));
    /* Sem o responsável ouvir o sensor, o candidato assume depois do prazo. */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, 55000, 1, 1, U, IOC_ASSOC_LOST, 1, 0);
    ioc_fo_on_observation(&t, 60002, 2, U, -68);
    ioc_fo_on_observation(&t, 65002, 2, U, -68);
    CHECK(!ioc_fo_poll(&t, 65003, &d));
    CHECK(ioc_fo_poll(&t, 60002 + IOC_FO_SETTLE_MS, &d) && d.to_node == 2 && d.reason == IOC_FO_REASON_SENSOR_LOST);
    /* Uma observação vencida reinicia a contagem. */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, 1000, 1, 1, U, IOC_ASSOC_LOST, 1, 0);
    ioc_fo_on_observation(&t, 1000, 2, U, -60);
    ioc_fo_on_observation(&t, 1000 + IOC_FO_OBSERVATION_AGE_MS + 1, 2, U, -60);
    CHECK(!ioc_fo_poll(&t, 1000 + IOC_FO_OBSERVATION_AGE_MS + 2, &d));

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
    ioc_fo_on_observation(&t, 1000 + IOC_FO_SETTLE_MS, 5, U, -60);
    ioc_fo_on_observation(&t, 1000 + IOC_FO_SETTLE_MS, 3, U, -60);
    CHECK(ioc_fo_poll(&t, 1000 + IOC_FO_SETTLE_MS, &d) && d.to_node == 3);
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
     * responsável: o novo Node teve sessão com o sensor, mas a perde e o
     * declara STALE logo em seguida; a próxima decisão espera o intervalo.
     */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, 1000, 2, 1, U, IOC_ASSOC_STALE, 1, 0);
    observe_since(&t, U, 3, -60, 1000 - IOC_FO_SETTLE_MS, 1000);
    observe_since(&t, U, 4, -70, 1000 - IOC_FO_SETTLE_MS, 1000);
    CHECK(ioc_fo_poll(&t, 1000, &d) && d.to_node == 3);
    ioc_fo_on_status(&t, 1200, 3, 1, U, IOC_ASSOC_ASSOCIATING, 1, 0);
    CHECK(ioc_fo_find(&t, U)->owner == 3);
    ioc_fo_on_status(&t, 5000, 3, 1, U, IOC_ASSOC_ONLINE, 1, 1);      /* abriu o canal de dados */
    ioc_fo_on_status(&t, 9000, 3, 1, U, IOC_ASSOC_STALE, 1, 0);       /* e o perdeu */
    observe_since(&t, U, 4, -70, 6000, 9000);
    CHECK(!ioc_fo_poll(&t, 9000, &d));
    observe_since(&t, U, 4, -70, 9000, 1000 + IOC_FO_COOLDOWN_MS);
    CHECK(ioc_fo_poll(&t, 1000 + IOC_FO_COOLDOWN_MS, &d));
    CHECK(d.from_node == 3 && d.to_node == 4 && d.reason == IOC_FO_REASON_SENSOR_LOST);

    /*
     * O novo responsável ainda não teve sessão: o canal de dados está sendo
     * criado e o Node, com o rádio dividido, deixa de ouvir o sensor (STALE).
     * Além do intervalo mínimo, o líder espera o prazo da primeira sessão,
     * contado da assunção, antes de mover o sensor outra vez.
     */
    {
        const fo_observer only4[] = {{4, -70}};
        uint32_t clock = 1000;
        ioc_fo_init(&t);
        ioc_fo_on_status(&t, 1000, 2, 1, U, IOC_ASSOC_STALE, 1, 0);
        observe_since(&t, U, 3, -60, 1000 - IOC_FO_SETTLE_MS, 1000);
        observe_since(&t, U, 4, -70, 1000 - IOC_FO_SETTLE_MS, 1000);
        CHECK(ioc_fo_poll(&t, 1000, &d) && d.to_node == 3);
        ioc_fo_on_status(&t, 1200, 3, 1, U, IOC_ASSOC_ASSOCIATING, 1, 0);
        fo_run(&t, U, 1000, &clock, 9000, 3, IOC_ASSOC_ASSOCIATING, 0, only4, 1);
        ioc_fo_on_status(&t, 9000, 3, 1, U, IOC_ASSOC_STALE, 1, 0);
        fo_run(&t, U, 1000, &clock, 1000 + IOC_FO_COOLDOWN_MS, 3, IOC_ASSOC_STALE, 0, only4, 1);
        CHECK(!ioc_fo_poll(&t, 1000 + IOC_FO_COOLDOWN_MS, &d));
        fo_run(&t, U, 1000, &clock, 1200 + IOC_FO_FIRST_SESSION_GRACE_MS - 1, 3, IOC_ASSOC_STALE, 0, only4, 1);
        CHECK(!ioc_fo_poll(&t, 1200 + IOC_FO_FIRST_SESSION_GRACE_MS - 1, &d));
        fo_run(&t, U, 1000, &clock, 1200 + IOC_FO_FIRST_SESSION_GRACE_MS, 3, IOC_ASSOC_STALE, 0, only4, 1);
        CHECK(ioc_fo_poll(&t, 1200 + IOC_FO_FIRST_SESSION_GRACE_MS, &d));
        CHECK(d.from_node == 3 && d.to_node == 4 && d.reason == IOC_FO_REASON_SENSOR_LOST);
    }

    /* O destino recusa ou não responde: o vínculo anterior continua na tabela e há nova tentativa. */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, 1000, 2, 1, U, IOC_ASSOC_LOST, 1, 0);
    observe_since(&t, U, 3, -60, 1000 - IOC_FO_SETTLE_MS, 1000);
    CHECK(ioc_fo_poll(&t, 1000, &d) && d.to_node == 3);
    ioc_fo_on_status(&t, 1100, 3, 0, U, IOC_ASSOC_REJECTED, 0, 0);
    ioc_fo_on_status(&t, 11000, 2, 1, U, IOC_ASSOC_LOST, 1, 0);
    CHECK(ioc_fo_find(&t, U) != NULL && ioc_fo_find(&t, U)->owner == 2);
    observe_since(&t, U, 4, -72, 1000 + IOC_FO_COOLDOWN_MS - IOC_FO_SETTLE_MS, 1000 + IOC_FO_COOLDOWN_MS);
    CHECK(ioc_fo_poll(&t, 1000 + IOC_FO_COOLDOWN_MS, &d) && d.from_node == 2 && d.to_node == 4);

    /* Um Node declarado ausente deixa de ser destino, mesmo com observação recente. */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, 1000, 2, 1, U, IOC_ASSOC_STALE, 1, 0);
    observe_since(&t, U, 3, -50, 1500 - IOC_FO_SETTLE_MS, 1500);
    observe_since(&t, U, 4, -80, 1500 - IOC_FO_SETTLE_MS, 1500);
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

/*
 * Primeiro acesso com reassociação automática. Enquanto o canal de dados está
 * sendo criado, o Node responsável fica surdo ao BLE (rádio dividido com o
 * ponto de acesso) e declara o sensor STALE em 15 s. O líder não pode mover o
 * sensor nesse intervalo: recomeçaria a criação em outro Node, que também fica
 * surdo, e o canal nunca abriria (a política MANUAL não tinha o problema).
 */
static void test_failover_first_session(void) {
    const uint64_t U = 0xE6616408432B6F39ull;
    const uint32_t GRACE = IOC_FO_FIRST_SESSION_GRACE_MS;
    /* Os Nodes 2 e 3 ouvem o sensor o tempo todo; o 2 com o sinal melhor. */
    const fo_observer others[] = {{2, -64}, {3, -70}};
    ioc_fo_table t;
    ioc_fo_decision d;
    uint32_t clock;

    /* O operador associa ao Node 1; sem sessão, o vínculo vira STALE 15,7 s depois. */
    const uint32_t bind = 20000;
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, bind, 1, 1, U, IOC_ASSOC_ASSOCIATING, 1, 0);
    ioc_fo_on_status(&t, bind + 250, 1, 1, U, IOC_ASSOC_BOUND, 1, 0);
    clock = bind + 250;
    fo_run(&t, U, bind, &clock, bind + 15700, 1, IOC_ASSOC_BOUND, 0, others, 2);
    ioc_fo_on_status(&t, bind + 15700, 1, 1, U, IOC_ASSOC_STALE, 1, 0);
    clock = bind + 15700;
    fo_run(&t, U, bind, &clock, bind + 15900, 1, IOC_ASSOC_STALE, 0, others, 2);
    CHECK(!ioc_fo_poll(&t, bind + 15900, &d));              /* STALE sem sessão, mas o canal está sendo criado */
    fo_run(&t, U, bind, &clock, bind + 60000, 1, IOC_ASSOC_STALE, 0, others, 2);
    CHECK(!ioc_fo_poll(&t, bind + 60000, &d));
    fo_run(&t, U, bind, &clock, bind + GRACE - 1, 1, IOC_ASSOC_STALE, 0, others, 2);
    CHECK(!ioc_fo_poll(&t, bind + GRACE - 1, &d));
    /* Passado o prazo sem sessão, o sensor volta a poder ser movido. */
    fo_run(&t, U, bind, &clock, bind + GRACE, 1, IOC_ASSOC_STALE, 0, others, 2);
    CHECK(ioc_fo_poll(&t, bind + GRACE, &d));
    CHECK(d.reason == IOC_FO_REASON_SENSOR_LOST && d.from_node == 1 && d.to_node == 2);

    /* Mesmo vínculo, sem prazo: visto pela primeira vez já STALE (líder recém-iniciado), o líder age como antes. */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, bind, 1, 1, U, IOC_ASSOC_STALE, 1, 0);
    clock = bind;
    fo_run(&t, U, bind, &clock, bind + 16000, 1, IOC_ASSOC_STALE, 0, others, 2);
    CHECK(ioc_fo_poll(&t, bind + 16000, &d) && d.reason == IOC_FO_REASON_SENSOR_LOST);

    /* A sessão abriu dentro do prazo: acabou a espera. Se depois o sensor some, o líder age sem demora. */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, bind, 1, 1, U, IOC_ASSOC_ASSOCIATING, 1, 0);
    ioc_fo_on_status(&t, bind + 250, 1, 1, U, IOC_ASSOC_BOUND, 1, 0);
    clock = bind + 250;
    fo_run(&t, U, bind, &clock, bind + 15700, 1, IOC_ASSOC_BOUND, 0, others, 2);
    ioc_fo_on_status(&t, bind + 15700, 1, 1, U, IOC_ASSOC_STALE, 1, 0);
    fo_run(&t, U, bind, &clock, bind + 41000, 1, IOC_ASSOC_STALE, 0, others, 2);
    ioc_fo_on_status(&t, bind + 41000, 1, 1, U, IOC_ASSOC_STALE, 1, 1);   /* sessão aberta */
    CHECK(!ioc_fo_poll(&t, bind + 41200, &d));
    ioc_fo_on_status(&t, bind + 45000, 1, 1, U, IOC_ASSOC_ONLINE, 1, 1);
    fo_run(&t, U, bind, &clock, bind + 60000, 1, IOC_ASSOC_ONLINE, 1, others, 2);
    ioc_fo_on_status(&t, bind + 60500, 1, 1, U, IOC_ASSOC_STALE, 1, 0);   /* o sensor sumiu: sem sessão e sem BLE */
    clock = bind + 60500;
    fo_run(&t, U, bind, &clock, bind + 61000, 1, IOC_ASSOC_STALE, 0, others, 2);
    CHECK(ioc_fo_poll(&t, bind + 61000, &d));
    CHECK(d.reason == IOC_FO_REASON_SENSOR_LOST && d.from_node == 1 && d.to_node == 2);

    /* O responsável sai do ar durante a criação do canal: OWNER_LOST não espera o prazo. */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, bind, 1, 1, U, IOC_ASSOC_ASSOCIATING, 1, 0);
    clock = bind;
    fo_run(&t, U, bind, &clock, bind + 5000, 1, IOC_ASSOC_ASSOCIATING, 0, others, 2);
    ioc_fo_on_node_fault(&t, 1);
    fo_run(&t, U, bind, &clock, bind + 10000, 0, 0, 0, others, 2);
    CHECK(ioc_fo_poll(&t, bind + 10000, &d));
    CHECK(d.reason == IOC_FO_REASON_OWNER_LOST && d.from_node == 1 && d.to_node == 2);
    ioc_fo_init(&t);                                        /* ou deixa de publicar o vínculo */
    ioc_fo_on_status(&t, bind, 1, 1, U, IOC_ASSOC_ASSOCIATING, 1, 0);
    clock = bind;
    fo_run(&t, U, bind, &clock, bind + IOC_FO_OWNER_SILENT_MS + 2000, 0, 0, 0, others, 2);
    CHECK(ioc_fo_poll(&t, bind + IOC_FO_OWNER_SILENT_MS + 2000, &d) && d.reason == IOC_FO_REASON_OWNER_LOST);

    /* O Node que reiniciou e retomou o vínculo da NVS recomeça a criação do canal: novo prazo. */
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, bind, 1, 1, U, IOC_ASSOC_ONLINE, 1, 1);
    clock = bind;
    fo_run(&t, U, bind, &clock, bind + 10000, 1, IOC_ASSOC_ONLINE, 1, others, 2);
    ioc_fo_on_status(&t, bind + 11000, 1, 1, U, IOC_ASSOC_ASSOCIATING, 1, 0);   /* reiniciou e retomou */
    clock = bind + 11000;
    fo_run(&t, U, bind, &clock, bind + 26000, 1, IOC_ASSOC_ASSOCIATING, 0, others, 2);
    ioc_fo_on_status(&t, bind + 26000, 1, 1, U, IOC_ASSOC_STALE, 1, 0);
    fo_run(&t, U, bind, &clock, bind + 40000, 1, IOC_ASSOC_STALE, 0, others, 2);
    CHECK(!ioc_fo_poll(&t, bind + 40000, &d));
    fo_run(&t, U, bind, &clock, bind + 11000 + GRACE - 1, 1, IOC_ASSOC_STALE, 0, others, 2);
    CHECK(!ioc_fo_poll(&t, bind + 11000 + GRACE - 1, &d));
    fo_run(&t, U, bind, &clock, bind + 11000 + GRACE, 1, IOC_ASSOC_STALE, 0, others, 2);
    CHECK(ioc_fo_poll(&t, bind + 11000 + GRACE, &d) && d.reason == IOC_FO_REASON_SENSOR_LOST);

    /* O prazo atravessa a volta do relógio de 32 bits. */
    const uint32_t near_wrap = 0xFFFFFFFFu - 50000u;
    ioc_fo_init(&t);
    ioc_fo_on_status(&t, near_wrap, 1, 1, U, IOC_ASSOC_ASSOCIATING, 1, 0);
    clock = near_wrap;
    fo_run(&t, U, near_wrap, &clock, near_wrap + 16000, 1, IOC_ASSOC_ASSOCIATING, 0, others, 2);
    ioc_fo_on_status(&t, near_wrap + 16000, 1, 1, U, IOC_ASSOC_STALE, 1, 0);
    fo_run(&t, U, near_wrap, &clock, near_wrap + 60000, 1, IOC_ASSOC_STALE, 0, others, 2);
    CHECK((int32_t)(near_wrap + 60000 - near_wrap) > 0 && near_wrap + 60000 < near_wrap);   /* passou do zero */
    CHECK(!ioc_fo_poll(&t, near_wrap + 60000, &d));
    fo_run(&t, U, near_wrap, &clock, near_wrap + GRACE - 1, 1, IOC_ASSOC_STALE, 0, others, 2);
    CHECK(!ioc_fo_poll(&t, near_wrap + GRACE - 1, &d));
    fo_run(&t, U, near_wrap, &clock, near_wrap + GRACE, 1, IOC_ASSOC_STALE, 0, others, 2);
    CHECK(ioc_fo_poll(&t, near_wrap + GRACE, &d) && d.reason == IOC_FO_REASON_SENSOR_LOST);
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

static void test_bindstore(void) {
    /* CRC-32 do zlib: vetor clássico. */
    CHECK(ioc_bs_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);

    ioc_bs_entry in[3] = {
        {0xE6616408432B7A11ull, 1, 0x02, 6, IOC_BS_FLAG_AUTO_FAILOVER},
        {0xE6616408432B6F39ull, 2, 0x01, 6, 0},
        {0x0000000000000001ull, 4, 0x01, 5, 0},
    };
    uint8_t buf[IOC_BS_MAX_BYTES];
    const size_t len = ioc_bs_encode(2, in, 3, buf, sizeof(buf));
    CHECK(len == IOC_BS_HEADER_LEN + 3 * IOC_BS_ENTRY_LEN + IOC_BS_CRC_LEN);

    ioc_bs_entry out[IOC_BS_MAX];
    uint8_t count = 99;
    CHECK(ioc_bs_decode(buf, len, 2, out, IOC_BS_MAX, &count) == 1 && count == 3);
    for (int i = 0; i < 3; ++i) {
        CHECK(out[i].uuid == in[i].uuid && out[i].child_id == in[i].child_id);
        CHECK(out[i].profile == in[i].profile && out[i].protocol == in[i].protocol && out[i].flags == in[i].flags);
    }

    /* Outro Node, espaço insuficiente, tamanho errado, corrupção. */
    CHECK(ioc_bs_decode(buf, len, 3, out, IOC_BS_MAX, &count) == 0 && count == 0);
    CHECK(ioc_bs_decode(buf, len, 2, out, 2, &count) == 0);
    CHECK(ioc_bs_decode(buf, len - 1, 2, out, IOC_BS_MAX, &count) == 0);
    for (size_t i = 0; i < len; ++i) {
        buf[i] ^= 0x10;
        CHECK(ioc_bs_decode(buf, len, 2, out, IOC_BS_MAX, &count) == 0);
        buf[i] ^= 0x10;
    }
    CHECK(ioc_bs_decode(buf, len, 2, out, IOC_BS_MAX, &count) == 1);

    /* Filho repetido, filho fora da faixa e UUID zero são recusados mesmo com CRC correto. */
    ioc_bs_entry dup[2] = {{0x11, 1, 1, 6, 0}, {0x22, 1, 1, 6, 0}};
    size_t n = ioc_bs_encode(2, dup, 2, buf, sizeof(buf));
    CHECK(n > 0 && ioc_bs_decode(buf, n, 2, out, IOC_BS_MAX, &count) == 0);
    ioc_bs_entry same_uuid[2] = {{0x11, 1, 1, 6, 0}, {0x11, 2, 1, 6, 0}};
    n = ioc_bs_encode(2, same_uuid, 2, buf, sizeof(buf));
    CHECK(ioc_bs_decode(buf, n, 2, out, IOC_BS_MAX, &count) == 0);
    ioc_bs_entry bad_child[1] = {{0x11, IOC_BS_MAX + 1, 1, 6, 0}};
    n = ioc_bs_encode(2, bad_child, 1, buf, sizeof(buf));
    CHECK(ioc_bs_decode(buf, n, 2, out, IOC_BS_MAX, &count) == 0);
    ioc_bs_entry zero[1] = {{0, 1, 1, 6, 0}};
    n = ioc_bs_encode(2, zero, 1, buf, sizeof(buf));
    CHECK(ioc_bs_decode(buf, n, 2, out, IOC_BS_MAX, &count) == 0);

    /* Tabela vazia é válida (Node sem vínculos); limites de codificação. */
    n = ioc_bs_encode(5, NULL, 0, buf, sizeof(buf));
    CHECK(n == IOC_BS_HEADER_LEN + IOC_BS_CRC_LEN);
    CHECK(ioc_bs_decode(buf, n, 5, out, IOC_BS_MAX, &count) == 1 && count == 0);
    CHECK(ioc_bs_encode(5, in, IOC_BS_MAX + 1, buf, sizeof(buf)) == 0);
    CHECK(ioc_bs_encode(5, in, 3, buf, 10) == 0);
}

/* Relógio da rede: formato do quadro e regra de adoção. */
static void test_time(void) {
    uint8_t d[IOC_TIME_LEN];
    ioc_time_msg in = {IOC_TIME_KIND_TIME, IOC_TIME_SRC_TERMINAL, 1793000000999LL}, out;
    ioc_time_encode(&in, d);
    CHECK(d[0] == 1 && d[1] == 1 && d[6] == 0xE7 && d[7] == 0x03);
    CHECK(ioc_time_decode(d, sizeof(d), &out) && out.kind == in.kind && out.source == in.source &&
          out.utc_ms == in.utc_ms);
    CHECK(!ioc_time_decode(d, 7, &out));                    /* tamanho errado */
    d[6] = 0xE8;                                             /* 1000 ms */
    CHECK(!ioc_time_decode(d, sizeof(d), &out));
    in.source = IOC_TIME_SRC_NONE;
    ioc_time_encode(&in, d);
    CHECK(!ioc_time_decode(d, sizeof(d), &out));             /* hora sem fonte */
    in.source = IOC_TIME_SRC_RTC;
    in.utc_ms = 1000;
    ioc_time_encode(&in, d);
    CHECK(!ioc_time_decode(d, sizeof(d), &out));             /* relógio nunca ajustado */
    const ioc_time_msg req = {IOC_TIME_KIND_REQUEST, IOC_TIME_SRC_NONE, 0};
    ioc_time_encode(&req, d);
    CHECK(ioc_time_decode(d, sizeof(d), &out) && out.kind == IOC_TIME_KIND_REQUEST &&
          out.source == IOC_TIME_SRC_NONE && out.utc_ms == 0);
    d[0] = 3;
    CHECK(!ioc_time_decode(d, sizeof(d), &out));             /* tipo desconhecido */

    const int64_t t = 1793000000000LL;
    CHECK(ioc_time_adopt(IOC_TIME_SRC_NONE, 0, IOC_TIME_SRC_OPERATOR, t, 50));
    CHECK(ioc_time_adopt(IOC_TIME_SRC_RTC, t, IOC_TIME_SRC_TERMINAL, t + 1, 50));
    CHECK(!ioc_time_adopt(IOC_TIME_SRC_TERMINAL, t, IOC_TIME_SRC_RTC, t + 60000, 50));
    CHECK(!ioc_time_adopt(IOC_TIME_SRC_TERMINAL, t, IOC_TIME_SRC_TERMINAL, t + 50, 50));
    CHECK(ioc_time_adopt(IOC_TIME_SRC_TERMINAL, t, IOC_TIME_SRC_TERMINAL, t - 51, 50));
    CHECK(!ioc_time_adopt(IOC_TIME_SRC_NONE, 0, IOC_TIME_SRC_NONE, t, 50));
    CHECK(!ioc_time_adopt(IOC_TIME_SRC_NONE, 0, IOC_TIME_SRC_TERMINAL, 1000, 50));
    CHECK(strcmp(ioc_time_source_name(IOC_TIME_SRC_RTC), "RTC") == 0);
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
    test_failover_first_session();
    test_bindstore();
    test_time();
    if (g_failures) {
        fprintf(stderr, "ioc_net: %d falha(s)\n", g_failures);
        return 1;
    }
    printf("ioc_net native tests: PASS\n");
    return 0;
}
