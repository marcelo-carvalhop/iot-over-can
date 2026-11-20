/* Autenticação de origem dos comandos da Probe no CAN (0x304 e 0x305): etiqueta, repetição, falsificação, reinício. */
#include <stdio.h>
#include <string.h>

#include "ioc_canauth.h"
#include "ioc_secpdu.h"

static int failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            printf("FALHA %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static const uint8_t MASTER[IOC_LINK_KEY_LEN] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};

static void bus_key(uint8_t out[IOC_LINK_KEY_LEN]) { ioc_secpdu_derive_bus_key(MASTER, out); }

static const uint8_t CMD[8] = {0x11, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0xA5};

/* Um comando legítimo de ponta a ponta: a Probe monta a etiqueta, o módulo a confere. */
static ioc_ca_action send_legit(ioc_ca_tx *tx, ioc_ca_rx *rx, uint32_t now, uint16_t id, const uint8_t *cmd,
                                ioc_ca_frame *out) {
    uint8_t tag[IOC_CA_TAG_LEN];
    ioc_ca_why why;
    CHECK(ioc_ca_tx_make_tag(tx, id, cmd, 8, tag));
    CHECK(ioc_ca_rx_frame(rx, now, id, cmd, 8, NULL, &why) == IOC_CA_HOLD);
    return ioc_ca_rx_frame(rx, now + 2, IOC_CA_TAG_ID, tag, IOC_CA_TAG_LEN, out, &why);
}

static void test_the_secpdu_variant_with_other_freshness_lengths(void) {
    uint8_t key[IOC_LINK_KEY_LEN];
    bus_key(key);
    uint8_t pdu[24];
    size_t plen = 0;

    /* A variante de 1 byte é exatamente a clássica: as duas conferem o que a outra produz. */
    uint64_t tx = 0, rx = 0;
    size_t n = ioc_secpdu_protect_ex(key, 0x304, CMD, 8, &tx, 1, 3, pdu, sizeof(pdu));
    CHECK(n == 8 + 1 + 3);
    CHECK(ioc_secpdu_verify(key, 0x304, pdu, n, &rx, 3, &plen) && plen == 8 && rx == 1);

    /* Dois bytes: a janela passa de 255 para 65535 e o valor sai com o byte mais significativo primeiro. */
    tx = 0x1233;
    rx = 0;
    n = ioc_secpdu_protect_ex(key, 0x305, CMD, 8, &tx, 2, 5, pdu, sizeof(pdu));
    CHECK(n == 8 + 2 + 5 && tx == 0x1234);
    CHECK(pdu[8] == 0x12 && pdu[9] == 0x34);
    CHECK(ioc_secpdu_verify_ex(key, 0x305, pdu, n, &rx, 2, 5, &plen) && plen == 8 && rx == 0x1234);
    rx = 0x1234;   /* repetição */
    CHECK(!ioc_secpdu_verify_ex(key, 0x305, pdu, n, &rx, 2, 5, &plen));
    rx = 0;
    tx = 0x10000;   /* 65536 à frente de quem recebe: fora da janela */
    n = ioc_secpdu_protect_ex(key, 0x305, CMD, 8, &tx, 2, 5, pdu, sizeof(pdu));
    CHECK(!ioc_secpdu_verify_ex(key, 0x305, pdu, n, &rx, 2, 5, &plen) && rx == 0);
    tx = 0xFFFE;   /* 65535 à frente: ainda cabe */
    n = ioc_secpdu_protect_ex(key, 0x305, CMD, 8, &tx, 2, 5, pdu, sizeof(pdu));
    CHECK(ioc_secpdu_verify_ex(key, 0x305, pdu, n, &rx, 2, 5, &plen) && rx == 0xFFFF);

    /* Parâmetros fora de faixa. */
    tx = 0;
    CHECK(ioc_secpdu_protect_ex(key, 0x305, CMD, 8, &tx, 0, 5, pdu, sizeof(pdu)) == 0);
    CHECK(ioc_secpdu_protect_ex(key, 0x305, CMD, 8, &tx, IOC_SECPDU_FRESHNESS_MAX + 1, 5, pdu, sizeof(pdu)) == 0);
    CHECK(ioc_secpdu_protect_ex(key, 0x305, CMD, 8, &tx, 2, 0, pdu, sizeof(pdu)) == 0);
    CHECK(ioc_secpdu_protect_ex(key, 0x305, CMD, 8, &tx, 2, 5, pdu, 14) == 0);   /* não cabe */
    CHECK(tx == 0);
    CHECK(!ioc_secpdu_verify_ex(key, 0x305, pdu, 6, &rx, 2, 5, &plen));   /* curto demais */
}

static void test_which_frames_are_protected(void) {
    CHECK(ioc_ca_kind_of(0x304) == 0);
    CHECK(ioc_ca_kind_of(0x305) == 1);
    CHECK(ioc_ca_kind_of(IOC_CA_TAG_ID) < 0);
    CHECK(ioc_ca_kind_of(0x080) < 0);   /* o controle tem vários emissores: fora deste desenho */
    CHECK(ioc_ca_kind_of(0x306) < 0);
    CHECK(IOC_CA_TAG_ID == 0x307u && IOC_CA_TAG_LEN == 8u);
}

static void test_a_legit_command_is_released_with_the_original_bytes(void) {
    uint8_t key[IOC_LINK_KEY_LEN];
    bus_key(key);
    ioc_ca_tx tx;
    ioc_ca_rx rx;
    ioc_ca_tx_init(&tx, key, 0);
    ioc_ca_rx_init(&rx, key, 0);
    for (int kind = 0; kind < 2; ++kind) {
        const uint16_t id = kind ? IOC_CA_ID_TEST_CMD : IOC_CA_ID_DATA_CMD;
        ioc_ca_frame out;
        memset(&out, 0, sizeof(out));
        CHECK(send_legit(&tx, &rx, 1000, id, CMD, &out) == IOC_CA_RELEASE);
        CHECK(out.id == id && out.len == 8 && memcmp(out.data, CMD, 8) == 0);
    }
    CHECK(rx.accepted == 2 && rx.bad_tag == 0 && rx.orphan_tag == 0 && rx.no_tag == 0);
    CHECK(rx.freshness == 2 && tx.freshness == 2);
    CHECK(!rx.pending[0].valid && !rx.pending[1].valid);
}

static void test_other_identifiers_pass_through(void) {
    uint8_t key[IOC_LINK_KEY_LEN];
    bus_key(key);
    ioc_ca_rx rx;
    ioc_ca_rx_init(&rx, key, 0);
    const uint8_t any[4] = {0x22, 0x10, 0x01, 0x11};
    ioc_ca_why why = IOC_CA_WHY_BAD_KIND;
    CHECK(ioc_ca_rx_frame(&rx, 0, 0x080, any, 4, NULL, &why) == IOC_CA_PASS && why == IOC_CA_WHY_NONE);
    CHECK(ioc_ca_rx_frame(&rx, 0, 0x381, any, 4, NULL, &why) == IOC_CA_PASS);
    CHECK(ioc_ca_rx_frame(&rx, 0, 0x306, any, 4, NULL, NULL) == IOC_CA_PASS);
}

static void test_a_forged_command_without_a_tag_is_never_released(void) {
    uint8_t key[IOC_LINK_KEY_LEN];
    bus_key(key);
    ioc_ca_rx rx;
    ioc_ca_rx_init(&rx, key, 0);
    ioc_ca_why why;
    CHECK(ioc_ca_rx_frame(&rx, 100, 0x305, CMD, 8, NULL, &why) == IOC_CA_HOLD);
    CHECK(ioc_ca_rx_expire(&rx, 100 + IOC_CA_TAG_WAIT_MS) == 0);   /* ainda dentro do prazo */
    CHECK(ioc_ca_rx_expire(&rx, 100 + IOC_CA_TAG_WAIT_MS + 1) == 0x305);
    CHECK(rx.no_tag == 1 && !rx.pending[1].valid);
    CHECK(ioc_ca_rx_expire(&rx, 100000) == 0);   /* nada mais pendente */
    CHECK(rx.accepted == 0);
}

static void test_a_forged_tag_is_refused_and_the_real_one_still_works(void) {
    uint8_t key[IOC_LINK_KEY_LEN];
    bus_key(key);
    ioc_ca_tx tx;
    ioc_ca_rx rx;
    ioc_ca_tx_init(&tx, key, 0);
    ioc_ca_rx_init(&rx, key, 0);
    uint8_t tag[IOC_CA_TAG_LEN];
    ioc_ca_why why;
    ioc_ca_frame out;
    CHECK(ioc_ca_tx_make_tag(&tx, 0x305, CMD, 8, tag));

    CHECK(ioc_ca_rx_frame(&rx, 0, 0x305, CMD, 8, NULL, &why) == IOC_CA_HOLD);
    for (size_t i = 1; i < IOC_CA_TAG_LEN; ++i) {   /* qualquer byte trocado invalida */
        uint8_t bad[IOC_CA_TAG_LEN];
        memcpy(bad, tag, sizeof(bad));
        bad[i] ^= 0x01;
        CHECK(ioc_ca_rx_frame(&rx, 10, IOC_CA_TAG_ID, bad, IOC_CA_TAG_LEN, &out, &why) == IOC_CA_DROP);
        CHECK(why == IOC_CA_WHY_BAD_TAG);
    }
    CHECK(rx.bad_tag == IOC_CA_TAG_LEN - 1 && rx.accepted == 0 && rx.freshness == 0);
    /* Uma etiqueta falsa não derruba o comando verdadeiro que ainda está para chegar. */
    CHECK(ioc_ca_rx_frame(&rx, 20, IOC_CA_TAG_ID, tag, IOC_CA_TAG_LEN, &out, &why) == IOC_CA_RELEASE);
    CHECK(out.id == 0x305 && memcmp(out.data, CMD, 8) == 0 && rx.accepted == 1);
}

static void test_a_tag_cannot_move_to_another_command(void) {
    uint8_t key[IOC_LINK_KEY_LEN];
    bus_key(key);
    ioc_ca_tx tx;
    ioc_ca_rx rx;
    ioc_ca_tx_init(&tx, key, 0);
    ioc_ca_rx_init(&rx, key, 0);
    uint8_t tag[IOC_CA_TAG_LEN];
    ioc_ca_why why;
    ioc_ca_frame out;
    CHECK(ioc_ca_tx_make_tag(&tx, 0x305, CMD, 8, tag));

    /* Mesmo identificador, outro conteúdo (o atacante troca o comando e mantém a etiqueta que viu). */
    uint8_t other[8];
    memcpy(other, CMD, 8);
    other[2] = 0x0F;   /* de DROP_SESSION para CANCEL, por exemplo */
    CHECK(ioc_ca_rx_frame(&rx, 0, 0x305, other, 8, NULL, &why) == IOC_CA_HOLD);
    CHECK(ioc_ca_rx_frame(&rx, 1, IOC_CA_TAG_ID, tag, IOC_CA_TAG_LEN, &out, &why) == IOC_CA_DROP);
    CHECK(why == IOC_CA_WHY_BAD_TAG && rx.accepted == 0);

    /* Outro identificador, mesmo conteúdo: a etiqueta de 0x305 não vale para 0x304. */
    ioc_ca_rx_init(&rx, key, 0);
    CHECK(ioc_ca_rx_frame(&rx, 0, 0x304, CMD, 8, NULL, &why) == IOC_CA_HOLD);
    uint8_t as_kind0[IOC_CA_TAG_LEN];
    memcpy(as_kind0, tag, sizeof(as_kind0));
    as_kind0[0] = 0;
    CHECK(ioc_ca_rx_frame(&rx, 1, IOC_CA_TAG_ID, as_kind0, IOC_CA_TAG_LEN, &out, &why) == IOC_CA_DROP);
    CHECK(why == IOC_CA_WHY_BAD_TAG && rx.accepted == 0);
}

static void test_a_recorded_command_and_tag_cannot_be_replayed(void) {
    uint8_t key[IOC_LINK_KEY_LEN];
    bus_key(key);
    ioc_ca_tx tx;
    ioc_ca_rx rx;
    ioc_ca_tx_init(&tx, key, 0);
    ioc_ca_rx_init(&rx, key, 0);
    uint8_t tag[IOC_CA_TAG_LEN];
    ioc_ca_why why;
    ioc_ca_frame out;

    CHECK(ioc_ca_tx_make_tag(&tx, 0x305, CMD, 8, tag));   /* o que um atacante gravaria no cabo */
    CHECK(ioc_ca_rx_frame(&rx, 0, 0x305, CMD, 8, NULL, &why) == IOC_CA_HOLD);
    CHECK(ioc_ca_rx_frame(&rx, 1, IOC_CA_TAG_ID, tag, IOC_CA_TAG_LEN, &out, &why) == IOC_CA_RELEASE);
    for (int i = 0; i < 5; ++i) {   /* repetida, igualzinha: nunca mais */
        CHECK(ioc_ca_rx_frame(&rx, 1000 + 10 * i, 0x305, CMD, 8, NULL, &why) == IOC_CA_HOLD);
        CHECK(ioc_ca_rx_frame(&rx, 1002 + 10 * i, IOC_CA_TAG_ID, tag, IOC_CA_TAG_LEN, &out, &why) == IOC_CA_DROP);
        CHECK(why == IOC_CA_WHY_BAD_TAG);
    }
    CHECK(rx.accepted == 1 && rx.bad_tag == 5);

    /* E um comando novo e legítimo continua passando depois do ataque. */
    CHECK(send_legit(&tx, &rx, 5000, 0x305, CMD, &out) == IOC_CA_RELEASE);
    CHECK(rx.accepted == 2);
}

static void test_orphan_tags_late_tags_and_replaced_commands(void) {
    uint8_t key[IOC_LINK_KEY_LEN];
    bus_key(key);
    ioc_ca_tx tx;
    ioc_ca_rx rx;
    ioc_ca_tx_init(&tx, key, 0);
    ioc_ca_rx_init(&rx, key, 0);
    uint8_t tag[IOC_CA_TAG_LEN];
    ioc_ca_why why;
    ioc_ca_frame out;
    CHECK(ioc_ca_tx_make_tag(&tx, 0x304, CMD, 8, tag));

    CHECK(ioc_ca_rx_frame(&rx, 0, IOC_CA_TAG_ID, tag, IOC_CA_TAG_LEN, &out, &why) == IOC_CA_DROP);
    CHECK(why == IOC_CA_WHY_ORPHAN_TAG && rx.orphan_tag == 1);

    /* Etiqueta que chega depois do prazo vale como etiqueta sem comando (e o comando conta como sem etiqueta). */
    CHECK(ioc_ca_rx_frame(&rx, 1000, 0x304, CMD, 8, NULL, &why) == IOC_CA_HOLD);
    CHECK(ioc_ca_rx_frame(&rx, 1000 + IOC_CA_TAG_WAIT_MS + 1, IOC_CA_TAG_ID, tag, IOC_CA_TAG_LEN, &out, &why) ==
          IOC_CA_DROP);
    CHECK(why == IOC_CA_WHY_ORPHAN_TAG && rx.no_tag == 1 && rx.orphan_tag == 2 && rx.accepted == 0);

    /* Um comando que toma o lugar de outro ainda sem etiqueta avisa, mas fica guardado. */
    CHECK(ioc_ca_rx_frame(&rx, 2000, 0x304, CMD, 8, NULL, &why) == IOC_CA_HOLD && why == IOC_CA_WHY_NONE);
    CHECK(ioc_ca_rx_frame(&rx, 2010, 0x304, CMD, 8, NULL, &why) == IOC_CA_HOLD && why == IOC_CA_WHY_REPLACED);

    /* Tamanho e tipo inválidos. */
    CHECK(ioc_ca_rx_frame(&rx, 3000, 0x304, CMD, 7, NULL, &why) == IOC_CA_DROP && why == IOC_CA_WHY_BAD_LENGTH);
    CHECK(ioc_ca_rx_frame(&rx, 3000, 0x305, CMD, 0, NULL, &why) == IOC_CA_DROP && why == IOC_CA_WHY_BAD_LENGTH);
    CHECK(ioc_ca_rx_frame(&rx, 3000, IOC_CA_TAG_ID, tag, 7, NULL, &why) == IOC_CA_DROP && why == IOC_CA_WHY_BAD_LENGTH);
    uint8_t badkind[IOC_CA_TAG_LEN];
    memcpy(badkind, tag, sizeof(badkind));
    badkind[0] = 2;
    CHECK(ioc_ca_rx_frame(&rx, 3000, IOC_CA_TAG_ID, badkind, IOC_CA_TAG_LEN, NULL, &why) == IOC_CA_DROP &&
          why == IOC_CA_WHY_BAD_KIND);
    badkind[0] = 0xFF;
    CHECK(ioc_ca_rx_frame(&rx, 3000, IOC_CA_TAG_ID, badkind, IOC_CA_TAG_LEN, NULL, &why) == IOC_CA_DROP &&
          why == IOC_CA_WHY_BAD_KIND);
    CHECK(ioc_ca_why_name(IOC_CA_WHY_BAD_TAG)[0] != 0 && ioc_ca_why_name(IOC_CA_WHY_NONE)[0] != 0);
}

static void test_another_key_never_verifies(void) {
    uint8_t key[IOC_LINK_KEY_LEN], other[IOC_LINK_KEY_LEN];
    bus_key(key);
    memcpy(other, key, sizeof(other));
    other[7] ^= 0x80;
    ioc_ca_tx tx;
    ioc_ca_rx rx;
    ioc_ca_tx_init(&tx, other, 0);   /* o atacante não tem a chave do barramento */
    ioc_ca_rx_init(&rx, key, 0);
    ioc_ca_frame out;
    CHECK(send_legit(&tx, &rx, 0, 0x305, CMD, &out) == IOC_CA_DROP);
    CHECK(rx.accepted == 0 && rx.bad_tag == 1);
}

static void test_the_low_bytes_wrap_around_for_a_long_run(void) {
    uint8_t key[IOC_LINK_KEY_LEN];
    bus_key(key);
    ioc_ca_tx tx;
    ioc_ca_rx rx;
    ioc_ca_tx_init(&tx, key, 0);
    ioc_ca_rx_init(&rx, key, 0);
    int bad = 0;
    for (uint32_t i = 1; i <= 200000; ++i) {   /* passa por mais de três voltas dos 16 bits */
        ioc_ca_frame out;
        uint8_t cmd[8];
        memcpy(cmd, CMD, 8);
        cmd[0] = (uint8_t)i;
        if (send_legit(&tx, &rx, i * 10u, (i & 1u) ? 0x304 : 0x305, cmd, &out) != IOC_CA_RELEASE) ++bad;
    }
    CHECK(bad == 0);
    CHECK(rx.accepted == 200000 && rx.freshness == 200000 && tx.freshness == 200000);
}

static void test_commands_lost_on_the_bus_do_not_break_the_sequence(void) {
    uint8_t key[IOC_LINK_KEY_LEN];
    bus_key(key);
    ioc_ca_tx tx;
    ioc_ca_rx rx;
    ioc_ca_tx_init(&tx, key, 0);
    ioc_ca_rx_init(&rx, key, 0);
    uint8_t tag[IOC_CA_TAG_LEN];
    for (int i = 0; i < 1000; ++i) CHECK(ioc_ca_tx_make_tag(&tx, 0x305, CMD, 8, tag));   /* ninguém ouviu */
    ioc_ca_frame out;
    CHECK(send_legit(&tx, &rx, 0, 0x305, CMD, &out) == IOC_CA_RELEASE);   /* o módulo acompanha o salto */
    CHECK(rx.freshness == 1001);
    for (int i = 0; i < 65534; ++i) CHECK(ioc_ca_tx_make_tag(&tx, 0x305, CMD, 8, tag));
    CHECK(send_legit(&tx, &rx, 0, 0x305, CMD, &out) == IOC_CA_RELEASE);   /* 65535 à frente: no limite */
    for (int i = 0; i < 65535; ++i) CHECK(ioc_ca_tx_make_tag(&tx, 0x305, CMD, 8, tag));
    CHECK(send_legit(&tx, &rx, 0, 0x305, CMD, &out) == IOC_CA_DROP);   /* 65536 à frente: precisa de CANAUTH SYNC */
}

/* NVS simulada: o que a Probe e o módulo gravaram. */
static void test_the_probe_never_reuses_a_value_across_restarts(void) {
    uint8_t key[IOC_LINK_KEY_LEN];
    bus_key(key);
    uint64_t nvs = 0;
    uint64_t highest = 0;
    uint32_t writes = 0;
    ioc_ca_tx tx;
    uint8_t tag[IOC_CA_TAG_LEN];
    unsigned seed = 12345;
    for (int life = 0; life < 300; ++life) {
        ioc_ca_tx_init(&tx, key, nvs);
        nvs = ioc_ca_tx_boot_ceiling(&tx);
        ++writes;
        seed = seed * 1103515245u + 12345u;
        const unsigned commands = (seed >> 16) % 100u;   /* a vida desta partida: 0 a 99 comandos */
        for (unsigned c = 0; c < commands; ++c) {
            if (ioc_ca_tx_needs_save(&tx, nvs)) {   /* grava ANTES de usar o valor */
                nvs = ioc_ca_tx_next_ceiling(&tx);
                ++writes;
            }
            CHECK(ioc_ca_tx_make_tag(&tx, 0x304, CMD, 8, tag));
            CHECK(tx.freshness > highest);   /* nunca repete nem volta */
            CHECK(tx.freshness <= nvs);      /* e nunca passou do que está gravado */
            highest = tx.freshness;
        }   /* queda de energia: nada é gravado na saída */
    }
    CHECK(highest > 5000);
    /* Uma gravação por partida e uma a cada 32 comandos: bem menos que uma por comando. */
    CHECK(writes < 300u + highest / IOC_CA_TX_SAVE_STEP + 300u);
}

static void test_a_module_restart_leaves_a_small_replay_residue(void) {
    uint8_t key[IOC_LINK_KEY_LEN];
    bus_key(key);
    ioc_ca_tx tx;
    ioc_ca_rx rx;
    ioc_ca_tx_init(&tx, key, 0);
    ioc_ca_rx_init(&rx, key, 0);
    uint64_t nvs = 0;
    uint8_t recorded_cmd[40][8];
    uint8_t recorded_tag[40][IOC_CA_TAG_LEN];
    ioc_ca_frame out;
    ioc_ca_why why;
    int n = 0;
    for (; n < 21; ++n) {   /* 21 comandos aceitos; o módulo grava a cada 8 */
        memcpy(recorded_cmd[n], CMD, 8);
        recorded_cmd[n][0] = (uint8_t)n;
        CHECK(ioc_ca_tx_make_tag(&tx, 0x305, recorded_cmd[n], 8, recorded_tag[n]));
        CHECK(ioc_ca_rx_frame(&rx, 0, 0x305, recorded_cmd[n], 8, NULL, &why) == IOC_CA_HOLD);
        CHECK(ioc_ca_rx_frame(&rx, 1, IOC_CA_TAG_ID, recorded_tag[n], IOC_CA_TAG_LEN, &out, &why) == IOC_CA_RELEASE);
        if (ioc_ca_rx_needs_save(&rx, nvs)) nvs = rx.freshness;
    }
    CHECK(rx.freshness == 21 && nvs == 16);

    ioc_ca_rx_init(&rx, key, nvs);   /* o módulo reinicia e volta ao valor gravado */
    int replayed = 0;
    for (int i = 0; i < n; ++i) {   /* o atacante repete tudo o que gravou, na ordem */
        if (ioc_ca_rx_frame(&rx, 10, 0x305, recorded_cmd[i], 8, NULL, &why) != IOC_CA_HOLD) continue;
        if (ioc_ca_rx_frame(&rx, 11, IOC_CA_TAG_ID, recorded_tag[i], IOC_CA_TAG_LEN, &out, &why) == IOC_CA_RELEASE) {
            ++replayed;
        }
    }
    CHECK(replayed == 5);                       /* os comandos 17 a 21, aceitos depois da última gravação */
    CHECK(replayed < (int)IOC_CA_RX_SAVE_STEP);   /* o resíduo documentado */
    /* Depois disso, nada mais do que foi gravado vale, e o próximo comando verdadeiro passa. */
    CHECK(send_legit(&tx, &rx, 100, 0x305, CMD, &out) == IOC_CA_RELEASE);
}

int main(void) {
    test_the_secpdu_variant_with_other_freshness_lengths();
    test_which_frames_are_protected();
    test_a_legit_command_is_released_with_the_original_bytes();
    test_other_identifiers_pass_through();
    test_a_forged_command_without_a_tag_is_never_released();
    test_a_forged_tag_is_refused_and_the_real_one_still_works();
    test_a_tag_cannot_move_to_another_command();
    test_a_recorded_command_and_tag_cannot_be_replayed();
    test_orphan_tags_late_tags_and_replaced_commands();
    test_another_key_never_verifies();
    test_the_low_bytes_wrap_around_for_a_long_run();
    test_commands_lost_on_the_bus_do_not_break_the_sequence();
    test_the_probe_never_reuses_a_value_across_restarts();
    test_a_module_restart_leaves_a_small_replay_residue();
    if (failures) {
        printf("test_ioc_canauth: %d falha(s)\n", failures);
        return 1;
    }
    puts("test_ioc_canauth: ok");
    return 0;
}
