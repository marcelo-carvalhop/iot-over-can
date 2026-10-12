"""Análises do sensor acústico feitas na TUI.

O firmware entrega, a cada segundo, níveis já integrados (LAeq, LCeq, LZeq,
LAF, pico, bandas de oitava) e os resultados próprios de cada modo. Este
módulo transforma esses números em leituras de operador, sem depender da
interface (é coberto por testes unitários):

* LEVEL - exposição ocupacional segundo a NR-15 (Anexo 1, q = 5) e a NHO-01
  da Fundacentro (q = 3): dose projetada para a jornada, tempo até 100 %,
  LEX,8h, nível médio e o critério de decisão da NHO-01;
* qualquer modo - caráter do ruído: curvas NC (Beranek), interferência na
  conversa (SIL), conteúdo de baixa frequência (LCeq - LAeq), impulsividade,
  variabilidade (L10 - L90) e o caráter espectral;
* MACHINE - "tacômetro acústico": rotação a partir da fundamental, hipótese
  de motor de indução (polos e escorregamento) e zumbido da rede elétrica;
* EVENTS - estatística dos eventos sonoros (classes, taxa, SEL combinado).

As interpretações são indicativas: o INMP441 não é um medidor de nível
sonoro classe 1 ou 2 (IEC 61672), e as conclusões dependem da calibração
(comando CAL ou ``sensor.acoustic_calibrate``).
"""
from __future__ import annotations

import math
import statistics
import time
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field

from pico_tui.core.models import AcousticEvent, AcousticMetrics

REFERENCE_PRESSURE_PA = 20e-6
SHIFT_S = 8 * 3600
NR15_CRITERION_DB = 85.0
NR15_EXCHANGE_DB = 5.0
NHO01_EXCHANGE_DB = 3.0
INTEGRATION_THRESHOLD_DB = 80.0     # abaixo disso a dose não acumula (NR-15 e NHO-01)
MAX_UNPROTECTED_DB = 115.0          # NR-15: exposição não permitida sem proteção adequada
MAINS_HZ = 60.0                     # rede elétrica brasileira

OCTAVE_BANDS_HZ = (31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0)


def band_label(center_hz: float) -> str:
    if center_hz >= 1000:
        value = center_hz / 1000.0
        text = f"{value:g}".replace(".", ",")
        return f"{text} kHz"
    return f"{center_hz:g} Hz".replace(".", ",")


def pa_to_db(pressure_pa: float | None) -> float | None:
    """Pressão eficaz (Pa) em dB re 20 µPa; None para valores não positivos."""

    if pressure_pa is None or not pressure_pa > 0 or math.isinf(pressure_pa):
        return None
    return 20.0 * math.log10(pressure_pa / REFERENCE_PRESSURE_PA)


def energy_sum_db(levels: Iterable[float | None]) -> float | None:
    """Soma energética de níveis em dB (bandas, eventos)."""

    total = sum(10.0 ** (level / 10.0) for level in levels if level is not None)
    return 10.0 * math.log10(total) if total > 0 else None


# ---------------------------------------------------------------------------
# Exposição ocupacional (modo LEVEL)
# ---------------------------------------------------------------------------


def permissible_seconds(level_dba: float | None, exchange_db: float) -> float | None:
    """Tempo máximo diário de exposição ao nível dado.

    T = 8 h / 2^((L - 85) / q). Abaixo de 80 dB(A) a exposição não entra na
    dose (None: sem limite diário); acima de 115 dB(A) a NR-15 não admite
    exposição sem proteção (0 s).
    """

    if level_dba is None:
        return None
    if level_dba < INTEGRATION_THRESHOLD_DB:
        return None
    if level_dba > MAX_UNPROTECTED_DB:
        return 0.0
    return SHIFT_S / 2.0 ** ((level_dba - NR15_CRITERION_DB) / exchange_db)


def projected_dose(dose_pct: float | None, elapsed_s: int | None, shift_s: int = SHIFT_S) -> float | None:
    """Dose ao fim da jornada se a condição medida se mantiver."""

    if dose_pct is None or not elapsed_s or elapsed_s <= 0:
        return None
    return dose_pct * shift_s / elapsed_s


