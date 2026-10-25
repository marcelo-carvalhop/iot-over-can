/**
 * @file ioc_view.h
 * @brief Estado da rede reconstruído a partir de quadros CAN.
 *
 * É o mesmo modelo qualquer que seja a fonte dos quadros: o tap do Node pela
 * sessão autenticada, o TWAI do próprio painel em modo só escuta ou um cenário
 * da bancada virtual. Os instantes são do relógio de quem chama (ms); cada
 * quadro chega com a idade que tinha no Node.
 */
#ifndef IOC_VIEW_H
#define IOC_VIEW_H

#include <stddef.h>
#include <stdint.h>

#include "ioc_panel_proto.h"
#include "ioc_wdata.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IOC_VIEW_NODES        32
#define IOC_VIEW_SENSORS      24
#define IOC_VIEW_EVENTS       24
#define IOC_VIEW_JOINS        8
#define IOC_VIEW_LOAD_BINS    36
#define IOC_VIEW_LOAD_BIN_MS  600000u   /* 10 min por ponto: 6 h no gráfico */

typedef enum {
    IOC_VEV_LEADER    = 1,  /* node = novo líder, arg = anterior */
    IOC_VEV_ELECTION  = 2,
    IOC_VEV_JOIN      = 3,  /* node entrou (ou reentrou) na rede */
    IOC_VEV_FAILOVER  = 4,  /* node = destino, arg = motivo */
    IOC_VEV_ASSOC     = 5,  /* arg = estado do vínculo */
    IOC_VEV_FAULT     = 6,  /* arg = tipo do ensaio; child = estado */
    IOC_VEV_DTC       = 7,  /* arg = severidade */
    IOC_VEV_NODE_LOST = 8,
    IOC_VEV_NODE_BACK = 9
} ioc_view_event_kind;

typedef struct {
    uint32_t ms;
    uint8_t  kind;
    uint8_t  node;
    uint8_t  child;
    uint8_t  arg;
} ioc_view_event;

typedef struct {
    uint8_t  seen;
    uint8_t  status;          /* estado replicado pelo líder (0xFF desconhecido) */
    uint8_t  absent;
    uint32_t first_ms;
    uint32_t last_ms;
    uint32_t absent_since_ms;
    uint8_t  join_n;
    uint32_t join_ms[IOC_VIEW_JOINS];
} ioc_view_node;

typedef struct {
    uint8_t  used;
    uint8_t  node;
    uint8_t  child;
    uint8_t  uuid_parts;      /* 1 = parte A, 2 = parte B */
    uint64_t uuid;
    uint8_t  profile;
    uint8_t  assoc;
    int8_t   rssi;
    uint32_t assoc_since_ms;
    uint32_t last_ms;
    uint8_t  link_state;      /* IOC_WD_LINK_*; 0xFF sem relatório */
    uint32_t link_since_ms;
    uint16_t auth_fail;
    uint16_t replay;
    uint16_t lost;
    uint16_t rtt_100us;
    uint8_t  sec_base_set;
    uint16_t auth_base;
    uint16_t replay_base;
    uint32_t sec_since_ms;    /* primeiro aumento dos contadores de rejeição */
    uint8_t  have_value;
    float    value;           /* RMS (m/s²) ou LAeq (dB) */
    float    value2;          /* curtose ou dose NR-15 (%) */
    uint32_t value_ms;
    uint8_t  dtc_count;
    uint16_t dtc_code;
    uint8_t  dtc_sev;
    uint32_t dtc_ms;
    uint8_t  battery_pct;
    uint8_t  panel_flags;     /* perfil painel: último STATUS */
} ioc_view_sensor;

typedef struct {
    uint32_t now_ms;
    uint32_t started_ms;
    uint8_t  leader;
    uint8_t  prev_leader;
    uint32_t leader_since_ms;
    uint32_t last_heartbeat_ms;
    uint32_t last_election_ms;
    uint32_t hb_period_ms;

    ioc_view_node   nodes[IOC_VIEW_NODES];
    ioc_view_sensor sensors[IOC_VIEW_SENSORS];
    ioc_view_event  events[IOC_VIEW_EVENTS];
    uint8_t  ev_head;
    uint8_t  ev_count;

    uint8_t  fault_active;
    uint8_t  fault_kind;
    uint8_t  fault_node;
    uint32_t fault_since_ms;

    uint8_t  failover_seq;
    uint8_t  failover_uuid_a[5];
    uint8_t  failover_to;
    uint8_t  failover_reason;
    uint64_t failover_uuid;
    uint32_t failover_ms;

    /* Ocupação do barramento, a partir dos lotes ao vivo. */
    uint32_t win_bits;
    uint32_t win_span_ms;
    float    load_pct;
    float    load_peak_pct;
    uint32_t bin_start_ms;
    float    bin_sum;
    uint16_t bin_n;
    uint8_t  load_hist[IOC_VIEW_LOAD_BINS];  /* média de cada ponto, em 0,5 % */
    uint8_t  load_n;

    uint32_t frames;
    uint32_t batches;
    uint32_t batch_gaps;
    uint16_t last_seq;
    uint8_t  have_seq;
    uint8_t  snapshot_done;
    uint32_t last_batch_ms;

    uint8_t  self_node;
    uint8_t  self_child;
    ioc_wd_rx rx[IOC_VIEW_NODES];
} ioc_view;

void ioc_view_init(ioc_view *v, uint32_t now_ms);
void ioc_view_set_self(ioc_view *v, uint8_t node, uint8_t child);
/* Um quadro, com o instante (relógio de quem chama) em que foi observado. */
void ioc_view_frame(ioc_view *v, uint16_t id, const uint8_t *data, uint8_t len, uint32_t at_ms);
/* Um lote FRAMES inteiro (o buffer começa no magic). Devolve os quadros lidos ou -1. */
int  ioc_view_batch(ioc_view *v, const uint8_t *buf, size_t len, uint32_t now_ms);
/* Atualiza ausências. */
void ioc_view_tick(ioc_view *v, uint32_t now_ms);

uint8_t ioc_view_node_count(const ioc_view *v, uint8_t *absent);
uint8_t ioc_view_sensor_count(const ioc_view *v);
const ioc_view_event *ioc_view_event_at(const ioc_view *v, uint8_t newest_index);
/* Entradas de um Node nas últimas `window_ms`. */
uint8_t ioc_view_joins_within(const ioc_view *v, uint8_t node, uint32_t window_ms);
/* Ocupação média dos últimos pontos (em %). */
float ioc_view_load_avg(const ioc_view *v);

#ifdef __cplusplus
}
#endif

#endif
