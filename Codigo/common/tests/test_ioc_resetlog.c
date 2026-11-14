/*
 * Testes nativos: motivo e contagem dos reinícios (ioc_resetlog).
 */
#include <stdio.h>
#include <string.h>

#include "ioc_resetlog.h"

static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FALHA %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

static void test_classify(void) {
    /* Só queda de tensão, watchdog e falha do programa são reinícios anormais. */
    CHECK(ioc_rl_classify(IOC_RST_BROWNOUT) == IOC_RL_BROWNOUT);
    CHECK(ioc_rl_classify(IOC_RST_INT_WDT) == IOC_RL_WATCHDOG);
    CHECK(ioc_rl_classify(IOC_RST_TASK_WDT) == IOC_RL_WATCHDOG);
    CHECK(ioc_rl_classify(IOC_RST_WDT) == IOC_RL_WATCHDOG);
    CHECK(ioc_rl_classify(IOC_RST_PANIC) == IOC_RL_PANIC);
    /* Ligar a placa, apertar EN (ou abrir a porta serial) e esp_restart são partidas normais. */
    CHECK(ioc_rl_classify(IOC_RST_POWERON) == IOC_RL_NORMAL);
    CHECK(ioc_rl_classify(IOC_RST_EXT) == IOC_RL_NORMAL);
    CHECK(ioc_rl_classify(IOC_RST_SW) == IOC_RL_NORMAL);
    CHECK(ioc_rl_classify(IOC_RST_DEEPSLEEP) == IOC_RL_NORMAL);
    CHECK(ioc_rl_classify(IOC_RST_UNKNOWN) == IOC_RL_NORMAL);
    CHECK(ioc_rl_classify(200) == IOC_RL_NORMAL);

    CHECK(strcmp(ioc_rl_reason_name(IOC_RST_BROWNOUT), "QUEDA_DE_TENSAO") == 0);
    CHECK(strcmp(ioc_rl_reason_name(IOC_RST_POWERON), "ENERGIA_LIGADA") == 0);
    CHECK(strcmp(ioc_rl_reason_name(200), "DESCONHECIDO") == 0);
    for (int reason = 0; reason < 256; ++reason) {
        const char *name = ioc_rl_reason_name((uint8_t)reason);
        CHECK(name != NULL && name[0] != '\0');
        for (const char *p = name; *p; ++p) CHECK(*p >= ' ' && *p <= '~');   /* ASCII puro */
    }
}

static void test_note(void) {
    ioc_rl_counters c;
    memset(&c, 0, sizeof(c));

    CHECK(ioc_rl_note(&c, IOC_RST_POWERON) == 0);   /* nada a gravar */
    CHECK(ioc_rl_note(&c, IOC_RST_EXT) == 0);
    CHECK(c.brownout == 0 && c.watchdog == 0 && c.panic == 0 && c.last == 0);

    CHECK(ioc_rl_note(&c, IOC_RST_BROWNOUT) == 1);
    CHECK(ioc_rl_note(&c, IOC_RST_BROWNOUT) == 1);
    CHECK(ioc_rl_note(&c, IOC_RST_TASK_WDT) == 1);
    CHECK(ioc_rl_note(&c, IOC_RST_PANIC) == 1);
    CHECK(c.brownout == 2 && c.watchdog == 1 && c.panic == 1);
    CHECK(c.last == IOC_RST_PANIC);

    /* Uma partida normal depois de um reinício anormal não apaga o histórico. */
    CHECK(ioc_rl_note(&c, IOC_RST_POWERON) == 0);
    CHECK(c.brownout == 2 && c.last == IOC_RST_PANIC);

    /* O contador satura em vez de dar a volta. */
    c.brownout = UINT32_MAX;
    CHECK(ioc_rl_note(&c, IOC_RST_BROWNOUT) == 1);
    CHECK(c.brownout == UINT32_MAX);

    CHECK(ioc_rl_note(NULL, IOC_RST_BROWNOUT) == 0);
}

static void test_codec(void) {
    ioc_rl_counters c;
    memset(&c, 0, sizeof(c));
    c.brownout = 3;
    c.watchdog = 1;
    c.panic = 0x01020304u;
    c.last = IOC_RST_BROWNOUT;

    uint8_t blob[IOC_RL_BYTES];
    CHECK(ioc_rl_encode(&c, blob, sizeof(blob)) == IOC_RL_BYTES);
    CHECK(ioc_rl_encode(&c, blob, sizeof(blob) - 1) == 0);
    CHECK(ioc_rl_encode(NULL, blob, sizeof(blob)) == 0);

    ioc_rl_counters back;
    CHECK(ioc_rl_decode(blob, sizeof(blob), &back) == 1);
    CHECK(back.brownout == 3 && back.watchdog == 1 && back.panic == 0x01020304u);
    CHECK(back.last == IOC_RST_BROWNOUT);

    /* Qualquer alteração de um byte invalida o registro, e os contadores voltam a zero. */
    for (size_t i = 0; i < sizeof(blob); ++i) {
        uint8_t bad[IOC_RL_BYTES];
        memcpy(bad, blob, sizeof(bad));
        bad[i] ^= 0x40;
        CHECK(ioc_rl_decode(bad, sizeof(bad), &back) == 0);
        CHECK(back.brownout == 0 && back.watchdog == 0 && back.panic == 0 && back.last == 0);
    }
    /* Tamanho errado, vazio e ponteiros nulos. */
    CHECK(ioc_rl_decode(blob, sizeof(blob) - 1, &back) == 0);
    CHECK(ioc_rl_decode(blob, 0, &back) == 0);
    CHECK(ioc_rl_decode(NULL, sizeof(blob), &back) == 0);
    CHECK(ioc_rl_decode(blob, sizeof(blob), NULL) == 0);
}

static void test_format(void) {
    ioc_rl_counters c;
    memset(&c, 0, sizeof(c));
    c.brownout = 3;
    c.watchdog = 1;

    char line[96];
    const size_t n = ioc_rl_format(line, sizeof(line), IOC_RST_BROWNOUT, &c);
    CHECK(n == strlen(line));
    CHECK(strcmp(line, "reinicio=QUEDA_DE_TENSAO quedas_de_tensao=3 watchdog=1 panico=0") == 0);

    ioc_rl_format(line, sizeof(line), IOC_RST_POWERON, NULL);
    CHECK(strcmp(line, "reinicio=ENERGIA_LIGADA quedas_de_tensao=0 watchdog=0 panico=0") == 0);

    /* Buffer pequeno: trunca sem estourar e informa o tamanho que caberia. */
    char small[16];
    CHECK(ioc_rl_format(small, sizeof(small), IOC_RST_BROWNOUT, &c) > sizeof(small) - 1);
    CHECK(strlen(small) == sizeof(small) - 1);
}

int main(void) {
    test_classify();
    test_note();
    test_codec();
    test_format();
    if (g_failures) {
        fprintf(stderr, "ioc_resetlog: %d falha(s)\n", g_failures);
        return 1;
    }
    printf("ioc_resetlog native tests: PASS\n");
    return 0;
}
