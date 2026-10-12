/**
 * @file audio_console.h
 * @brief Console ASCII do sensor acústico pela USB (mesmas convenções do sensor de vibração).
 *
 * Uma linha por comando, respostas CHAVE=VALOR. Os comandos de leitura são
 * livres; os que alteram estado exigem AUTH UNLOCK <token>, como no firmware
 * de vibração. A TUI reconhece o perfil pela chave PROFILE=ACOUSTIC das linhas
 * STATUS e TEL.
 */
#ifndef AUDIO_CONSOLE_H
#define AUDIO_CONSOLE_H

#include <stdbool.h>
#include <stdint.h>

#include "audio_app.h"
#include "audio_core1.h"
#include "edge_network_driver.h"
#include "edge_protocol_definitions.h"

void audio_console_init(void);
void audio_console_poll(void);

/** Pedidos feitos pelo operador no console (APPLY, CAL, MEASURE, BASELINE). */
bool audio_console_take_pending_config(Payload_Configuration *out);
bool audio_console_take_pending_control(AudioPendingControl *out);

/** Cada relatório do núcleo 1 passa por aqui: TEL, FFT ONCE, CAL e mensagens [MIC]. */
void audio_console_on_report(const AudioReportMessage *msg);
void audio_console_report_config_result(const AudioCommandResult *res);
void audio_console_report_control_result(const AudioCommandResult *res);
void audio_console_report_dtc(const DTC_Record *rec);
void audio_console_report_net_state(NetworkMode mode);

#endif /* AUDIO_CONSOLE_H */
