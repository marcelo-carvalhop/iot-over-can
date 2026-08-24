/**
 * @file serial_console.h
 * @brief Console de comandos ASCII via USB CDC (serial), com respostas
 * em texto simples. Permite montar e testar uma TUI usando
 * só o Pico 2 W na USB, sem precisar montar MPU-6050 nem o gateway ESP32
 * (ver comando SIMULATE). Ver README.md, seção "Console Serial".
 */
#ifndef SERIAL_CONSOLE_H
#define SERIAL_CONSOLE_H

#include <stdint.h>
#include <stdbool.h>
#include "edge_protocol_definitions.h"
#include "dsp_pipeline.h"
#include "battery_monitor.h"
#include "edge_network_driver.h" // necessário pelo tipo NetworkMode

// Chamada uma vez no boot, depois de dsp_init_engine()/mpu6050_init_dma_driver().
void serial_console_init(void);

// Deve ser chamada a cada iteração do loop principal. Não bloqueia (usa
// leitura não-bloqueante de stdio); processa no máximo 1 linha completa por
// chamada para não monopolizar o loop caso várias linhas cheguem juntas.
void serial_console_poll(void);

// true se o modo SIMULATE está ativo (ver main.c: quando ativo, o loop
// principal gera buffers sintéticos em vez de exigir hardware real).
bool serial_console_is_simulate_mode(void);

// Retorna true quando a serial está aguardando telemetria contínua ou uma
// amostra única. Usado pelo main para evitar gastar CPU gerando buffers quando
// ninguém está observando a telemetria local.
bool serial_console_wants_telemetry(void);
bool serial_console_wants_fft_once(void);

// Chamada por main.c sempre que um DSP_AnalysisResult novo for calculado
// (real ou simulado). Só imprime algo se "TELEMETRY ON" estiver ativo
// (checagem interna); seguro chamar incondicionalmente a cada buffer.
void serial_console_report_telemetry(const DSP_AnalysisResult *res, OperationModeFSM mode, uint16_t dtc_code, AcquisitionMode acq_mode, BatteryStatus batt, uint8_t dtc_count);
void serial_console_report_config_applied(const Payload_Configuration *cfg, float effective_rate_hz, uint16_t effective_window_size, AcquisitionMode acq_mode);
void serial_console_report_fft(const float *fft_mag, uint16_t bins, bool valid);

// Chamada por main.c (em contexto de loop principal, nunca de ISR) sempre
// que um DTC_Record for retirado da fila de eventos críticos.
void serial_console_report_dtc(const DTC_Record *rec);

// Chamada por main.c sempre que o estado da rede mudar (ex.: DISCOVERY -> BOUND).
void serial_console_report_net_state(NetworkMode mode);

#endif // SERIAL_CONSOLE_H
