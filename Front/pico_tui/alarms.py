"""Alarmes por limiar, definidos pelo operador para cada sensor.

Cada regra é "métrica acima do limite" e vale para um sensor (identificador
lógico ``NN.CC``). A avaliação acontece a cada telemetria recebida
(``StateStore.update_telemetry``): o alarme acende quando o valor passa do
limite e só apaga quando volta para baixo de ``limite − histerese``, para não
oscilar na fronteira. Acender e apagar viram intercorrências do sensor e
mensagens; enquanto aceso, o alarme entra na condição do sensor (atenção) e,
com ela, na faixa de estado, na tela Início e na borda do supervisório. Os
limites ficam nas preferências (``alarm_limits`` em tui.json).
"""
from __future__ import annotations

import math
import time
from collections.abc import Callable
from dataclasses import dataclass

from pico_tui.core.models import ActiveAlarm, TelemetrySample


@dataclass(frozen=True, slots=True)
class AlarmMetric:
    key: str
    label: str
    unit: str
    profile: str
    hysteresis: float          # absoluta, na unidade da métrica
    relative: bool             # True: histerese como fração do limite
    read: Callable[[TelemetrySample], float | None]

    def describe_value(self, value: float) -> str:
        return f"{_number(value)} {self.unit}".strip()


def _acoustic(attribute: str) -> Callable[[TelemetrySample], float | None]:
    def read(sample: TelemetrySample) -> float | None:
        metrics = sample.acoustic
        return getattr(metrics, attribute) if metrics is not None else None

    return read


METRICS: tuple[AlarmMetric, ...] = (
    AlarmMetric("rms", "RMS", "g", "VIBRATION", 0.05, True, lambda sample: sample.rms),
    AlarmMetric("ppv", "PPV", "mm/s", "VIBRATION", 0.05, True, lambda sample: sample.ppv_mm_s),
    AlarmMetric("kurtosis", "Curtose (excesso)", "", "VIBRATION", 0.5, False, lambda sample: sample.kurtosis),
    AlarmMetric("crest", "Fator de crista", "", "VIBRATION", 0.2, False, lambda sample: sample.crest_factor),
    AlarmMetric("laeq", "LAeq do último segundo", "dB(A)", "ACOUSTIC", 1.0, False, _acoustic("laeq_db")),
    AlarmMetric("lzpeak", "Pico sem ponderação", "dB", "ACOUSTIC", 1.0, False, _acoustic("lzpeak_db")),
    AlarmMetric("dose_nho01", "Dose NHO-01", "%", "ACOUSTIC", 0.0, False, _acoustic("dose_nho01_pct")),
    AlarmMetric("dose_nr15", "Dose NR-15", "%", "ACOUSTIC", 0.0, False, _acoustic("dose_nr15_pct")),
    AlarmMetric("anomaly_z", "Escore de anomalia (z)", "", "ACOUSTIC", 0.5, False, _acoustic("anomaly_z")),
)
METRICS_BY_KEY = {metric.key: metric for metric in METRICS}


def metrics_for(profile: str | None) -> list[AlarmMetric]:
    profile = (profile or "").upper()
    return [metric for metric in METRICS if metric.profile == profile]


def _number(value: float) -> str:
    text = f"{value:.3f}".rstrip("0").rstrip(".") if abs(value) < 100 else f"{value:.1f}"
    return text.replace(".", ",")


def clean_limits(limits: dict[str, object]) -> dict[str, float]:
    """Só métricas conhecidas e limites numéricos finitos."""

    cleaned: dict[str, float] = {}
    for key, value in limits.items():
        if key in METRICS_BY_KEY and isinstance(value, (int, float)) and not isinstance(value, bool):
            number = float(value)
            if math.isfinite(number):
                cleaned[key] = number
    return cleaned


def evaluate(
    limits: dict[str, float], active: dict[str, ActiveAlarm], sample: TelemetrySample
) -> tuple[list[ActiveAlarm], list[ActiveAlarm]]:
    """Atualiza ``active`` com a amostra; devolve (acesos agora, apagados agora)."""

    lit: list[ActiveAlarm] = []
    cleared: list[ActiveAlarm] = []
    for key in [key for key in active if key not in limits]:
        cleared.append(active.pop(key))   # regra removida pelo operador
    for key, limit in limits.items():
        metric = METRICS_BY_KEY.get(key)
        if metric is None:
            continue
        value = metric.read(sample)
        if value is None or not math.isfinite(value):
            continue
        unit = sample.rms_unit if key == "rms" and sample.rms_unit else metric.unit
        current = active.get(key)
        if current is None:
            if value > limit:
                alarm = ActiveAlarm(key, metric.label, unit, limit, value, time.time())
                active[key] = alarm
                lit.append(alarm)
            continue
        current.value = value
        current.limit = limit
        margin = abs(limit) * metric.hysteresis if metric.relative else metric.hysteresis
        if value <= limit - margin:
            cleared.append(active.pop(key))
    return lit, cleared


def describe(alarm: ActiveAlarm) -> str:
    unit = f" {alarm.unit}" if alarm.unit else ""
    return f"Alarme: {alarm.label} em {_number(alarm.value)}{unit}, acima do limite de {_number(alarm.limit)}{unit}"


def describe_cleared(alarm: ActiveAlarm) -> str:
    unit = f" {alarm.unit}" if alarm.unit else ""
    return f"Alarme normalizado: {alarm.label} em {_number(alarm.value)}{unit} (limite {_number(alarm.limit)}{unit})"


def summary(limits: dict[str, float]) -> str:
    parts = []
    for key, limit in limits.items():
        metric = METRICS_BY_KEY.get(key)
        if metric is not None:
            parts.append(f"{metric.label} > {metric.describe_value(limit)}")
    return "; ".join(parts) if parts else "Nenhum"
