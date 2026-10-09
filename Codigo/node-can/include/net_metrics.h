#ifndef NET_METRICS_H
#define NET_METRICS_H

#include <Arduino.h>
#include <ACAN2515.h>

/*
 * Instrumentação do barramento na Probe 00.
 *
 * A Probe mede o que observa, sem hardware adicional:
 *   - ocupação do barramento, pela duração exata de cada quadro (os bits de
 *     preenchimento e o CRC são reconstruídos a partir do conteúdo);
 *   - período e variação do período de cada identificador;
 *   - contadores de erro e pico de ocupação da fila do controlador CAN;
 *   - transferências segmentadas concluídas e descartadas.
 *
 * Saída (uma linha por segundo e uma por identificador a cada 5 s):
 *   [GW] BUS_STATS t_ms=... win_ms=... frames=... bits=... load=... peak=...
 *        rx_err=... tx_err=... eflg=0x.. rx_peak=... ids=... xfer_ok=... xfer_err=... own=...
 *   [GW] ID_STATS id=0x... win_ms=... n=... dlc=... bits=... dt_min_us=... dt_avg_us=... dt_max_us=...
 *
 * O instante de cada quadro é lido quando o laço principal o retira da fila
 * do driver; por isso dt_min/dt_max incluem a latência desse laço (da ordem
 * de 1 ms). O período médio não é afetado.
 *
 * Nos Nodes funcionais todas as funções retornam imediatamente.
 */

void netMetricsInit();
void netMetricsPoll();
/* Quadro recebido do barramento. */
void netMetricsOnReceive(const CANMessage& rx);
/* Quadro transmitido pela própria Probe (ela não recebe o que envia). */
void netMetricsOnTransmit(const CANMessage& tx);
/* Resultado da remontagem de uma transferência segmentada. */
void netMetricsOnTransfer(bool ok);
/* METRICS ON | OFF | RESET */
bool netMetricsHandleSerialCommand(const char* command);

#endif
