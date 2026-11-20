/* Autoteste do módulo: quadros do laço interno e classificação dos contadores de erro. */
#include <stdio.h>
#include <string.h>

#include "ioc_selftest.h"

static int failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            printf("FALHA %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static void test_frames_cover_every_identifier_and_data_bit(void) {
    uint32_t id_zero_seen = 0, id_one_seen = 0;
    uint8_t data_zero_seen[8] = {0}, data_one_seen[8] = {0};
    for (uint8_t n = 0; n < IOC_ST_FRAMES; ++n) {
        ioc_st_frame frame;
        CHECK(ioc_st_frame_at(n, &frame) == 1);
        CHECK(frame.len == 8);
        CHECK(frame.id <= 0x7FFu);   /* identificador padrão de 11 bits */
        id_one_seen |= frame.id;
        id_zero_seen |= (~frame.id) & 0x7FFu;
        for (int i = 0; i < 8; ++i) {
            data_one_seen[i] |= frame.data[i];
            data_zero_seen[i] |= (uint8_t)~frame.data[i];
        }
    }
    /* Cada bit do identificador e de cada byte aparece em 0 e em 1: um bit preso em qualquer nível é visto. */
    CHECK(id_one_seen == 0x7FFu && id_zero_seen == 0x7FFu);
    for (int i = 0; i < 8; ++i) CHECK(data_one_seen[i] == 0xFF && data_zero_seen[i] == 0xFF);

    ioc_st_frame ignored;
    CHECK(ioc_st_frame_at(IOC_ST_FRAMES, &ignored) == 0);
    CHECK(ioc_st_frame_at(0, NULL) == 0);
}

static void test_the_test_identifiers_are_unused_by_the_protocol(void) {
    /* 0x7EF é a carga de baixa prioridade dos ensaios (can_ids.h); os identificadores do laço não podem coincidir
       com nenhum identificador do projeto, e o único acima de 0x3FF é o 0x7F0, que nunca vai ao barramento. */
    ioc_st_frame frame;
    for (uint8_t n = 0; n < IOC_ST_FRAMES; ++n) {
        CHECK(ioc_st_frame_at(n, &frame) == 1);
        CHECK(frame.id != 0x7EFu);
        CHECK(frame.id != 0x100u && frame.id != 0x200u && frame.id != 0x080u);
    }
}

static void test_loopback_comparison(void) {
    ioc_st_frame sent, got;
    CHECK(ioc_st_frame_at(0, &sent) == 1);
    got = sent;
    CHECK(ioc_st_loopback_compare(&sent, &got) == IOC_ST_LB_OK);
    CHECK(ioc_st_loopback_compare(&sent, NULL) == IOC_ST_LB_NO_FRAME);
    CHECK(ioc_st_loopback_compare(NULL, &got) == IOC_ST_LB_NO_FRAME);

    got = sent;
    got.id ^= 0x001u;
    CHECK(ioc_st_loopback_compare(&sent, &got) == IOC_ST_LB_ID);
    got = sent;
    got.len = 7;
    CHECK(ioc_st_loopback_compare(&sent, &got) == IOC_ST_LB_LEN);
    for (int byte = 0; byte < 8; ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            got = sent;
            got.data[byte] ^= (uint8_t)(1u << bit);
            CHECK(ioc_st_loopback_compare(&sent, &got) == IOC_ST_LB_DATA);
        }
    }

    CHECK(strcmp(ioc_st_lb_text(IOC_ST_LB_OK), "OK") == 0);
    CHECK(strcmp(ioc_st_lb_text(IOC_ST_LB_NO_FRAME), "SEM_QUADRO") == 0);
    CHECK(strcmp(ioc_st_lb_text(IOC_ST_LB_DATA), "DADOS") == 0);
    CHECK(strstr(ioc_st_lb_hint(IOC_ST_LB_NO_FRAME), "INT") != NULL);
    CHECK(strstr(ioc_st_lb_hint(IOC_ST_LB_DATA), "MISO") != NULL);
    CHECK(ioc_st_lb_hint(IOC_ST_LB_OK)[0] == '\0');
    for (int r = IOC_ST_LB_OK; r <= IOC_ST_LB_DATA; ++r) {
        CHECK(strchr(ioc_st_lb_text((ioc_st_lb_result)r), ' ') == NULL);   /* uma palavra só, para a linha KEY=VALOR */
    }
}

