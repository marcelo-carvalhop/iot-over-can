/*
 * Medição da criptografia (ioc_bench): estatísticas conhecidas, formato das
 * linhas e a bateria com um relógio artificial. Com "--host" executa a
 * bateria com o relógio real do computador, como referência (domain=host).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ioc_bench.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FALHA %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static uint64_t fake_now = 0;
static uint64_t fake_clock(void) { return fake_now += 3; }  /* cada leitura avança 3 us */

static uint64_t host_clock(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static char lines[16][220];
static int nlines = 0;
static void collect(const char *line, void *ctx) {
    (void)ctx;
    if (nlines < 16) snprintf(lines[nlines++], sizeof(lines[0]), "%s", line);
}
static void print_line(const char *line, void *ctx) { (void)ctx; puts(line); }

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--host") == 0) {
        return ioc_bench_crypto("host", IOC_BENCH_DEFAULT_N, host_clock, print_line, NULL) == 8 ? 0 : 1;
    }
    /* Posto mais próximo sobre 1..100: p50=50, p95=95, p99=99. */
    uint32_t s[100];
    for (int i = 0; i < 100; ++i) s[i] = (uint32_t)(100 - i);
    ioc_bench_stats st;
    ioc_bench_summarize(s, 100, 5050, &st);
    CHECK(st.min_us == 1 && st.max_us == 100);
    CHECK(st.p50_us == 50 && st.p95_us == 95 && st.p99_us == 99);
    CHECK(st.mean_us > 50.49 && st.mean_us < 50.51);
    uint32_t one[1] = {7};
    ioc_bench_summarize(one, 1, 7, &st);
    CHECK(st.p50_us == 7 && st.p99_us == 7 && st.n == 1);

    /* Bateria com relógio artificial: oito operações, cada amostra vale 3 us. */
    CHECK(ioc_bench_crypto("teste", 10, fake_clock, collect, NULL) == 8);
    CHECK(nlines == 8);
    CHECK(strstr(lines[0], "BENCH domain=teste op=HMAC_SHA256 bytes=16 n=10 ") == lines[0]);
    CHECK(strstr(lines[0], " p50_us=3 ") != NULL && strstr(lines[0], " max_us=3") != NULL);
    CHECK(strstr(lines[4], "op=ENV_SEAL bytes=96") != NULL);
    CHECK(strstr(lines[6], "op=HANDSHAKE_NODE") != NULL && strstr(lines[7], "op=HANDSHAKE_SENSOR") != NULL);
    /* Limites: zero vira o padrão e o máximo é respeitado. */
    nlines = 0;
    CHECK(ioc_bench_crypto("teste", 9999, fake_clock, collect, NULL) == 8);
    CHECK(strstr(lines[0], " n=512 ") != NULL);
    CHECK(ioc_bench_crypto("teste", 5, NULL, collect, NULL) == 0);

    if (failures) {
        fprintf(stderr, "test_ioc_bench: %d falha(s)\n", failures);
        return 1;
    }
    puts("test_ioc_bench: PASS");
    return 0;
}
