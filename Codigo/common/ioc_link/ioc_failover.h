/**
 * @file ioc_failover.h
 * @brief Reassociação automática de sensores wireless (decisão, sem E/S).
 *
 * Todo Node funcional mantém esta tabela a partir do que ouve no barramento:
 * estados de vínculo publicados pelos Nodes responsáveis e observações BLE
 * (RSSI) de cada Node. Somente o líder em exercício consulta ioc_fo_poll() e
 * age; como a tabela existe em todos, um líder recém-eleito continua de onde
 * o anterior parou.
 *
 * O líder apenas pede a associação ao novo Node. O responsável anterior, se
 * ainda estiver ativo, libera o sensor quando ouve o novo publicar o vínculo
 * (ioc_fo_should_yield). Assim o sensor nunca fica sem responsável porque o
 * destino recusou o pedido ou saiu do ar: o vínculo anterior permanece, e
 * depois do intervalo mínimo o líder tenta outro Node.
 *
 * A reassociação só acontece para vínculos que o operador pré-autorizou
 * (política AUTO, escolhida na associação ou depois). Motivos:
 *
 *   OWNER_LOST   o Node responsável deixou de publicar o vínculo ou foi
 *                marcado como ausente pelo líder;
 *   SENSOR_LOST  o Node responsável está ativo, mas não tem sessão de dados
 *                com o sensor nem ouve os anúncios dele (estado STALE ou
 *                LOST), e outro Node o observa.
 *
 * O novo responsável é o Node com melhor RSSI recente. No caso SENSOR_LOST o
 * responsável atual não concorre. No caso OWNER_LOST ele concorre: um Node
 * que reiniciou perde os vínculos (ficam em RAM), volta a observar o sensor
 * e pode recebê-lo de volta quando nenhum outro Node o alcança melhor.
 */
#ifndef IOC_FAILOVER_H
#define IOC_FAILOVER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Estados de associação (mesmos valores de Codigo/node-can/include/protocolo.h). */
#define IOC_ASSOC_DISCOVERED  0x01
#define IOC_ASSOC_ASSOCIATING 0x02
#define IOC_ASSOC_BOUND       0x03
#define IOC_ASSOC_ONLINE      0x04
#define IOC_ASSOC_STALE       0x05
#define IOC_ASSOC_LOST        0x06
#define IOC_ASSOC_UNBOUND     0x07
#define IOC_ASSOC_REJECTED    0x7F

/*
 * O byte de estado publicado no CAN carrega duas indicações nos bits altos:
 *   bit 7  política de reassociação automática
 *   bit 6  o Node tem sessão de dados autenticada com o sensor
 * REJECTED (0x7F) é sempre enviado sem indicações.
 */
#define IOC_ASSOC_FLAG_AUTO   0x80
#define IOC_ASSOC_FLAG_SECURE 0x40

uint8_t ioc_assoc_pack(uint8_t state, int auto_failover, int secure);
void    ioc_assoc_unpack(uint8_t raw, uint8_t *state, int *auto_failover, int *secure);

#define IOC_FO_MAX_ENTRIES          12
#define IOC_FO_MAX_OBSERVERS        6
/* O responsável republica o vínculo a cada 10 s; duas ausências e meia. */
#define IOC_FO_OWNER_SILENT_MS      25000u
/* Observações BLE são reenviadas a cada 5 s. */
#define IOC_FO_OBSERVATION_AGE_MS   12000u
/* Tempo mínimo entre duas decisões para o mesmo sensor. */
#define IOC_FO_COOLDOWN_MS          30000u
/* Observação do próprio responsável só vale se for esta quantidade mais nova
 * que o último estado dele (mais que um ciclo de republicação). */
#define IOC_FO_OWNER_RETURN_MS      12000u
#define IOC_FO_MIN_RSSI             (-90)
#define IOC_FO_PURGE_MS             600000u

typedef enum {
    IOC_FO_REASON_NONE        = 0,
    IOC_FO_REASON_OWNER_LOST  = 1,
    IOC_FO_REASON_SENSOR_LOST = 2
} ioc_fo_reason;

typedef struct {
    uint8_t  node;
    int8_t   rssi;
    uint32_t seen_ms;
} ioc_fo_observation;

typedef struct {
    uint8_t  used;
    uint64_t uuid;
    uint8_t  owner;
    uint8_t  child;
    uint8_t  state;
    uint8_t  auto_failover;
    uint8_t  secure;
    uint8_t  owner_faulted;
    uint8_t  action_taken;
    uint32_t status_ms;
    uint32_t action_ms;
    ioc_fo_observation obs[IOC_FO_MAX_OBSERVERS];
} ioc_fo_entry;

typedef struct {
    ioc_fo_entry entries[IOC_FO_MAX_ENTRIES];
} ioc_fo_table;

typedef struct {
    uint64_t uuid;
    uint8_t  from_node;
    uint8_t  to_node;
    uint8_t  reason;      /* ioc_fo_reason */
    int8_t   rssi;        /* RSSI observado pelo novo responsável */
} ioc_fo_decision;

void ioc_fo_init(ioc_fo_table *table);

/** Estado de vínculo publicado pelo Node `node` (já sem as indicações). */
void ioc_fo_on_status(ioc_fo_table *table, uint32_t now_ms, uint8_t node, uint8_t child,
                      uint64_t uuid, uint8_t state, int auto_failover, int secure);
/** Observação BLE do sensor `uuid` feita pelo Node `node`. */
void ioc_fo_on_observation(ioc_fo_table *table, uint32_t now_ms, uint8_t node, uint64_t uuid,
                           int8_t rssi);
/**
 * O líder declarou o Node ausente (watchdog de omissão). Os vínculos dele
 * podem ser reassociados de imediato e as observações dele são descartadas.
 */
void ioc_fo_on_node_fault(ioc_fo_table *table, uint8_t node);

/**
 * Este Node ficou sem executar (travamento, ensaio de silêncio) e não ouviu o
 * barramento nesse intervalo: o silêncio dos demais não significa nada. A
 * contagem recomeça e as observações antigas são descartadas, de modo que
 * nenhuma decisão é tomada antes de chegarem informações novas.
 */
void ioc_fo_on_local_stall(ioc_fo_table *table, uint32_t now_ms);

/**
 * Consulta feita pelo líder. Retorna 1 e preenche *out quando há uma
 * reassociação a executar (no máximo uma por chamada).
 */
int ioc_fo_poll(ioc_fo_table *table, uint32_t now_ms, ioc_fo_decision *out);

const ioc_fo_entry *ioc_fo_find(const ioc_fo_table *table, uint64_t uuid);

/**
 * Dois Nodes publicando vínculo para o mesmo sensor: este Node deve liberar o
 * seu? Só cede a quem está de fato assumindo (ASSOCIATING, BOUND ou ONLINE).
 * Quem tem sessão autenticada com o sensor permanece; quem deixou de ouvir
 * o sensor (STALE ou LOST) cede; sem sessão em nenhum dos lados, permanece o
 * de maior identificador.
 */
int ioc_fo_should_yield(uint8_t self_node, uint8_t self_state, int self_secure,
                        uint8_t other_node, uint8_t other_state, int other_secure);

#ifdef __cplusplus
}
#endif

#endif /* IOC_FAILOVER_H */
