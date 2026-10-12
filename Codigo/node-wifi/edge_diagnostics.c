/**
 * @file edge_diagnostics.c
 * @brief Tabela de DTCs ativos com freeze frame e fila de eventos para envio.
 *
 * Extraído de main.c (v0.18) sem mudança de comportamento, para que o sensor
 * de vibração e o sensor acústico usem a mesma política:
 *   - event-on-change: o mesmo DTC com o mesmo sintoma e severidade apenas
 *     atualiza a tabela, sem gerar novo evento;
 *   - repetição suprimida por DTC_DUPLICATE_SUPPRESS_MS;
 *   - o DTC ativo resumido é o de maior severidade e, no empate, o mais recente.
 */
#include "edge_diagnostics.h"

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/sync.h"

#define DTC_QUEUE_CAPACITY 8
#define DTC_TABLE_CAPACITY 8
#define DTC_DUPLICATE_SUPPRESS_MS 2000

typedef struct {
    bool active;
    DTC_Record rec;
} DTC_TableEntry;

static volatile uint16_t g_active_dtc = DTC_NONE;

static volatile DTC_Record g_dtc_queue[DTC_QUEUE_CAPACITY];
static volatile uint8_t    g_dtc_queue_head = 0;
static volatile uint8_t    g_dtc_queue_tail = 0;
static volatile uint8_t    g_dtc_queue_count = 0;

static DTC_TableEntry g_dtc_table[DTC_TABLE_CAPACITY];
static uint16_t g_last_queued_dtc_code = DTC_NONE;
static uint8_t  g_last_queued_dtc_symptom = 0;
static uint8_t  g_last_queued_dtc_severity = 0;
static uint32_t g_last_queued_dtc_ms = 0;

// Contexto do freeze frame, mantido pelo laço principal.
static volatile uint8_t  g_ctx_fsm_state = 0;
static volatile int8_t   g_ctx_sensor_temp_c = 0;
static volatile int8_t   g_ctx_mcu_temp_c = 0;
static volatile uint16_t g_ctx_vcc_mv = 0xFFFF;

void edge_diag_init(void) {
    uint32_t irq_state = save_and_disable_interrupts();
    memset(g_dtc_table, 0, sizeof(g_dtc_table));
    g_active_dtc = DTC_NONE;
    g_dtc_queue_head = g_dtc_queue_tail = g_dtc_queue_count = 0;
    g_last_queued_dtc_code = DTC_NONE;
    g_last_queued_dtc_symptom = 0;
    g_last_queued_dtc_severity = 0;
    g_last_queued_dtc_ms = 0;
    restore_interrupts(irq_state);
}

void edge_diag_set_context(uint8_t fsm_state, int8_t sensor_temp_c, int8_t mcu_temp_c, uint16_t vcc_mv) {
    g_ctx_fsm_state = fsm_state;
    g_ctx_sensor_temp_c = sensor_temp_c;
    g_ctx_mcu_temp_c = mcu_temp_c;
    g_ctx_vcc_mv = vcc_mv;
}

static DTC_FreezeFrame make_freeze_frame(void) {
    DTC_FreezeFrame fr;
    fr.timestamp_ms = to_ms_since_boot(get_absolute_time());
    fr.fsm_state = g_ctx_fsm_state;
    // Usa o último snapshot ambiental coletado no loop principal. Isso evita
    // acessar I2C/ADC de dentro de caminhos de diagnóstico que podem ter sido
    // acionados por erro de I2C ou por callback de rede.
    fr.mpu_temp_c = g_ctx_sensor_temp_c;
    fr.mcu_temp_c = g_ctx_mcu_temp_c;
    fr.vcc_mv = g_ctx_vcc_mv;
    return fr;
}

static void update_active_dtc_summary(void) {
    uint16_t best = DTC_NONE;
    uint8_t best_sev = 0;
    uint32_t best_ts = 0;

    for (int i = 0; i < DTC_TABLE_CAPACITY; i++) {
        if (!g_dtc_table[i].active) continue;
        DTC_Record *r = &g_dtc_table[i].rec;
        if (r->severity > best_sev ||
            (r->severity == best_sev && r->freeze.timestamp_ms >= best_ts)) {
            best = r->dtc_code;
            best_sev = r->severity;
            best_ts = r->freeze.timestamp_ms;
        }
    }
    g_active_dtc = best;
}

