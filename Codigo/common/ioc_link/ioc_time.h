/**
 * @file ioc_time.h
 * @brief Relógio da rede: hora UTC num quadro CAN clássico de 8 bytes.
 *
 * Cada transmissor usa o próprio identificador, CAN_ID_TIME_BASE + NODE_ID
 * (can_ids.h): 0x120 é a Probe 00 e 0x121..0x13F são os Nodes 1..31. A faixa
 * perde para eleição, controle e sinal de presença e vence todo o tráfego dos
 * sensores.
 *
 *   byte 0     tipo: 1 hora, 2 pedido de hora
 *   byte 1     fonte: 0 sem hora, 1 terminal, 2 relógio de tempo real (RTC), 3 operador
 *   bytes 2..5 segundos UTC desde 1970 (little-endian)
 *   bytes 6..7 milissegundos, 0..999 (little-endian)
 *
 * No pedido, fonte e hora são as de quem pede (zeros se não tem hora); só
 * responde quem tem uma hora que o solicitante adotaria.
 */
#ifndef IOC_TIME_H
#define IOC_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IOC_TIME_LEN           8
#define IOC_TIME_KIND_TIME     0x01
#define IOC_TIME_KIND_REQUEST  0x02

/* Fontes em ordem de preferência: número menor é hora melhor. */
#define IOC_TIME_SRC_NONE      0
#define IOC_TIME_SRC_TERMINAL  1   /* relógio do computador da estação, pela Probe 00 */
#define IOC_TIME_SRC_RTC       2   /* relógio de tempo real com bateria */
#define IOC_TIME_SRC_OPERATOR  3   /* ajustada à mão num painel */

/* 2024-01-01T00:00:00Z: abaixo disso o relógio não foi ajustado. */
#define IOC_TIME_MIN_EPOCH     1704067200u

typedef struct {
    uint8_t kind;
    uint8_t source;
    int64_t utc_ms;   /* milissegundos UTC desde 1970; 0 quando não há hora */
} ioc_time_msg;

void ioc_time_encode(const ioc_time_msg *in, uint8_t out[IOC_TIME_LEN]);
/* 1 se o quadro é válido: tipo conhecido e, num quadro de hora, fonte e hora válidas. */
int ioc_time_decode(const uint8_t *data, uint8_t len, ioc_time_msg *out);
/*
 * Adotar a hora recebida? Sempre, se não há hora ou se a fonte recebida é
 * melhor; nunca, se é pior; da mesma fonte, quando difere mais que
 * tolerance_ms (corrige a deriva sem oscilar com o atraso da entrega).
 */
int ioc_time_adopt(uint8_t mine_source, int64_t mine_ms, uint8_t rx_source, int64_t rx_ms, uint32_t tolerance_ms);
const char *ioc_time_source_name(uint8_t source);

#ifdef __cplusplus
}
#endif

#endif
