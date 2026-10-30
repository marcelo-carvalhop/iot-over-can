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
#include <math.h>
#include <stdint.h>

/* Vínculo publicado como STALE quando o sensor não é ouvido por este tempo (L). */
#ifndef IOT_ASSOC_STALE_MS
#define IOT_ASSOC_STALE_MS 15000
#endif
/* De STALE a LOST. */
#ifndef IOT_ASSOC_LOST_MS
#define IOT_ASSOC_LOST_MS 45000
#endif
/* Histerese da volta a ONLINE (H): tempo mínimo em STALE ou LOST antes de o
 * vínculo voltar a ONLINE. 0 = volta na primeira observação (comportamento
 * original). */
#ifndef IOT_ASSOC_HYSTERESIS_MS
#define IOT_ASSOC_HYSTERESIS_MS 0
#endif
/* Período padrão de encaminhamento da telemetria ao CAN (T). */
#ifndef IOT_STREAM_PERIOD_MS
#define IOT_STREAM_PERIOD_MS 1000
#endif

inline uint64_t pesquisaMonoUs() { return static_cast<uint64_t>(esp_timer_get_time()); }

/*
 * Amostragem de Poisson para sondas (RFC 2330, seção 11.1.1). Um trem de
 * sondas de período fixo entra em fase com as atividades periódicas da rede
 * (sinal de presença a 1 s, relatório do enlace a 5 s, PING a 3 s) e passa a
 * medir sempre o mesmo instante delas: o resultado depende da fase, não da
 * condição. Com intervalos exponenciais, cada sonda cai num instante
 * independente das atividades periódicas e a média das sondas estima a média
 * no tempo (PASTA). Os intervalos são limitados a [T/10, 5T] para que o trem
 * tenha duração previsível; a média continua a ser T (0,998 T).
 */
inline uint32_t pesquisaSemente(uint32_t seed) {
  uint32_t s = seed * 0x9E3779B1u;  // espalha sementes pequenas pelos 32 bits
  s ^= s >> 16;
  s *= 0x85EBCA6Bu;
  s ^= s >> 13;
  return s ? s : 1u;
}

inline uint32_t pesquisaXorshift32(uint32_t& state) {
  uint32_t x = state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  state = x;
  return x;
}

inline uint32_t pesquisaPoissonUs(uint32_t& state, uint32_t meanUs) {
  const double u = (static_cast<double>(pesquisaXorshift32(state) >> 8) + 1.0) / 16777216.0;  // (0, 1]
  double v = -log(u) * static_cast<double>(meanUs);
  const double lo = meanUs / 10.0, hi = meanUs * 5.0;
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  return static_cast<uint32_t>(v);
}

/* [GW] CONFIG ... com os parâmetros desta compilação (Probe 00, na partida). */
void pesquisaPrintConfig();
bool pesquisaHandleSerialCommand(const char* command);

#endif