static void diagnostics_store_record(const DTC_Record *rec) {
    int free_slot = -1;
    int oldest_slot = 0;
    uint32_t oldest_ts = UINT32_MAX;

    for (int i = 0; i < DTC_TABLE_CAPACITY; i++) {
        if (g_dtc_table[i].active && g_dtc_table[i].rec.dtc_code == rec->dtc_code) {
            g_dtc_table[i].rec = *rec;
            update_active_dtc_summary();
            return;
        }
        if (!g_dtc_table[i].active && free_slot < 0) free_slot = i;
        if (g_dtc_table[i].active && g_dtc_table[i].rec.freeze.timestamp_ms < oldest_ts) {
            oldest_ts = g_dtc_table[i].rec.freeze.timestamp_ms;
            oldest_slot = i;
        }
    }

    int slot = (free_slot >= 0) ? free_slot : oldest_slot;
    g_dtc_table[slot].active = true;
    g_dtc_table[slot].rec = *rec;
    update_active_dtc_summary();
}

static bool diagnostics_has_same_active_record(uint16_t code, uint8_t symptom, uint8_t severity) {
    for (int i = 0; i < DTC_TABLE_CAPACITY; i++) {
        if (g_dtc_table[i].active &&
            g_dtc_table[i].rec.dtc_code == code &&
            g_dtc_table[i].rec.symptom == symptom &&
            g_dtc_table[i].rec.severity == severity) {
            return true;
        }
    }
    return false;
}

void diagnostics_clear_all(void) {
    uint32_t irq_state = save_and_disable_interrupts();
    memset(g_dtc_table, 0, sizeof(g_dtc_table));
    g_active_dtc = DTC_NONE;
    g_dtc_queue_head = g_dtc_queue_tail = g_dtc_queue_count = 0;
    g_last_queued_dtc_code = DTC_NONE;
    restore_interrupts(irq_state);
}

uint8_t diagnostics_get_count(void) {
    uint8_t n = 0;
    for (int i = 0; i < DTC_TABLE_CAPACITY; i++) if (g_dtc_table[i].active) n++;
    return n;
}

uint16_t diagnostics_get_active_code(void) { return g_active_dtc; }
uint16_t bite_get_current_status(void) { return g_active_dtc; }

void diag_report_dtc_event(uint16_t code, uint8_t symptom, uint8_t severity) {
    DTC_Record rec;
    rec.dtc_code = code;
    rec.symptom = symptom;
    rec.severity = severity;
    rec.freeze = make_freeze_frame();

    // Event-on-change:
    // se o mesmo DTC já está ativo com o mesmo sintoma e severidade, apenas
    // atualiza a tabela interna. Não reenfileira DTC_EVENT assíncrono a cada
    // tentativa de leitura I2C, senão a serial fica inutilizável.
    bool already_active_same = diagnostics_has_same_active_record(code, symptom, severity);
    diagnostics_store_record(&rec);

    if (already_active_same) {
        return;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    bool duplicate_recent =
        (g_last_queued_dtc_code == code) &&
        (g_last_queued_dtc_symptom == symptom) &&
        (g_last_queued_dtc_severity == severity) &&
        ((rec.freeze.timestamp_ms - g_last_queued_dtc_ms) < DTC_DUPLICATE_SUPPRESS_MS);

    if (!duplicate_recent) {
        if (g_dtc_queue_count >= DTC_QUEUE_CAPACITY) {
            g_dtc_queue_tail = (g_dtc_queue_tail + 1) % DTC_QUEUE_CAPACITY;
            g_dtc_queue_count--;
        }
        g_dtc_queue[g_dtc_queue_head] = rec;
        g_dtc_queue_head = (g_dtc_queue_head + 1) % DTC_QUEUE_CAPACITY;
        g_dtc_queue_count++;
        g_last_queued_dtc_code = code;
        g_last_queued_dtc_symptom = symptom;
        g_last_queued_dtc_severity = severity;
        g_last_queued_dtc_ms = rec.freeze.timestamp_ms;
    }
    restore_interrupts(irq_state);
}

bool edge_diag_pop_pending(DTC_Record *out) {
    if (!out) return false;
    uint32_t irq_state = save_and_disable_interrupts();
    if (g_dtc_queue_count == 0) {
        restore_interrupts(irq_state);
        return false;
    }
    *out = (DTC_Record)g_dtc_queue[g_dtc_queue_tail];
    g_dtc_queue_tail = (g_dtc_queue_tail + 1) % DTC_QUEUE_CAPACITY;
    g_dtc_queue_count--;
    restore_interrupts(irq_state);
    return true;
}
