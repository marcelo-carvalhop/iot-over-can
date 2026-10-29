#ifndef PESQUISA_H
#define PESQUISA_H

/*
 * Instrumentação para pesquisa (ver Documentacao/pesquisa/metodologia.md).
 *
 * 1. Relógio monotônico de 64 bits em microssegundos. Cada placa tem o seu
 *    domínio de relógio: as linhas da Probe 00 levam t_us no domínio "probe";
 *    os Nodes medem intervalos no domínio "node". Intervalos de domínios
 *    diferentes podem ser comparados (a deriva entre cristais é de partes por
 *    milhão); instantes, não.
 *
 * 2. Parâmetros do enlace e da máquina de estados do vínculo, ajustáveis na
 *    compilação para análise de sensibilidade (-D IOT_ASSOC_STALE_MS=...). A
 *    Probe 00 os publica na partida ([GW] CONFIG ...), e o registro de cada
 *    experimento os guarda.
 *
 * 3. Comandos seriais em qualquer placa:
 *      BENCH CRYPTO [n]   custo da criptografia do enlace nesta placa (ioc_bench)
 *      CONFIG             repete a linha [GW] CONFIG
 */

#include <Arduino.h>
#include <esp_timer.h>
#include <stdint.h>

/* Vínculo publicado como STALE quando o sensor não é ouvido por este tempo (L). */
#ifndef IOT_ASSOC_STALE_MS
#define IOT_ASSOC_STALE_MS 15000
#endif
/* De STALE a LOST. */
#ifndef IOT_ASSOC_LOST_MS
#define IOT_ASSOC_LOST_MS 45000
#endif
/* Histerese da volta a ONLINE: o sensor precisa ser ouvido sem interrupção por
 * este tempo (H). 0 = volta na primeira observação (comportamento original). */
#ifndef IOT_ASSOC_HYSTERESIS_MS
#define IOT_ASSOC_HYSTERESIS_MS 0
#endif
/* Período padrão de encaminhamento da telemetria ao CAN (T). */
#ifndef IOT_STREAM_PERIOD_MS
#define IOT_STREAM_PERIOD_MS 1000
#endif

inline uint64_t pesquisaMonoUs() { return static_cast<uint64_t>(esp_timer_get_time()); }

/* [GW] CONFIG ... com os parâmetros desta compilação (Probe 00, na partida). */
void pesquisaPrintConfig();
bool pesquisaHandleSerialCommand(const char* command);

#endif