def seconds_to_full_dose(dose_pct: float | None, elapsed_s: int | None) -> float | None:
    """Tempo restante até 100 % de dose, no ritmo atual (0 se já atingida)."""

    if dose_pct is None or not elapsed_s or elapsed_s <= 0 or dose_pct <= 0:
        return None
    if dose_pct >= 100.0:
        return 0.0
    return elapsed_s * (100.0 - dose_pct) / dose_pct


def lex_8h(leq_db: float | None, elapsed_s: int | None) -> float | None:
    """Nível de exposição normalizado para 8 h (NEN da NHO-01), resto da jornada em silêncio."""

    if leq_db is None or not elapsed_s or elapsed_s <= 0:
        return None
    return leq_db + 10.0 * math.log10(elapsed_s / SHIFT_S)


def nr15_average_level(dose_8h_pct: float | None) -> float | None:
    """Nível médio (q = 5) equivalente a uma dose de 8 h: 85 + 16,61·log10(D/100)."""

    if dose_8h_pct is None or dose_8h_pct <= 0:
        return None
    return NR15_CRITERION_DB + NR15_EXCHANGE_DB / math.log10(2.0) * math.log10(dose_8h_pct / 100.0)


@dataclass(frozen=True, slots=True)
class Decision:
    """Critério de julgamento da NHO-01 para a dose diária."""

    level: int          # 0 aceitável .. 3 acima do limite
    label: str
    action: str


NHO01_DECISIONS = (
    (50.0, Decision(0, "Aceitável", "manter a condição e reavaliar periodicamente")),
    (80.0, Decision(1, "Acima do nível de ação", "adotar medidas preventivas")),
    (100.0, Decision(2, "Região de incerteza", "adotar medidas preventivas e corretivas")),
)
NHO01_ABOVE_LIMIT = Decision(3, "Acima do limite de exposição", "adotar medidas corretivas imediatas")


def nho01_decision(dose_8h_pct: float | None) -> Decision | None:
    if dose_8h_pct is None:
        return None
    for limit, decision in NHO01_DECISIONS:
        if dose_8h_pct < limit:
            return decision
    return NHO01_ABOVE_LIMIT if dose_8h_pct > 100.0 else NHO01_DECISIONS[-1][1]


@dataclass(slots=True)
class Exposure:
    elapsed_s: int | None = None
    leq_db: float | None = None
    dose_nr15_pct: float | None = None
    dose_nho01_pct: float | None = None
    projected_nr15_pct: float | None = None
    projected_nho01_pct: float | None = None
    to_full_nr15_s: float | None = None
    to_full_nho01_s: float | None = None
    lex_8h_db: float | None = None
    nr15_lavg_db: float | None = None
    allowed_nr15_s: float | None = None
    allowed_nho01_s: float | None = None
    decision: Decision | None = None
    above_115: bool = False


def exposure(metrics: AcousticMetrics) -> Exposure:
    """Leitura da exposição a partir do relatório LEVEL (dose desde o último reinício)."""

    leq = metrics.leq_t_db if metrics.leq_t_db is not None else metrics.laeq_db
    elapsed = metrics.elapsed_s
    projected_nho = projected_dose(metrics.dose_nho01_pct, elapsed)
    projected_nr15 = projected_dose(metrics.dose_nr15_pct, elapsed)
    return Exposure(
        elapsed_s=elapsed,
        leq_db=leq,
        dose_nr15_pct=metrics.dose_nr15_pct,
        dose_nho01_pct=metrics.dose_nho01_pct,
        projected_nr15_pct=projected_nr15,
        projected_nho01_pct=projected_nho,
        to_full_nr15_s=seconds_to_full_dose(metrics.dose_nr15_pct, elapsed),
        to_full_nho01_s=seconds_to_full_dose(metrics.dose_nho01_pct, elapsed),
        lex_8h_db=lex_8h(leq, elapsed),
        nr15_lavg_db=nr15_average_level(projected_nr15),
        allowed_nr15_s=permissible_seconds(metrics.laeq_db, NR15_EXCHANGE_DB),
        allowed_nho01_s=permissible_seconds(metrics.laeq_db, NHO01_EXCHANGE_DB),
        decision=nho01_decision(projected_nho),
        above_115=(metrics.lafmax_db or 0.0) > MAX_UNPROTECTED_DB or (metrics.laeq_db or 0.0) > MAX_UNPROTECTED_DB,
    )


