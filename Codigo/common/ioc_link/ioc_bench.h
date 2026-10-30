/**
 * @file ioc_bench.h
 * @brief Medição do custo da criptografia do enlace no próprio alvo.
 *
 * O mesmo código roda no Node (ESP32), no sensor (Pico W) e no computador:
 * cada um fornece o próprio relógio monotônico em microssegundos e a função
 * que escreve a linha. Mede a implementação usada pelo enlace (SHA-256 e
 * HMAC em software, ioc_sha256.c), não um acelerador que o projeto não usa.
 *
 * Cada operação é cronometrada individualmente, de modo que preempções por
 * interrupções e pelas tarefas do rádio aparecem nos percentis altos; a média
 * vem do tempo total dividido pelo número de repetições, com resolução melhor
 * que a do relógio. Uma linha por operação:
 *
 *   BENCH domain=node op=HMAC_SHA256 bytes=64 n=200 mean_us=14.2 min_us=14
 *         p50_us=14 p95_us=15 p99_us=21 max_us=48
 *
 * Operações: HMAC_SHA256 sobre 16, 64, 256 e 1024 bytes; ENV_SEAL e
 * ENV_VERIFY de um datagrama de telemetria (96 bytes de carga); o custo de
 * CPU de um handshake inteiro em cada lado (HANDSHAKE_NODE e
 * HANDSHAKE_SENSOR: derivações, verificações e selos de HELLO, CHALLENGE e
 * CONFIRM, sem o tempo de rádio).
 */
#ifndef IOC_BENCH_H
#define IOC_BENCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IOC_BENCH_MAX_N      512
#define IOC_BENCH_DEFAULT_N  200
#define IOC_BENCH_TELEMETRY  96   /* bytes de carga de um datagrama de telemetria típico */

typedef uint64_t (*ioc_bench_clock_us)(void);
typedef void (*ioc_bench_emit)(const char *line, void *ctx);

typedef struct {
    double   mean_us;
    uint32_t min_us, p50_us, p95_us, p99_us, max_us;
    uint16_t n;
} ioc_bench_stats;

/* Estatísticas de amostras (ordena o vetor no lugar; percentil pelo posto mais próximo). */
void ioc_bench_summarize(uint32_t *samples, uint16_t n, uint64_t total_us, ioc_bench_stats *out);

/* Executa a bateria inteira; devolve o número de operações medidas. */
int ioc_bench_crypto(const char *domain, uint16_t n, ioc_bench_clock_us clock, ioc_bench_emit emit, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
