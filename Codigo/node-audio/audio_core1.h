/**
 * @file audio_core1.h
 * @brief Núcleo 1 dedicado à aquisição e ao DSP acústico.
 *
 * Divisão de trabalho:
 *
 *   núcleo 0   rede (CYW43, lwIP, BTstack), console USB, diagnóstico, bateria
 *   núcleo 1   PIO + DMA do INMP441, acoustic_dsp.c, medição de carga
 *
 * Os núcleos conversam só por filas (pico/util/queue.h, protegidas por
 * spinlock): comandos do núcleo 0 para o 1, resultados e relatórios do 1 para
 * o 0. O núcleo 1 nunca chama a pilha de rede, o console ou o diagnóstico;
 * problemas detectados por ele seguem nos relatórios e o núcleo 0 decide o DTC.
 */
#ifndef AUDIO_CORE1_H
#define AUDIO_CORE1_H

#include <stdbool.h>
#include <stdint.h>

#include "acoustic_dsp.h"
#include "edge_protocol_definitions.h"

typedef enum {
    AUDIO_CMD_CONFIG  = 1,
    AUDIO_CMD_CONTROL = 2
} AudioCommandType;

typedef enum {
    AUDIO_ORIGIN_NETWORK = 0,   // Node CAN, responder pelo enlace
    AUDIO_ORIGIN_CONSOLE = 1    // console USB
} AudioCommandOrigin;

typedef struct {
    uint8_t  type;              // AudioCommandType
    uint8_t  origin;            // AudioCommandOrigin
    uint32_t token;             // request_counter ecoado na resposta
    Payload_Configuration config;
    uint8_t  action;            // AcousticControlAction
    uint16_t argument;
} AudioCommand;

typedef struct {
    uint8_t  type;
    uint8_t  origin;
    uint32_t token;
    bool     ok;
    uint8_t  action;
    Payload_Configuration config;   // pedido original
    float    effective_rate_hz;     // após aplicar
} AudioCommandResult;

typedef struct {
    AcousticReport report;
    uint8_t  cpu_pct;           // tempo do núcleo 1 processando / tempo decorrido
    uint8_t  channel;           // Inmp441Channel em uso
    bool     channel_switched;  // troca automática de canal neste intervalo
    uint32_t lost_blocks_total;
    uint32_t dropped_reports;   // relatórios descartados por fila cheia
} AudioReportMessage;

/** Lança o núcleo 1 com a configuração inicial (já validada). */
bool audio_core1_start(const Payload_Configuration *initial);

/** Fila de comandos; false se cheia (o núcleo 0 tenta de novo depois). */
bool audio_core1_post(const AudioCommand *cmd);
bool audio_core1_poll_result(AudioCommandResult *out);
bool audio_core1_poll_report(AudioReportMessage *out);

/** Contador incrementado a cada volta do laço do núcleo 1 (vigilância). */
uint32_t audio_core1_heartbeat(void);
/** Taxa efetiva e canal atuais, para STATUS e para o menu de configuração. */
float    audio_core1_effective_rate_hz(void);
uint8_t  audio_core1_channel(void);
bool     audio_core1_hardware_ok(void);

#endif /* AUDIO_CORE1_H */
