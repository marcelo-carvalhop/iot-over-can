/**
 * @file edge_diagnostics.h
 * @brief Tabela e fila de DTCs compartilhadas pelos firmwares de sensor (vibração e acústico).
 *
 * Os nomes públicos são os mesmos que os drivers já usavam por `extern`
 * (diag_report_dtc_event, diagnostics_get_count, ...). O registro de um DTC é
 * seguro em contexto de interrupção do núcleo 0 (callbacks do lwIP e do
 * BTstack): só atualiza a tabela e enfileira o evento. O envio ao Node e ao
 * console é feito pelo laço principal, que retira os eventos com
 * edge_diag_pop_pending().
 *
 * Não chamar a partir do núcleo 1: a exclusão mútua é feita desabilitando
 * interrupções no núcleo que chama, não entre núcleos.
 */
#ifndef EDGE_DIAGNOSTICS_H
#define EDGE_DIAGNOSTICS_H

#include <stdbool.h>
#include <stdint.h>
#include "edge_protocol_definitions.h"

void     edge_diag_init(void);

/**
 * Contexto copiado no freeze frame de cada DTC: modo de operação,
 * temperatura do sensor (INT8_MIN quando ele não mede), temperatura do
 * microcontrolador e tensão de alimentação. O laço principal atualiza estes
 * valores; o registro de um DTC nunca acessa I2C ou ADC.
 */
void     edge_diag_set_context(uint8_t fsm_state, int8_t sensor_temp_c, int8_t mcu_temp_c, uint16_t vcc_mv);

void     diag_report_dtc_event(uint16_t code, uint8_t symptom, uint8_t severity);
void     diagnostics_clear_all(void);
uint8_t  diagnostics_get_count(void);
uint16_t diagnostics_get_active_code(void);
uint16_t bite_get_current_status(void);

/** Retira o próximo DTC pendente de envio. Retorna false quando a fila está vazia. */
bool     edge_diag_pop_pending(DTC_Record *out);

#endif /* EDGE_DIAGNOSTICS_H */