static void test_error_counters(void) {
    const char *why = NULL;
    CHECK(ioc_st_classify_errors(0, 0, 0x00, &why) == IOC_ST_PASS && strcmp(why, "OK") == 0);
    CHECK(ioc_st_classify_errors(95, 95, 0x00, &why) == IOC_ST_PASS);              /* abaixo do limiar do controlador */
    CHECK(ioc_st_classify_errors(96, 0, 0x00, &why) == IOC_ST_WARN && strcmp(why, "CONTADOR_ALTO") == 0);
    CHECK(ioc_st_classify_errors(0, 96, 0x00, &why) == IOC_ST_WARN && strcmp(why, "CONTADOR_ALTO") == 0);
    CHECK(ioc_st_classify_errors(0, 0, 0x01, &why) == IOC_ST_WARN && strcmp(why, "AVISO_DE_ERRO") == 0);
    CHECK(ioc_st_classify_errors(0, 0, 0x02, &why) == IOC_ST_WARN && strcmp(why, "AVISO_DE_ERRO") == 0);
    CHECK(ioc_st_classify_errors(0, 0, 0x04, &why) == IOC_ST_WARN && strcmp(why, "AVISO_DE_ERRO") == 0);
    CHECK(ioc_st_classify_errors(0, 0, 0x08, &why) == IOC_ST_WARN && strcmp(why, "ERRO_PASSIVO") == 0);
    CHECK(ioc_st_classify_errors(0, 0, 0x10, &why) == IOC_ST_WARN && strcmp(why, "ERRO_PASSIVO") == 0);
    CHECK(ioc_st_classify_errors(0, 255, 0x20, &why) == IOC_ST_FAIL && strcmp(why, "BUS_OFF") == 0);
    CHECK(ioc_st_classify_errors(0, 0, 0x40, &why) == IOC_ST_WARN && strcmp(why, "ESTOURO_DE_RECEPCAO") == 0);
    CHECK(ioc_st_classify_errors(0, 0, 0x80, &why) == IOC_ST_WARN && strcmp(why, "ESTOURO_DE_RECEPCAO") == 0);
    /* Vários sinais juntos: vale o mais grave. */
    CHECK(ioc_st_classify_errors(200, 255, 0xFF, &why) == IOC_ST_FAIL && strcmp(why, "BUS_OFF") == 0);
    CHECK(ioc_st_classify_errors(0, 0, 0xC1, &why) == IOC_ST_WARN && strcmp(why, "AVISO_DE_ERRO") == 0);
    /* O motivo é opcional. */
    CHECK(ioc_st_classify_errors(0, 0, 0x20, NULL) == IOC_ST_FAIL);

    CHECK(ioc_st_error_hint("OK")[0] == '\0' && ioc_st_error_hint(NULL)[0] == '\0');
    CHECK(strstr(ioc_st_error_hint("BUS_OFF"), "120 ohms") != NULL);
    CHECK(strstr(ioc_st_error_hint("ERRO_PASSIVO"), "500 kbit/s") != NULL);
    CHECK(strstr(ioc_st_error_hint("ESTOURO_DE_RECEPCAO"), "reinicio") != NULL);
}

static void test_levels(void) {
    CHECK(strcmp(ioc_st_level_name(IOC_ST_PASS), "PASS") == 0);
    CHECK(strcmp(ioc_st_level_name(IOC_ST_WARN), "WARN") == 0);
    CHECK(strcmp(ioc_st_level_name(IOC_ST_FAIL), "FAIL") == 0);
    CHECK(ioc_st_worst(IOC_ST_PASS, IOC_ST_WARN) == IOC_ST_WARN);
    CHECK(ioc_st_worst(IOC_ST_FAIL, IOC_ST_WARN) == IOC_ST_FAIL);
    CHECK(ioc_st_worst(IOC_ST_PASS, IOC_ST_PASS) == IOC_ST_PASS);
}

int main(void) {
    test_frames_cover_every_identifier_and_data_bit();
    test_the_test_identifiers_are_unused_by_the_protocol();
    test_loopback_comparison();
    test_error_counters();
    test_levels();
    if (failures) {
        printf("test_ioc_selftest: %d falha(s)\n", failures);
        return 1;
    }
    puts("test_ioc_selftest: ok");
    return 0;
}
