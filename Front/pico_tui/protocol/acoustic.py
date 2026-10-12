"""Telemetria do sensor acústico (INMP441, perfil ACOUSTIC).

As duas origens usam as mesmas chaves: o console USB do sensor
(``TEL PROFILE=ACOUSTIC MODE=...``) e a Probe 00, que remonta o relatório de
108 bytes recebido pelo CAN (``TEL NODE=.. CHILD=.. PROFILE=ACOUSTIC ...``).
Uma chave ausente significa "não calculado neste modo".
"""
from __future__ import annotations

from pico_tui.core.models import (
    AcousticMetrics,
    AcquisitionMode,
    BatteryInfo,
    DataQuality,
    SensorMode,
    TelemetrySample,
)
from pico_tui.protocol.common import first, parse_bool, parse_float, parse_int

OCTAVE_BANDS_HZ = (31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0)

_ACOUSTIC_KEYS = ("LAEQ_DB", "LZEQ_DB", "OCT_DB")


def is_acoustic_payload(payload: dict[str, str]) -> bool:
    """Linha TEL do sensor acústico (perfil explícito ou chaves de nível sonoro)."""

    profile = str(payload.get("PROFILE", "")).upper()
    if profile:
        return profile == "ACOUSTIC"
    return any(key in payload for key in _ACOUSTIC_KEYS) or str(payload.get("ACQ", "")).upper() == "I2S"


def parse_octaves(text: str | None) -> list[float | None]:
    """``OCT_DB=40.0,42.5,-,...``: um valor por banda, ``-`` quando indisponível."""

    if not text:
        return []
    values: list[float | None] = []
    for token in str(text).split(","):
        token = token.strip()
        values.append(None if token in {"", "-"} else parse_float(token))
    return values


def parse_acoustic_metrics(payload: dict[str, str]) -> AcousticMetrics:
    baseline = payload.get("BASELINE")
    event_class = payload.get("EVENT_CLASS")
    return AcousticMetrics(
        interval_ms=parse_int(payload.get("INTERVAL_MS")),
        coverage_pct=parse_int(payload.get("COVERAGE_PCT")),
        dsp_load_pct=parse_int(payload.get("CPU_PCT")),
        channel=payload.get("CHANNEL"),
        spectrum_valid=parse_bool(payload.get("SPECTRUM_VALID")),
        overload=parse_bool(payload.get("OVERLOAD")),
        mic_fault=parse_bool(payload.get("MIC_FAULT")),
        laeq_db=parse_float(payload.get("LAEQ_DB")),
        lceq_db=parse_float(payload.get("LCEQ_DB")),
        lzeq_db=parse_float(payload.get("LZEQ_DB")),
        lzpeak_db=parse_float(payload.get("LZPEAK_DB")),
        laf_db=parse_float(payload.get("LAF_DB")),
        lafmax_db=parse_float(payload.get("LAFMAX_DB")),
        lafmin_db=parse_float(payload.get("LAFMIN_DB")),
        crest=parse_float(payload.get("CREST")),
        kurtosis=parse_float(payload.get("KURT")),
        peak_hz=parse_float(payload.get("PEAK_HZ")),
        centroid_hz=parse_float(payload.get("CENTROID_HZ")),
        flatness=parse_float(payload.get("FLATNESS")),
        octave_db=parse_octaves(payload.get("OCT_DB")),
        leq_t_db=parse_float(payload.get("LEQ_T_DB")),
        l10_db=parse_float(payload.get("L10_DB")),
        l50_db=parse_float(payload.get("L50_DB")),
        l90_db=parse_float(payload.get("L90_DB")),
        dose_nr15_pct=parse_float(payload.get("DOSE_NR15_PCT")),
        dose_nho01_pct=parse_float(payload.get("DOSE_NHO01_PCT")),
        elapsed_s=parse_int(payload.get("ELAPSED_S")),
        dose_limit=parse_bool(payload.get("DOSE_LIMIT")),
        tonal_db=parse_float(payload.get("TONAL_DB")),
        f0_hz=parse_float(payload.get("F0_HZ")),
        harmonics=parse_int(payload.get("HARMONICS")),
        anomaly_z=parse_float(payload.get("ANOMALY_Z")),
        baseline=str(baseline).upper() if baseline else None,
        baseline_pct=parse_int(payload.get("BASELINE_PCT")),
        anomaly=parse_bool(payload.get("ANOMALY")),
        events_total=parse_int(payload.get("EVENTS")),
        events_interval=parse_int(payload.get("EVENTS_INTERVAL")),
        event_active=parse_bool(payload.get("EVENT_ACTIVE")),
        sta_lta_db=parse_float(payload.get("STA_LTA_DB")),
        background_db=parse_float(payload.get("BACKGROUND_DB")),
        event_class=str(event_class).upper() if event_class else None,
        event_peak_db=parse_float(payload.get("EVENT_PEAK_DB")),
        event_sel_db=parse_float(payload.get("EVENT_SEL_DB")),
        event_duration_ms=parse_int(payload.get("EVENT_DURATION_MS")),
        event_centroid_hz=parse_float(payload.get("EVENT_CENTROID_HZ")),
        event_age_s=parse_int(payload.get("EVENT_AGE_S")),
    )


