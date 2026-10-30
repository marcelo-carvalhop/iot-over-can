"""Estatística sem dependências externas.

Latências de rede são assimétricas e de cauda longa: além de média e desvio,
a análise reporta mediana, p95 e p99, intervalos de confiança por bootstrap
e tamanhos de efeito não paramétricos (delta de Cliff) ao lado do g de
Hedges. A unidade de replicação é a repetição do experimento, não a amostra:
comparações entre condições usam um valor por repetição.
"""
from __future__ import annotations

import math
import random
from collections.abc import Callable, Sequence

Z95 = 1.959963984540054

# t de Student bicaudal 95 % (graus de liberdade 1..30); acima, aproximação de Cornish-Fisher.
_T95 = (12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228, 2.201, 2.179, 2.160, 2.145,
        2.131, 2.120, 2.110, 2.101, 2.093, 2.086, 2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048,
        2.045, 2.042)


def t95(df: int) -> float:
    if df < 1:
        return math.nan
    if df <= len(_T95):
        return _T95[df - 1]
    z = Z95
    return z + (z ** 3 + z) / (4 * df) + (5 * z ** 5 + 16 * z ** 3 + 3 * z) / (96 * df ** 2)


def percentile(values: Sequence[float], q: float) -> float:
    """Percentil com interpolação linear (o tipo 7 de Hyndman e Fan, padrão do R e do NumPy)."""
    if not values:
        return math.nan
    xs = sorted(values)
    if len(xs) == 1:
        return float(xs[0])
    pos = (len(xs) - 1) * q / 100.0
    lo = math.floor(pos)
    hi = min(lo + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (pos - lo)


def mean(values: Sequence[float]) -> float:
    return sum(values) / len(values) if values else math.nan


def stdev(values: Sequence[float]) -> float:
    n = len(values)
    if n < 2:
        return math.nan
    m = mean(values)
    return math.sqrt(sum((x - m) ** 2 for x in values) / (n - 1))


def ci_mean(values: Sequence[float]) -> tuple[float, float]:
    """IC 95 % da média pela distribuição t."""
    n = len(values)
    if n < 2:
        return (math.nan, math.nan)
    m, s = mean(values), stdev(values)
    half = t95(n - 1) * s / math.sqrt(n)
    return (m - half, m + half)


def bootstrap_ci(values: Sequence[float], stat: Callable[[Sequence[float]], float] = mean,
                 reps: int = 2000, seed: int = 20261029) -> tuple[float, float]:
    """IC 95 % por bootstrap percentílico, com semente fixa (reprodutível)."""
    n = len(values)
    if n < 2:
        return (math.nan, math.nan)
    rng = random.Random(seed)
    data = list(values)
    stats = sorted(stat([data[rng.randrange(n)] for _ in range(n)]) for _ in range(reps))
    return (percentile(stats, 2.5), percentile(stats, 97.5))


def summary(values: Sequence[float]) -> dict[str, float]:
    """Resumo de uma distribuição: n, média, desvio, mínimo, mediana, p95, p99, máximo e IC da média."""
    vals = [float(v) for v in values if v is not None and not math.isnan(float(v))]
    lo, hi = ci_mean(vals)
    return {
        "n": len(vals), "media": mean(vals), "desvio": stdev(vals),
        "min": min(vals) if vals else math.nan, "p50": percentile(vals, 50), "p95": percentile(vals, 95),
        "p99": percentile(vals, 99), "max": max(vals) if vals else math.nan,
        "ic95_inf": lo, "ic95_sup": hi, "dispersao_p99_p50": percentile(vals, 99) - percentile(vals, 50),
    }


def wilson(successes: int, n: int, z: float = Z95) -> tuple[float, float, float]:
    """Proporção e intervalo de Wilson (adequado a proporções pequenas, como perdas)."""
    if n <= 0:
        return (math.nan, math.nan, math.nan)
    p = successes / n
    denom = 1 + z * z / n
    centre = (p + z * z / (2 * n)) / denom
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / denom
    return (p, max(0.0, centre - half), min(1.0, centre + half))


def hedges_g(a: Sequence[float], b: Sequence[float]) -> tuple[float, float, float]:
    """g de Hedges (b em relação a a) e IC 95 % aproximado."""
    na, nb = len(a), len(b)
    if na < 2 or nb < 2:
        return (math.nan, math.nan, math.nan)
    sa, sb = stdev(a), stdev(b)
    pooled = math.sqrt(((na - 1) * sa * sa + (nb - 1) * sb * sb) / (na + nb - 2))
    if pooled == 0:
        return (0.0 if mean(a) == mean(b) else math.copysign(math.inf, mean(b) - mean(a)), math.nan, math.nan)
    d = (mean(b) - mean(a)) / pooled
    j = 1 - 3 / (4 * (na + nb) - 9)
    g = d * j
    se = math.sqrt((na + nb) / (na * nb) + g * g / (2 * (na + nb)))
    return (g, g - Z95 * se, g + Z95 * se)


def cliffs_delta(a: Sequence[float], b: Sequence[float]) -> tuple[float, str]:
    """Delta de Cliff (b em relação a a) e magnitude (Romano et al., 2006)."""
    if not a or not b:
        return (math.nan, "indefinido")
    greater = less = 0
    sb = sorted(b)
    import bisect
    for x in a:
        less += bisect.bisect_left(sb, x)            # b < x
        greater += len(sb) - bisect.bisect_right(sb, x)  # b > x
    delta = (greater - less) / (len(a) * len(b))
    mag = abs(delta)
    label = ("desprezível" if mag < 0.147 else "pequeno" if mag < 0.33 else "médio" if mag < 0.474 else "grande")
    return (delta, label)


def mann_whitney(a: Sequence[float], b: Sequence[float]) -> tuple[float, float]:
    """U de Mann-Whitney e valor-p bicaudal pela aproximação normal com correção de empates."""
    na, nb = len(a), len(b)
    if na == 0 or nb == 0:
        return (math.nan, math.nan)
    allv = sorted([(x, 0) for x in a] + [(x, 1) for x in b])
    ranks = [0.0] * len(allv)
    i = 0
    ties = 0.0
    while i < len(allv):
        j = i
        while j + 1 < len(allv) and allv[j + 1][0] == allv[i][0]:
            j += 1
        r = (i + j) / 2 + 1
        for k in range(i, j + 1):
            ranks[k] = r
        t = j - i + 1
        ties += t ** 3 - t
        i = j + 1
    ra = sum(r for r, (_, g) in zip(ranks, allv) if g == 0)
    u = ra - na * (na + 1) / 2
    n = na + nb
    mu = na * nb / 2
    sigma = math.sqrt(na * nb / 12 * ((n + 1) - ties / (n * (n - 1)))) if n > 1 else 0.0
    if sigma == 0:
        return (u, 1.0)
    z = (abs(u - mu) - 0.5) / sigma
    p = math.erfc(max(z, 0.0) / math.sqrt(2))
    return (u, min(1.0, p))


def holm(pvalores: Sequence[float]) -> list[float]:
    """Valores-p ajustados pelo método de Holm (controle do erro de família em comparações múltiplas)."""
    ordem = sorted(range(len(pvalores)), key=lambda i: pvalores[i])
    m = len(pvalores)
    ajustados = [math.nan] * m
    acumulado = 0.0
    for posto, i in enumerate(ordem):
        if math.isnan(pvalores[i]):
            continue
        acumulado = max(acumulado, min(1.0, (m - posto) * pvalores[i]))
        ajustados[i] = acumulado
    return ajustados
