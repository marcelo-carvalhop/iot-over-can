/**
 * @file ioc_panel_cache.h
 * @brief Lado do Node: retrato do estado e caixa postal para o painel.
 *
 * Todo Node recebe todos os quadros do barramento (não há filtros de
 * aceitação no MCP2515). O cache guarda, sem interpretar o conteúdo:
 *
 *  - retrato: o último quadro de cada chave de estado (sinal de presença do
 *    líder, estado replicado de cada Node, vínculo de cada filho, observações
 *    BLE) e a última transferência completa de telemetria, enlace,
 *    configuração e relatório de painel de cada filho;
 *  - caixa postal: quadros de evento (eleição, entrada de Node, comandos de
 *    associação, ensaios, DTC, mudança de vínculo) recebidos enquanto ela está
 *    aberta, isto é, enquanto o painel dorme.
 *
 * O retrato e a caixa postal são entregues como quadros CAN; o painel não
 * distingue um quadro do retrato de um quadro ao vivo, a não ser pela idade.
 */
#ifndef IOC_PANEL_CACHE_H
#define IOC_PANEL_CACHE_H

#include <stddef.h>
#include <stdint.h>

#include "ioc_panel_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IOC_PCACHE_STATE_SLOTS  96
#define IOC_PCACHE_XFER_SLOTS   16
#define IOC_PCACHE_XFER_FRAMES  20
#define IOC_PCACHE_MAILBOX      96

typedef enum {
    IOC_PCLASS_DROP  = 0,   /* nem guardado nem encaminhado (carga artificial, espectro) */
    IOC_PCLASS_LIVE  = 1,   /* só ao vivo */
    IOC_PCLASS_STATE = 2,   /* retrato */
    IOC_PCLASS_EVENT = 3,   /* caixa postal */
    IOC_PCLASS_BOTH  = 4,   /* retrato e caixa postal */
    IOC_PCLASS_XFER  = 5    /* segmento de transferência do plano de dados */
} ioc_pclass;

typedef struct {
    uint8_t  used;
    uint8_t  own;
    uint8_t  len;
    uint8_t  key;
    uint16_t id;
    uint32_t ms;
    uint8_t  data[8];
} ioc_pcache_entry;

typedef struct {
    uint8_t  used;
    uint8_t  child;
    uint8_t  kind;
    uint8_t  own;
    uint16_t id;
    uint8_t  building;     /* número de segmentos acumulados da transferência em curso */
    uint8_t  expected;
    uint8_t  transfer;
    uint8_t  committed;    /* segmentos da última transferência completa */
    uint32_t committed_ms;
    uint32_t touched_ms;
    uint8_t  frames[IOC_PCACHE_XFER_FRAMES][8];
    uint8_t  done[IOC_PCACHE_XFER_FRAMES][8];
} ioc_pcache_xfer;

typedef struct {
    ioc_pcache_entry state[IOC_PCACHE_STATE_SLOTS];
    ioc_pcache_xfer  xfer[IOC_PCACHE_XFER_SLOTS];
    ioc_pcache_entry mailbox[IOC_PCACHE_MAILBOX];
    uint8_t  mb_head;
    uint8_t  mb_count;
    uint8_t  mailbox_open;
    uint16_t mb_dropped;
    uint32_t frames_seen;
} ioc_pcache;

typedef void (*ioc_pcache_emit_fn)(void *ctx, const ioc_panel_frame *frame);

/* Classe de um quadro e, para quadros de estado, a chave dentro do identificador. */
ioc_pclass ioc_pcache_classify(uint16_t id, const uint8_t *data, uint8_t len, uint8_t *key);

void ioc_pcache_init(ioc_pcache *cache);
void ioc_pcache_on_frame(ioc_pcache *cache, uint16_t id, const uint8_t *data, uint8_t len,
                         uint8_t own, uint32_t now_ms);
/* Emite o retrato: estados e transferências completas, do mais antigo ao mais novo. */
void ioc_pcache_snapshot(const ioc_pcache *cache, uint32_t now_ms, ioc_pcache_emit_fn emit, void *ctx);
/* Abre ou fecha a caixa postal; ao abrir, ela começa vazia. */
void ioc_pcache_mailbox_open(ioc_pcache *cache, int open);
/* Emite e esvazia a caixa postal. */
void ioc_pcache_mailbox_drain(ioc_pcache *cache, uint32_t now_ms, ioc_pcache_emit_fn emit, void *ctx);

/* 1 se o quadro deve seguir ao vivo para o painel. */
int ioc_pcache_forward_live(uint16_t id, const uint8_t *data, uint8_t len);

#ifdef __cplusplus
}
#endif

#endif