def acoustic_telemetry_sample(
    parent_node_id: int,
    child_id: int,
    payload: dict[str, str],
    *,
    battery: BatteryInfo,
    sample_rate_requested_hz: float | None = None,
    window_type: str | None = None,
) -> TelemetrySample:
    """Amostra de telemetria do perfil acústico.

    Campos genéricos (modo, taxa, janela, pico espectral, crista, curtose,
    DTC) são preenchidos para as telas e exportações comuns; o restante fica
    em ``acoustic``. Saturação e gatilho STA/LTA do acelerômetro não se
    aplicam e permanecem vazios: a sobrecarga do microfone é
    ``acoustic.overload``.
    """

    metrics = parse_acoustic_metrics(payload)
    degraded = metrics.overload is True or metrics.mic_fault is True
    acquisition = str(payload.get("ACQ", "I2S")).upper()
    return TelemetrySample(
        parent_node_id=parent_node_id,
        child_id=child_id,
        sensor_timestamp_ms=parse_int(payload.get("TS_MS")),
        gateway_timestamp_ms=parse_int(first(payload, "GW_TS_MS", "GATEWAY_TS_MS")),
        sequence=parse_int(first(payload, "SEQ", "SEQUENCE")),
        mode=_mode(payload.get("MODE")),
        acquisition_mode=AcquisitionMode.I2S if acquisition == "I2S" else _acquisition(acquisition),
        sample_rate_requested_hz=sample_rate_requested_hz,
        sample_rate_effective_hz=parse_float(first(payload, "RATE_EFF_HZ", "RATE_HZ")),
        window_type=window_type,
        window_size=parse_int(first(payload, "WIN", "FFT_SIZE", "WINDOW_SIZE")),
        # A falta de espectro em um segundo isolado (modo EVENTS sob carga)
        # não invalida o último espectro pedido: só um "sim" é informativo.
        fft_valid=True if metrics.spectrum_valid else None,
        kurtosis=metrics.kurtosis,
        crest_factor=metrics.crest,
        peak_frequency_hz=metrics.peak_hz if metrics.spectrum_valid is not False else None,
        dtc_code=parse_int(first(payload, "DTC", "DTC_CODE")),
        dtc_count=parse_int(first(payload, "DTC_COUNT", "ACTIVE_DTC_COUNT")),
        quality=DataQuality.DEGRADED if degraded else DataQuality.REAL,
        battery=battery,
        raw=payload,
        acoustic=metrics,
    )


def _mode(value: object) -> SensorMode:
    try:
        return SensorMode(str(value or "UNKNOWN").upper())
    except ValueError:
        return SensorMode.UNKNOWN


def _acquisition(value: str) -> AcquisitionMode:
    try:
        return AcquisitionMode(value)
    except ValueError:
        return AcquisitionMode.UNKNOWN