# ---------------------------------------------------------------------------
# Caráter do ruído (qualquer modo)
# ---------------------------------------------------------------------------

# Curvas NC (Beranek; ANSI S12.2), bandas de 63 Hz a 8 kHz.
NC_BANDS_HZ = (63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0)
NC_CURVES: tuple[tuple[int, tuple[float, ...]], ...] = (
    (15, (47, 36, 29, 22, 17, 14, 12, 11)),
    (20, (51, 40, 33, 26, 22, 19, 17, 16)),
    (25, (54, 44, 37, 31, 27, 24, 22, 21)),
    (30, (57, 48, 41, 35, 31, 29, 28, 27)),
    (35, (60, 52, 45, 40, 36, 34, 33, 32)),
    (40, (64, 56, 50, 45, 41, 39, 38, 37)),
    (45, (67, 60, 54, 49, 46, 44, 43, 42)),
    (50, (71, 64, 58, 54, 51, 49, 48, 47)),
    (55, (74, 67, 62, 58, 56, 54, 53, 52)),
    (60, (77, 71, 67, 63, 61, 59, 58, 57)),
    (65, (80, 75, 71, 68, 66, 64, 63, 62)),
    (70, (83, 79, 75, 72, 71, 70, 69, 68)),
)

NC_GUIDE = (
    (25, "muito silencioso (estúdios, salas de concerto)"),
    (35, "silencioso (dormitórios, salas de aula, escritórios privativos)"),
    (45, "moderado (escritórios abertos, lojas, restaurantes)"),
    (55, "ruidoso (cozinhas, oficinas leves, áreas técnicas)"),
    (1000, "muito ruidoso (áreas industriais)"),
)


def _band_nc(index: int, level: float) -> float:
    values = [curve[1][index] for curve in NC_CURVES]
    ratings = [curve[0] for curve in NC_CURVES]
    if level <= values[0]:
        return float(ratings[0]) - (values[0] - level) * 5.0 / max(1.0, values[1] - values[0])
    for k in range(len(values) - 1):
        if level <= values[k + 1]:
            span = values[k + 1] - values[k]
            return ratings[k] + 5.0 * (level - values[k]) / span if span else float(ratings[k + 1])
    span = values[-1] - values[-2]
    return ratings[-1] + 5.0 * (level - values[-1]) / (span or 1.0)


@dataclass(frozen=True, slots=True)
class NcRating:
    value: int                           # limitado a 70, a última curva tabelada
    governing_hz: float
    guide: str
    missing_hz: tuple[float, ...] = ()   # bandas acima de fs/2, fora da avaliação
    above_table: bool = False            # ruído acima da curva NC-70

    @property
    def label(self) -> str:
        return f"acima de NC-{self.value}" if self.above_table else f"NC-{self.value}"


def nc_rating(octave_db: Sequence[float | None]) -> NcRating | None:
    """Curva NC tangente: a maior entre as bandas de 63 Hz a 8 kHz (interpolada).

    O firmware omite as bandas cujo limite superior passa de fs/2 (a 16 kHz
    de amostragem, a de 8 kHz). A avaliação segue com as demais desde que
    as bandas de 63 Hz a 4 kHz estejam presentes; as omitidas são informadas.
    """

    if len(octave_db) < 8:
        return None
    best: tuple[float, float] | None = None
    missing: list[float] = []
    for index, center in enumerate(NC_BANDS_HZ):
        position = index + 1                   # octave_db[0] é 31,5 Hz
        level = octave_db[position] if position < len(octave_db) else None
        if level is None:
            if center < 8000.0:
                return None
            missing.append(center)
            continue
        rating = _band_nc(index, level)
        if best is None or rating > best[0]:
            best = (rating, center)
    if best is None:
        return None
    top = NC_CURVES[-1][0]
    above = best[0] > top
    value = top if above else max(0, round(best[0]))
    guide = next(text for limit, text in NC_GUIDE if value <= limit)
    return NcRating(value, best[1], guide, tuple(missing), above)


