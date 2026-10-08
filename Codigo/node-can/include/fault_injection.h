#ifndef FAULT_INJECTION_H
#define FAULT_INJECTION_H

#include <Arduino.h>
#include <ACAN2515.h>

/*
 * Ensaios de injeção de falhas, comandados pela Probe 00.
 *
 * Comandos seriais (Probe 00):
 *   FAULT POWER_CYCLE  <node> <segundos>   Node fora do ar e reinício
 *   FAULT SILENCE      <node> <segundos>   Node congelado, retoma em seguida
 *   FAULT AP_OUTAGE    <node> <segundos>   ponto de acesso Wi-Fi desligado
 *   FAULT DROP_SESSION <node>              sessões com os sensores descartadas
 *   FAULT LOAD <pct> <segundos> [HIGH|LOW] carga artificial gerada pela Probe
 *   FAULT CANCEL [node]                    encerra o ensaio em curso
 *
 * Saída:
 *   [GW] FAULT_TX kind=... node=... duration_ms=... seq=...
 *   [GW] FAULT_EVENT node=... kind=... state=STARTED|ENDED|REJECTED duration_ms=... seq=...
 *
 * O recurso existe para ensaio em bancada. Compilar com
 * -D IOT_FAULT_INJECTION=0 remove o tratamento dos comandos nos Nodes; ver
 * Documentacao/arquitetura/metricas-e-ensaios.md.
 */

#ifndef IOT_FAULT_INJECTION
#define IOT_FAULT_INJECTION 1
#endif

void faultInjectionInit();
void faultInjectionPoll();
bool faultInjectionHandleCanMessage(const CANMessage& rx);
bool faultInjectionHandleSerialCommand(const char* command);

/*
 * true enquanto o Node deve permanecer fora do ar: o laço principal não
 * transmite nem processa mensagens, apenas entrega os quadros recebidos a
 * faultInjectionHandleCanMessage() para que um cancelamento seja ouvido.
 */
bool faultInjectionSilent();

#endif
