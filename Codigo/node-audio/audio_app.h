/**
 * @file audio_app.h
 * @brief Parte do enlace específica do sensor acústico (núcleo 0).
 *
 * Implementa os ganchos de edge_app.h, guarda os pedidos recebidos do Node
 * até o laço principal repassá-los ao núcleo 1 e monta os datagramas de
 * telemetria acústica (Payload_AcousticStream + espectro).
 */
#ifndef AUDIO_APP_H
#define AUDIO_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "audio_core1.h"
#include "edge_app.h"
#include "edge_protocol_definitions.h"

typedef struct {
    bool     valid;
    uint32_t request_counter;
    uint8_t  action;
    uint16_t argument;
} AudioPendingControl;

void audio_app_init(const Payload_Configuration *initial);

/** Configuração em vigor (aplicada pelo núcleo 1) e taxa efetiva. */
const Payload_Configuration *audio_app_current_config(void);
void  audio_app_set_current_config(const Payload_Configuration *cfg, float effective_rate_hz);
float audio_app_effective_rate_hz(void);

/** Pedidos vindos do Node (registrados no contexto do lwIP). */
bool  audio_app_take_pending_config(Payload_Configuration *out);
bool  audio_app_take_pending_control(AudioPendingControl *out);

/** Envia o relatório ao Node; false sem sessão. */
bool  audio_app_send_report(const AudioReportMessage *msg, uint16_t seq);
void  audio_app_send_control_ack(uint32_t request_counter, uint8_t action, uint8_t status, uint16_t detail);

/** Conversão comum ao datagrama e ao console: dB -> centésimos de dB (NAN -> ACOUSTIC_CB_NA). */
int16_t audio_db_to_cb(float db);

#endif /* AUDIO_APP_H */