# Beranek: nível de interferência na fala (média das oitavas de 500 Hz a
# 4 kHz) que ainda permite comunicação confiável com voz masculina normal a
# 1 m; cada esforço vocal acima soma 6 dB e cada distância dobrada tira 6 dB.
SIL_NORMAL_VOICE_1M = 57.5
VOICE_EFFORTS = (("em voz normal", 0.0), ("em voz elevada", 6.0), ("em voz muito alta", 12.0), ("aos gritos", 18.0))


def speech_interference_level(octave_db: Sequence[float | None]) -> float | None:
    if len(octave_db) < 8 or any(octave_db[i] is None for i in range(4, 8)):
        return None
    return sum(octave_db[i] for i in range(4, 8)) / 4.0  # type: ignore[misc]


def conversation_distance_m(sil_db: float | None, effort_db: float = 0.0) -> float | None:
    """Distância máxima para conversa confiável no esforço vocal dado."""

    if sil_db is None:
        return None
    return 10.0 ** ((SIL_NORMAL_VOICE_1M + effort_db - sil_db) / 20.0)


def speech_summary(sil_db: float | None) -> str | None:
    if sil_db is None:
        return None
    normal = conversation_distance_m(sil_db) or 0.0
    if normal >= 8.0:
        return "Conversa em voz normal sem restrição prática"
    if normal >= 0.5:
        return f"Conversa em voz normal até cerca de {_meters(normal)}"
    for name, effort in VOICE_EFFORTS[1:]:
        distance = conversation_distance_m(sil_db, effort) or 0.0
        if distance >= 0.5:
            return f"Voz normal não basta além de {_meters(normal)}; {name}, até cerca de {_meters(distance)}"
    return "Comunicação verbal impraticável sem proteção ou afastamento da fonte"


def _meters(value: float) -> str:
    if value < 1.0:
        return f"{value * 100:.0f} cm"
    return f"{_num(value, 1)} m"


def _num(value: float, decimals: int = 1) -> str:
    """Número com vírgula decimal (o texto ao redor não é alterado)."""

    return f"{value:.{decimals}f}".replace(".", ",")


@dataclass(frozen=True, slots=True)
class Insight:
    """Leitura curta para o operador; ``level``: 0 informativo, 1 atenção, 2 crítico."""

    level: int
    title: str
    text: str


def dominant_band(octave_db: Sequence[float | None]) -> tuple[float, float] | None:
    candidates = [(level, OCTAVE_BANDS_HZ[i]) for i, level in enumerate(octave_db[:10]) if level is not None]
    if not candidates:
        return None
    level, center = max(candidates)
    return center, level


def spectral_character(metrics: AcousticMetrics) -> str | None:
    """Caráter espectral a partir da banda dominante, do centroide e da planura."""

    dominant = dominant_band(metrics.octave_db)
    if dominant is None:
        return None
    center, _level = dominant
    if center <= 125:
        region = "graves (ventilação, motores grandes, tráfego pesado)"
    elif center <= 1000:
        region = "médios (voz, motores, máquinas em geral)"
    elif center <= 4000:
        region = "agudos (engrenagens, chiados, alarmes)"
    else:
        region = "muito agudos (escape de ar, atrito, componentes de alta rotação)"
    text = f"Energia concentrada em {band_label(center)}: {region}"
    flatness = metrics.flatness
    centroid = metrics.centroid_hz
    if flatness is not None and centroid is not None and flatness >= 0.4 and centroid >= 3000:
        text += "; ruído de banda larga e agudo, indício de vazamento de ar ou gás"
    elif flatness is not None and flatness < 0.05:
        text += "; espectro dominado por poucos tons"
    return text


