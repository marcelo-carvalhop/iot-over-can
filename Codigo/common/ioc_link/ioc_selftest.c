#include "ioc_selftest.h"

#include <string.h>

/* Bits do registrador EFLG do MCP2515. */
#define EFLG_EWARN  0x01u
#define EFLG_RXWAR  0x02u
#define EFLG_TXWAR  0x04u
#define EFLG_RXEP   0x08u
#define EFLG_TXEP   0x10u
#define EFLG_TXBO   0x20u
#define EFLG_RX0OVR 0x40u
#define EFLG_RX1OVR 0x80u

/* O controlador acende o aviso de erro em 96 (um contador passou de 95). */
#define COUNTER_WARN 96u

static const ioc_st_frame FRAMES[IOC_ST_FRAMES] = {
    {0x555u, 8, {0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA}},
    {0x2AAu, 8, {0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55}},
    {0x7F0u, 8, {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80}},
    {0x001u, 8, {0xFE, 0xFD, 0xFB, 0xF7, 0xEF, 0xDF, 0xBF, 0x7F}},
};

int ioc_st_frame_at(uint8_t n, ioc_st_frame *out) {
    if (n >= IOC_ST_FRAMES || out == NULL) return 0;
    *out = FRAMES[n];
    return 1;
}

ioc_st_lb_result ioc_st_loopback_compare(const ioc_st_frame *sent, const ioc_st_frame *got) {
    if (got == NULL) return IOC_ST_LB_NO_FRAME;
    if (sent == NULL) return IOC_ST_LB_NO_FRAME;
    if (got->id != sent->id) return IOC_ST_LB_ID;
    if (got->len != sent->len) return IOC_ST_LB_LEN;
    if (memcmp(got->data, sent->data, sent->len) != 0) return IOC_ST_LB_DATA;
    return IOC_ST_LB_OK;
}

const char *ioc_st_lb_text(ioc_st_lb_result r) {
    switch (r) {
        case IOC_ST_LB_OK:       return "OK";
        case IOC_ST_LB_NO_FRAME: return "SEM_QUADRO";
        case IOC_ST_LB_ID:       return "ID";
        case IOC_ST_LB_LEN:      return "TAMANHO";
        case IOC_ST_LB_DATA:     return "DADOS";
    }
    return "DESCONHECIDO";
}

const char *ioc_st_lb_hint(ioc_st_lb_result r) {
    switch (r) {
        case IOC_ST_LB_OK:
            return "";
        case IOC_ST_LB_NO_FRAME:
            return "o controlador respondeu pelo SPI mas nenhum quadro voltou: confira o fio INT do MCP2515";
        case IOC_ST_LB_ID:
        case IOC_ST_LB_LEN:
        case IOC_ST_LB_DATA:
            return "o quadro voltou diferente do enviado: confira SCK, MOSI e MISO (fios curtos, contato, alimentacao do modulo)";
    }
    return "";
}

ioc_st_level ioc_st_classify_errors(uint8_t rec, uint8_t tec, uint8_t eflg, const char **reason) {
    ioc_st_level level = IOC_ST_PASS;
    const char *why = "OK";

    /* Do mais grave ao menos grave; vale o primeiro que se aplica. */
    if (eflg & EFLG_TXBO) {
        level = IOC_ST_FAIL;
        why = "BUS_OFF";
    } else if (eflg & (EFLG_TXEP | EFLG_RXEP)) {
        level = IOC_ST_WARN;
        why = "ERRO_PASSIVO";
    } else if (rec >= COUNTER_WARN || tec >= COUNTER_WARN) {
        level = IOC_ST_WARN;
        why = "CONTADOR_ALTO";
    } else if (eflg & (EFLG_EWARN | EFLG_RXWAR | EFLG_TXWAR)) {
        level = IOC_ST_WARN;
        why = "AVISO_DE_ERRO";
    } else if (eflg & (EFLG_RX0OVR | EFLG_RX1OVR)) {
        level = IOC_ST_WARN;
        why = "ESTOURO_DE_RECEPCAO";
    }
    if (reason) *reason = why;
    return level;
}

const char *ioc_st_error_hint(const char *reason) {
    if (reason == NULL || strcmp(reason, "OK") == 0) return "";
    if (strcmp(reason, "BUS_OFF") == 0) {
        return "o controlador se desligou do barramento por excesso de erros: confira terminacao de 120 ohms nas duas pontas, CANH/CANL e se ha outro modulo ligado";
    }
    if (strcmp(reason, "ERRO_PASSIVO") == 0 || strcmp(reason, "CONTADOR_ALTO") == 0 ||
        strcmp(reason, "AVISO_DE_ERRO") == 0) {
        return "o controlador viu muitos erros no barramento: confira terminacao, fiacao CANH/CANL e se todos os modulos usam 500 kbit/s";
    }
    if (strcmp(reason, "ESTOURO_DE_RECEPCAO") == 0) {
        return "o controlador perdeu quadros por falta de leitura; o registro fica ate o proximo reinicio";
    }
    return "";
}

const char *ioc_st_level_name(ioc_st_level level) {
    switch (level) {
        case IOC_ST_PASS: return "PASS";
        case IOC_ST_WARN: return "WARN";
        case IOC_ST_FAIL: return "FAIL";
    }
    return "FAIL";
}

ioc_st_level ioc_st_worst(ioc_st_level a, ioc_st_level b) {
    return a > b ? a : b;
}
