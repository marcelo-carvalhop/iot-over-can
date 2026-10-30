/*
 * Amostragem de Poisson das sondas de eco (pesquisa.h): limites, média,
 * reprodutibilidade pela semente e independência entre sementes vizinhas.
 * Um trem de período fixo entra em fase com as atividades periódicas da rede;
 * este teste também mostra que os intervalos sorteados espalham as sondas por
 * toda a fase de uma atividade de 5 s.
 */
#include <stdio.h>

#include "pesquisa.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FALHA %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
  const uint32_t meanUs = 500000;

  // Limites e média (0,998 T com os limites [T/10, 5T]).
  uint32_t s = pesquisaSemente(12345);
  double sum = 0;
  uint32_t lo = 0xFFFFFFFFu, hi = 0;
  const int n = 200000;
  for (int i = 0; i < n; ++i) {
    const uint32_t v = pesquisaPoissonUs(s, meanUs);
    sum += v;
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  const double mean = sum / n;
  CHECK(lo >= meanUs / 10);
  CHECK(hi <= meanUs * 5);
  CHECK(mean > 0.98 * meanUs && mean < 1.02 * meanUs);

  // A mesma semente dá a mesma sequência; sementes vizinhas, sequências diferentes.
  uint32_t a = pesquisaSemente(7), b = pesquisaSemente(7), c = pesquisaSemente(8);
  bool same = true, differ = false;
  for (int i = 0; i < 64; ++i) {
    const uint32_t va = pesquisaPoissonUs(a, meanUs), vb = pesquisaPoissonUs(b, meanUs), vc = pesquisaPoissonUs(c, meanUs);
    same = same && va == vb;
    differ = differ || va != vc;
  }
  CHECK(same);
  CHECK(differ);

  // Semente zero não trava o gerador (xorshift com estado zero fica em zero).
  uint32_t z = pesquisaSemente(0);
  CHECK(z != 0);
  CHECK(pesquisaXorshift32(z) != 0);

  // Fase das sondas em relação a uma atividade de 5 s: o trem periódico de
  // 500 ms ocupa só 10 fases; o de Poisson cobre os 50 compartimentos de 100 ms.
  bool periodicBins[50] = {false}, poissonBins[50] = {false};
  uint64_t tPeriodic = 0, tPoisson = 0;
  uint32_t r = pesquisaSemente(2026);
  for (int i = 0; i < 400; ++i) {
    tPeriodic += meanUs;
    tPoisson += pesquisaPoissonUs(r, meanUs);
    periodicBins[(tPeriodic % 5000000u) / 100000u] = true;
    poissonBins[(tPoisson % 5000000u) / 100000u] = true;
  }
  int periodicCovered = 0, poissonCovered = 0;
  for (int i = 0; i < 50; ++i) {
    periodicCovered += periodicBins[i];
    poissonCovered += poissonBins[i];
  }
  CHECK(periodicCovered == 10);
  CHECK(poissonCovered == 50);

  if (failures) {
    fprintf(stderr, "test_pesquisa: %d falha(s)\n", failures);
    return 1;
  }
  printf("test_pesquisa: PASS (media %.0f us, min %u, max %u)\n", mean, lo, hi);
  return 0;
}