def character_insights(metrics: AcousticMetrics) -> list[Insight]:
    """Indicadores válidos em qualquer modo."""

    items: list[Insight] = []
    if metrics.mic_fault:
        items.append(Insight(2, "Microfone", "Sem sinal válido do INMP441: confira alimentação, SCK, WS, SD e o pino L/R"))
    if metrics.overload:
        items.append(Insight(1, "Sobrecarga", "Pressão acima do fundo de escala (~120 dB SPL): níveis e pico subestimados"))
    if metrics.lceq_db is not None and metrics.laeq_db is not None:
        difference = metrics.lceq_db - metrics.laeq_db
        if difference > 20.0:
            items.append(Insight(1, "Baixa frequência",
                                 f"LCeq - LAeq = {_db(difference)}: conteúdo forte de graves, pouco percebido na escala A"))
        elif difference > 10.0:
            items.append(Insight(0, "Baixa frequência", f"LCeq - LAeq = {_db(difference)}: graves presentes"))
    # Pico sobre o nível eficaz, na mesma ponderação (Z): ruído gaussiano fica
    # perto de 12 dB num segundo; impactos passam de 18 dB (fator de crista 8).
    if metrics.lzpeak_db is not None and metrics.lzeq_db is not None:
        impulsive = metrics.lzpeak_db - metrics.lzeq_db
        kurtosis = metrics.kurtosis
        if impulsive > 18.0 or (kurtosis is not None and kurtosis > 3.0):
            items.append(Insight(1, "Impulsividade",
                                 f"pico {_db(impulsive)} acima do nível eficaz: ruído com impactos "
                                 "(avaliar também o pico, NR-15 Anexo 2)"))
    if metrics.l10_db is not None and metrics.l90_db is not None:
        climate = metrics.l10_db - metrics.l90_db
        if climate > 10.0:
            items.append(Insight(0, "Variabilidade",
                                 f"L10 - L90 = {_db(climate)}: nível muito variável (operação intermitente, tráfego)"))
    if (character := spectral_character(metrics)) is not None:
        items.append(Insight(0, "Caráter espectral", character))
    if (nc := nc_rating(metrics.octave_db)) is not None:
        note = ""
        if nc.missing_hz:
            note = " (sem " + ", ".join(band_label(center) for center in nc.missing_hz) + ": acima de fs/2)"
        items.append(Insight(0, "Critério NC",
                             f"{nc.label}, definido pela banda de {band_label(nc.governing_hz)}: {nc.guide}{note}"))
    if (speech := speech_summary(speech_interference_level(metrics.octave_db))) is not None:
        items.append(Insight(0, "Conversa", speech))
    return items


def _db(value: float) -> str:
    return f"{_num(value, 1)} dB"


# ---------------------------------------------------------------------------
# Tacômetro acústico (modo MACHINE)
# ---------------------------------------------------------------------------

def rpm_from_frequency(f0_hz: float | None, events_per_revolution: int = 1) -> float | None:
    if f0_hz is None or f0_hz <= 0 or events_per_revolution <= 0:
        return None
    return 60.0 * f0_hz / events_per_revolution


def mains_component(f0_hz: float | None, line_hz: float = MAINS_HZ, tolerance: float = 0.01) -> int | None:
    """Múltiplo da rede elétrica (1 = 60 Hz, 2 = 120 Hz...) ou None."""

    if f0_hz is None or f0_hz <= 0:
        return None
    for multiple in (1, 2, 3, 4):
        target = multiple * line_hz
        if abs(f0_hz - target) <= tolerance * target:
            return multiple
    return None


@dataclass(frozen=True, slots=True)
class MotorHypothesis:
    poles: int
    synchronous_rpm: float
    slip_pct: float


def induction_motor_hypotheses(rpm: float | None, line_hz: float = MAINS_HZ) -> list[MotorHypothesis]:
    """Motores de indução compatíveis com a rotação (escorregamento de 0,5 % a 8 %)."""

    if rpm is None or rpm <= 0:
        return []
    result = []
    for poles in (2, 4, 6, 8, 10, 12):
        synchronous = 120.0 * line_hz / poles
        slip = 100.0 * (synchronous - rpm) / synchronous
        if 0.5 <= slip <= 8.0:
            result.append(MotorHypothesis(poles, synchronous, slip))
    return result


@dataclass(slots=True)
class Tachometer:
    f0_hz: float | None
    rpm: float | None
    harmonics: int | None
    mains_multiple: int | None
    motors: list[MotorHypothesis] = field(default_factory=list)
    mean_rpm: float | None = None
    spread_pct: float | None = None


def tachometer(metrics: AcousticMetrics, f0_history: Sequence[float] = ()) -> Tachometer:
    f0 = metrics.f0_hz if metrics.f0_hz and metrics.f0_hz > 0 else None
    rpm = rpm_from_frequency(f0)
    mains = mains_component(f0)
    history = [value for value in f0_history if value and value > 0]
    mean_rpm = spread = None
    if len(history) >= 3:
        mean_f0 = statistics.fmean(history)
        mean_rpm = 60.0 * mean_f0
        spread = 100.0 * statistics.pstdev(history) / mean_f0 if mean_f0 else None
    return Tachometer(
        f0_hz=f0,
        rpm=rpm,
        harmonics=metrics.harmonics,
        mains_multiple=mains,
        motors=[] if mains else induction_motor_hypotheses(rpm),
        mean_rpm=mean_rpm,
        spread_pct=spread,
    )


