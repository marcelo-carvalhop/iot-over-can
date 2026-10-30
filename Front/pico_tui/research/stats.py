"""Estatística descritiva das medições de pesquisa, sem dependências externas.

Mesmas definições de ``Pesquisa/iocpesquisa/estatistica.py`` (percentil tipo 7,
IC 95 % da média pela distribuição t, intervalo de Wilson para proporções),
para que a tela da TUI e a análise das campanhas digam a mesma coisa sobre
os mesmos números. Um teste confere as duas implementações.
"""
from __future__ import annotations

import math
from collections.abc import Sequence
from dataclasses import dataclass

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
    """Percentil com interpolação linear (tipo 7 de Hyndman e Fan, padrão do R e do NumPy)."""

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


def wilson(successes: int, n: int, z: float = Z95) -> tuple[float, float, float]:
    """Proporção e intervalo de Wilson (adequado a proporções pequenas, como perdas)."""

    if n <= 0:
        return (math.nan, math.nan, math.nan)
    p = successes / n
    denom = 1 + z * z / n
    centre = (p + z * z / (2 * n)) / denom
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / denom
    return (p, max(0.0, centre - half), min(1.0, centre + half))


@dataclass(slots=True, frozen=True)
class Summary:
    n: int
    mean: float
    stdev: float
    minimum: float
    p50: float
    p95: float
    p99: float
    maximum: float
    ci_low: float
    ci_high: float

    @property
    def valid(self) -> bool:
        return self.n > 0


def summarize(values: Sequence[float]) -> Summary:
    vals = [float(v) for v in values if v is not None and not math.isnan(float(v))]
    lo, hi = ci_mean(vals)
    return Summary(
        n=len(vals),
        mean=mean(vals),
        stdev=stdev(vals),
        minimum=min(vals) if vals else math.nan,
        p50=percentile(vals, 50),
        p95=percentile(vals, 95),
        p99=percentile(vals, 99),
        maximum=max(vals) if vals else math.nan,
        ci_low=lo,
        ci_high=hi,
    )