def machine_insights(metrics: AcousticMetrics, threshold_z: float | None, f0_history: Sequence[float] = ()) -> list[Insight]:
    items: list[Insight] = []
    tach = tachometer(metrics, f0_history)
    if tach.f0_hz is None:
        items.append(Insight(0, "Tacômetro", "Sem série harmônica estável: fundamental não identificada"))
    elif tach.mains_multiple:
        frequency = tach.mains_multiple * MAINS_HZ
        items.append(Insight(0, "Tacômetro",
                             f"Fundamental em {frequency:.0f} Hz: zumbido da rede elétrica "
                             "(transformador, reator ou motor; magnetostrição em 120 Hz)"))
    else:
        text = f"{_rpm(tach.rpm)} se a fundamental for a rotação (1 evento por volta)"
        if tach.motors:
            options = " ou ".join(f"{m.poles} polos (escorregamento de {_num(m.slip_pct)} %)" for m in tach.motors)
            text += f"; compatível com motor de indução de {options}"
        items.append(Insight(0, "Tacômetro", text))
        if tach.spread_pct is not None and tach.spread_pct > 2.0:
            items.append(Insight(1, "Rotação",
                                 f"Variação de {_num(tach.spread_pct)} % na fundamental recente: carga ou velocidade oscilando"))
    if metrics.tonal_db is not None:
        if metrics.tonal_db >= 10.0:
            items.append(Insight(0, "Tom", f"Componente dominante {_db(metrics.tonal_db)} acima do ruído vizinho: tom proeminente"))
        elif metrics.tonal_db >= 5.0:
            items.append(Insight(0, "Tom", f"Componente dominante {_db(metrics.tonal_db)} acima do ruído vizinho: tom perceptível"))
    baseline = (metrics.baseline or "NONE").upper()
    if baseline == "LEARNING":
        items.append(Insight(0, "Referência", f"Aprendendo a assinatura normal ({metrics.baseline_pct or 0} %)"))
    elif baseline == "NONE":
        items.append(Insight(0, "Referência", "Sem assinatura de referência: use \"Aprender referência\" com a máquina em operação normal"))
    elif metrics.anomaly_z is not None:
        limit = threshold_z if threshold_z and threshold_z > 1 else 4.0
        z = metrics.anomaly_z
        if metrics.anomaly is True or z >= limit:
            items.append(Insight(2, "Anomalia",
                                 f"Escore {_num(z)} acima do limiar {_num(limit)}: assinatura sonora fora do normal"))
        elif z >= 0.6 * limit:
            items.append(Insight(1, "Anomalia", f"Escore {_num(z)} se aproximando do limiar {_num(limit)}"))
        else:
            items.append(Insight(0, "Anomalia", f"Escore {_num(z)}: compatível com a referência"))
    return items


def _rpm(value: float | None) -> str:
    if value is None:
        return "—"
    return f"{value:,.0f} rpm".replace(",", ".")


# ---------------------------------------------------------------------------
# Eventos sonoros (modo EVENTS)
# ---------------------------------------------------------------------------

EVENT_CLASSES = {
    "IMPACT": ("Impacto", "curto e impulsivo, graves e médios: batida, queda de peça"),
    "CRACK": ("Estalo", "curto e agudo: quebra, estalo, faísca"),
    "TONAL": ("Tonal", "longo e tonal: alarme, apito, sirene"),
    "TRANSIENT": ("Transiente", "longo e de banda larga: passagem de veículo, partida de máquina"),
}


def event_label(event_class: str | None) -> str:
    return EVENT_CLASSES.get((event_class or "").upper(), ("Não classificado", ""))[0]


def event_help(event_class: str | None) -> str:
    return EVENT_CLASSES.get((event_class or "").upper(), ("", ""))[1]


@dataclass(slots=True)
class EventStats:
    total: int = 0
    per_class: dict[str, int] = field(default_factory=dict)
    rate_per_min: float | None = None
    loudest: AcousticEvent | None = None
    combined_sel_db: float | None = None


def event_statistics(events: Iterable[AcousticEvent], *, now: float | None = None, window_s: float = 600.0) -> EventStats:
    """Contagem por classe, taxa nos últimos ``window_s`` segundos e SEL combinado."""

    now = time.time() if now is None else now
    items = list(events)
    stats = EventStats()
    recent = 0
    for event in items:
        stats.total += event.count
        key = (event.event_class or "UNKNOWN").upper()
        stats.per_class[key] = stats.per_class.get(key, 0) + event.count
        if now - event.received_wall_time <= window_s:
            recent += event.count
        if event.peak_db is not None and (stats.loudest is None or (stats.loudest.peak_db or -1e9) < event.peak_db):
            stats.loudest = event
    if items:
        span = min(window_s, max(60.0, now - min(event.received_wall_time for event in items)))
        stats.rate_per_min = 60.0 * recent / span
    stats.combined_sel_db = energy_sum_db(event.sel_db for event in items)
    return stats


# ---------------------------------------------------------------------------
# Calibração com calibrador acústico
# ---------------------------------------------------------------------------

CALIBRATION_MAX_SPREAD_DB = 0.5


@dataclass(frozen=True, slots=True)
class Calibration:
    measured_db: float
    spread_db: float
    new_gain: float

    @property
    def stable(self) -> bool:
        return self.spread_db <= CALIBRATION_MAX_SPREAD_DB


def calibration(readings_db: Sequence[float], reference_db: float, current_gain: float) -> Calibration | None:
    """Ganho novo a partir de leituras LZeq com o calibrador (1 kHz: A, C e Z coincidem).

    O ganho multiplica a pressão: novo = atual · 10^((referência - medido) / 20).
    """

    values = [value for value in readings_db if value is not None]
    if len(values) < 3 or not current_gain or current_gain <= 0:
        return None
    measured = statistics.fmean(values)
    spread = max(values) - min(values)
    return Calibration(measured, spread, current_gain * 10.0 ** ((reference_db - measured) / 20.0))


def level_insights(metrics: AcousticMetrics) -> list[Insight]:
    items: list[Insight] = []
    result = exposure(metrics)
    if result.above_115:
        items.append(Insight(2, "Exposição", "Nível acima de 115 dB(A): exposição não permitida sem proteção (NR-15)"))
    if result.decision is not None and result.projected_nho01_pct is not None:
        level = 2 if result.decision.level >= 3 else (1 if result.decision.level >= 1 else 0)
        items.append(
            Insight(
                level,
                "NHO-01",
                f"{result.decision.label}: dose projetada de {_num(result.projected_nho01_pct, 0)} % em 8 h; "
                f"{result.decision.action}",
            )
        )
    if result.to_full_nho01_s is not None and 0 < result.to_full_nho01_s < SHIFT_S:
        items.append(Insight(1, "Dose", f"100 % de dose (NHO-01) em {duration(result.to_full_nho01_s)} no ritmo atual"))
    # A tabela da NR-15 começa em 85 dB(A) (8 h); abaixo disso não há limite diário.
    laeq = metrics.laeq_db
    if laeq is not None and laeq >= NR15_CRITERION_DB and result.allowed_nr15_s:
        items.append(
            Insight(1, "NR-15", f"No nível deste segundo ({_db(laeq)}(A)) o limite diário seria {duration(result.allowed_nr15_s)}")
        )
    return items


def duration(seconds: float | None) -> str:
    if seconds is None:
        return "—"
    seconds = max(0.0, seconds)
    if seconds < 60:
        return f"{seconds:.0f} s"
    if seconds < 3600:
        return f"{seconds / 60:.0f} min"
    hours, rest = divmod(round(seconds), 3600)
    minutes = rest // 60
    return f"{hours} h {minutes:02d} min" if minutes else f"{hours} h"


def bar_cells(level_db: float | None, floor_db: float, top_db: float, width: int) -> int:
    """Número de células de uma barra horizontal entre ``floor_db`` e ``top_db``."""

    if level_db is None or width <= 0 or top_db <= floor_db:
        return 0
    fraction = (level_db - floor_db) / (top_db - floor_db)
    return max(0, min(width, round(fraction * width)))
