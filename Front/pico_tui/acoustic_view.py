"""Conteúdo das telas para o sensor acústico (INMP441).

Monta rótulos, valores e textos a partir de ``AcousticMetrics`` e das
análises de ``pico_tui.acoustics``; não depende do Textual. A tela do
sensor (``SensorScreen``) só posiciona o que este módulo produz.
"""
from __future__ import annotations

import math

from rich.text import Text

from pico_tui import acoustics, palette
from pico_tui import presentation as pres
from pico_tui.core.models import AcousticMetrics, SensorNode

Field = tuple[str, "Text | str"]

_INSIGHT_LEVELS = {0: pres.Level.NORMAL, 1: pres.Level.ATTENTION, 2: pres.Level.CRITICAL}
_LEVEL_COLORS = {
    pres.Level.NO_DATA: palette.STATE_OFFLINE,
    pres.Level.NORMAL: palette.STATE_OK,
    pres.Level.ATTENTION: palette.STATE_WARNING,
    pres.Level.CRITICAL: palette.STATE_CRITICAL,
}

MODE_TITLES = {
    "LEVEL": "Exposição ao ruído (NR-15 e NHO-01)",
    "MACHINE": "Diagnóstico de máquina",
    "EVENTS": "Eventos sonoros",
    "IDLE": "Modo ocioso",
}


def _db(value: float | None, weighting: str = "", decimals: int = 1) -> str:
    if value is None:
        return pres.MISSING
    unit = f"dB({weighting})" if weighting else "dB"
    return pres.fmt_number(value, decimals, unit)


def _pct(value: float | None, decimals: int = 1) -> str:
    return pres.fmt_number(value, decimals, "%")


def _bold(text: str, color: str = palette.TEXT_PRIMARY) -> Text:
    return Text(text, style=f"bold {color}")


def _flag(value: bool | None, yes: str, no: str = "Não", *, level: pres.Level = pres.Level.ATTENTION) -> Text:
    if value is None:
        return Text(pres.MISSING, style=palette.TEXT_PRIMARY)
    if value:
        return Text(f"{pres.symbol(level)} {yes}", style=f"bold {_LEVEL_COLORS[level]}")
    return Text(no, style=palette.TEXT_PRIMARY)


def latest(sensor: SensorNode) -> AcousticMetrics | None:
    sample = sensor.latest_telemetry
    return sample.acoustic if sample is not None else None


# ---------------------------------------------------------------------------
# Níveis (todos os modos)
# ---------------------------------------------------------------------------


def level_fields(metrics: AcousticMetrics) -> list[Field]:
    rows: list[Field] = [
        ("LAeq (último segundo)", _bold(_db(metrics.laeq_db, "A", 2))),
        ("LCeq / LZeq", f"{_db(metrics.lceq_db, 'C')} / {_db(metrics.lzeq_db, 'Z')}"),
        (
            "LAF agora (máx. / mín.)",
            f"{_db(metrics.laf_db, 'A')} ({pres.fmt_number(metrics.lafmax_db, 1)} / {pres.fmt_number(metrics.lafmin_db, 1)})",
        ),
        ("Pico (sem ponderação)", _db(metrics.lzpeak_db)),
        ("Crista / curtose", f"{pres.fmt_number(metrics.crest, 2)} / {pres.fmt_number(metrics.kurtosis, 2)} (excesso)"),
    ]
    if metrics.spectrum_valid is not False:
        rows += [
            ("Frequência dominante", pres.fmt_number(metrics.peak_hz, 1, "Hz")),
            ("Centroide espectral", pres.fmt_number(metrics.centroid_hz, 0, "Hz")),
            ("Planura espectral", f"{pres.fmt_number(metrics.flatness, 3)} (0 tonal, 1 ruído branco)"),
        ]
    rows += [
        (
            "Cobertura da FFT / DSP",
            f"{_pct(metrics.coverage_pct, 0)} do áudio / núcleo de DSP {_pct(metrics.dsp_load_pct, 0)} ocupado",
        ),
        ("Sobrecarga do microfone", _flag(metrics.overload, "Sim, acima de ~120 dB SPL")),
    ]
    return rows


def octave_lines(metrics: AcousticMetrics, width: int) -> Text:
    """Barras horizontais das bandas de oitava (sem ponderação)."""

    bands = list(metrics.octave_db[:10]) + [None] * max(0, 10 - len(metrics.octave_db))
    present = [level for level in bands if level is not None]
    if not present:
        return Text("Bandas de oitava indisponíveis neste relatório.", style=palette.TEXT_MUTED)
    top = math.ceil(max(present) / 10.0) * 10.0
    floor = top - 50.0
    label_width = 8
    value_width = 9
    bar_width = max(4, width - label_width - value_width - 2)
    dominant = max(range(len(bands)), key=lambda i: bands[i] if bands[i] is not None else -1e9)
    text = Text()
    for index, (center, level) in enumerate(zip(acoustics.OCTAVE_BANDS_HZ, bands, strict=False)):
        if index:
            text.append("\n")
        text.append(f"{acoustics.band_label(center):>{label_width}} ", style=palette.TEXT_MUTED)
        if level is None:
            text.append("acima de fs/2", style=palette.TEXT_DIM)
            continue
        cells = acoustics.bar_cells(level, floor, top, bar_width)
        color = palette.ACCENT_FOCUS if index == dominant else palette.TEXT_SECONDARY
        text.append("█" * cells, style=color)
        text.append("·" * (bar_width - cells), style=palette.TEXT_DIM)
        text.append(f" {pres.fmt_number(level, 1, 'dB'):>{value_width - 1}}", style=palette.TEXT_PRIMARY)
    return text


# ---------------------------------------------------------------------------
# Painel do modo
# ---------------------------------------------------------------------------


def mode_title(mode: str) -> str:
    return MODE_TITLES.get((mode or "").upper(), "Medição acústica")


def mode_fields(sensor: SensorNode, metrics: AcousticMetrics) -> list[Field]:
    mode = sensor.sensor_mode.value
    if mode == "LEVEL":
        return _level_mode_fields(metrics)
    if mode == "MACHINE":
        return _machine_fields(sensor, metrics)
    if mode == "EVENTS":
        return _event_fields(sensor, metrics)
    return [("Medição", "Microfone em repouso: sem relógio no INMP441 e sem relatórios")]


def _decision_text(decision: acoustics.Decision | None) -> Text | str:
    if decision is None:
        return pres.MISSING
    level = {0: pres.Level.NORMAL, 1: pres.Level.ATTENTION, 2: pres.Level.ATTENTION, 3: pres.Level.CRITICAL}[decision.level]
    text = Text(f"{pres.symbol(level)} {decision.label}", style=f"bold {_LEVEL_COLORS[level]}")
    text.append(f": {decision.action}", style=palette.TEXT_PRIMARY)
    return text


def _level_mode_fields(metrics: AcousticMetrics) -> list[Field]:
    exposure = acoustics.exposure(metrics)
    ln = " / ".join(pres.fmt_number(value, 1) for value in (metrics.l10_db, metrics.l50_db, metrics.l90_db))
    return [
        ("LAeq,T desde o reinício", _bold(_db(metrics.leq_t_db, "A"))),
        ("Duração da medição", acoustics.duration(metrics.elapsed_s)),
        ("L10 / L50 / L90", f"{ln} dB(A)"),
        (
            "Dose NR-15 (q = 5)",
            f"{_pct(metrics.dose_nr15_pct, 2)} (projeção para 8 h: {_pct(exposure.projected_nr15_pct, 0)})",
        ),
        (
            "Dose NHO-01 (q = 3)",
            f"{_pct(metrics.dose_nho01_pct, 2)} (projeção para 8 h: {_pct(exposure.projected_nho01_pct, 0)})",
        ),
        ("Nível médio NR-15 (8 h)", _db(exposure.nr15_lavg_db, "A")),
        ("LEX,8h / NEN (NHO-01)", f"{_db(exposure.lex_8h_db, 'A')} com o resto da jornada em silêncio"),
        ("Tempo até 100 % (NHO-01)", acoustics.duration(exposure.to_full_nho01_s) if exposure.to_full_nho01_s else pres.MISSING),
        ("Critério da NHO-01", _decision_text(exposure.decision)),
        ("Limite atingido", _flag(metrics.dose_limit, "Sim: dose de 100 % ou nível acima de 115 dB(A)")),
    ]


def f0_history(sensor: SensorNode, count: int = 30) -> list[float]:
    return [
        sample.acoustic.f0_hz
        for sample in list(sensor.telemetry_history)[-count:]
        if sample.acoustic is not None and sample.acoustic.f0_hz
    ]


def f0_limits(sensor: SensorNode) -> tuple[float, float] | None:
    cfg = sensor.configuration
    return acoustics.f0_range(cfg.sample_rate_effective_hz or cfg.sample_rate_requested_hz, cfg.window_size)


def _machine_fields(sensor: SensorNode, metrics: AcousticMetrics) -> list[Field]:
    tach = acoustics.tachometer(metrics, f0_history(sensor))
    limits = f0_limits(sensor)
    if limits is None:
        search = pres.MISSING
    else:
        search = (
            f"f0 de {pres.fmt_number(limits[0], 1, 'Hz')} a {pres.fmt_number(limits[1], 0, 'Hz')} "
            f"({pres.fmt_int(round(60 * limits[0]))} a {pres.fmt_int(round(60 * limits[1]))} rpm a 1 evento por volta)"
        )
    threshold = sensor.configuration.stalta_threshold
    baseline = (metrics.baseline or "NONE").upper()
    baseline_text = {
        "READY": "Pronta",
        "LEARNING": f"Aprendendo ({metrics.baseline_pct or 0} %)",
        "NONE": "Não aprendida",
    }.get(baseline, baseline.title())
    if metrics.anomaly_z is None:
        score: Text | str = pres.MISSING
    else:
        limit = threshold if threshold and threshold > 1 else 4.0
        level = pres.Level.CRITICAL if (metrics.anomaly or metrics.anomaly_z >= limit) else (
            pres.Level.ATTENTION if metrics.anomaly_z >= 0.6 * limit else pres.Level.NORMAL
        )
        score = Text(f"{pres.symbol(level)} {pres.fmt_number(metrics.anomaly_z, 2)}", style=f"bold {_LEVEL_COLORS[level]}")
        score.append(f" (limiar {pres.fmt_number(limit, 1)})", style=palette.TEXT_PRIMARY)
    if tach.rpm is None:
        rotation = "Sem série harmônica estável"
    elif tach.mains_multiple:
        rotation = f"Zumbido da rede ({tach.mains_multiple * acoustics.MAINS_HZ:.0f} Hz), não é rotação"
    else:
        rotation = f"{pres.fmt_int(round(tach.rpm))} rpm (1 evento por volta)"
        if tach.mean_rpm is not None and tach.spread_pct is not None:
            rotation += f"; média {pres.fmt_int(round(tach.mean_rpm))} rpm ± {pres.fmt_number(tach.spread_pct, 1, '%')}"
    motors = "; ".join(
        f"{motor.poles} polos (síncrona {pres.fmt_int(round(motor.synchronous_rpm))} rpm, escorregamento "
        f"{pres.fmt_number(motor.slip_pct, 1, '%')})"
        for motor in tach.motors
    )
    return [
        ("Fundamental (f0)", _bold(pres.fmt_number(tach.f0_hz, 2, "Hz"))),
        ("Faixa do tacômetro", search),
        ("Rotação estimada", rotation),
        ("Hipótese de motor", motors or pres.MISSING),
        ("Harmônicos na série", pres.fmt_int(metrics.harmonics)),
        ("Proeminência tonal", _db(metrics.tonal_db)),
        ("Assinatura de referência", baseline_text),
        ("Escore de anomalia (z)", score),
    ]


def _event_fields(sensor: SensorNode, metrics: AcousticMetrics) -> list[Field]:
    stats = acoustics.event_statistics(sensor.acoustic_events)
    last = pres.MISSING
    if metrics.event_class and metrics.event_age_s is not None:
        details = [f"pico {_db(metrics.event_peak_db)}", f"SEL {_db(metrics.event_sel_db)}"]
        if metrics.event_duration_ms is not None:
            details.append(f"{metrics.event_duration_ms} ms")
        if metrics.event_centroid_hz:
            details.append(pres.fmt_number(metrics.event_centroid_hz, 0, "Hz"))
        last = f"{acoustics.event_label(metrics.event_class)} há {acoustics.duration(metrics.event_age_s)}: {', '.join(details)}"
    loudest = pres.MISSING
    if stats.loudest is not None:
        loudest = (
            f"{acoustics.event_label(stats.loudest.event_class)}, pico {_db(stats.loudest.peak_db)} "
            f"às {pres.fmt_clock(stats.loudest.received_wall_time)}"
        )
    return [
        ("Eventos desde o reinício", _bold(pres.fmt_int(metrics.events_total))),
        ("Evento em curso", _flag(metrics.event_active, "Sim", level=pres.Level.NORMAL)),
        ("Razão STA/LTA", f"{_db(metrics.sta_lta_db)} (limiar {pres.fmt_number(_threshold_db(sensor), 1, 'dB')})"),
        ("Ruído de fundo (LTA)", _db(metrics.background_db, "A")),
        ("Último evento", last),
        ("Taxa recente", pres.fmt_number(stats.rate_per_min, 1, "por minuto") if stats.rate_per_min is not None else pres.MISSING),
        ("Mais forte da sessão", loudest),
        ("SEL combinado da sessão", _db(stats.combined_sel_db)),
    ]


def _threshold_db(sensor: SensorNode) -> float | None:
    ratio = sensor.configuration.stalta_threshold
    return 10.0 * math.log10(ratio) if ratio and ratio > 0 else None


def mode_trend(sensor: SensorNode, count: int = 120) -> tuple[str, list[float]]:
    history = [sample.acoustic for sample in list(sensor.telemetry_history)[-count:] if sample.acoustic is not None]
    mode = sensor.sensor_mode.value
    if mode == "LEVEL":
        return "Dose NHO-01 acumulada (%)", [m.dose_nho01_pct for m in history if m.dose_nho01_pct is not None]
    if mode == "MACHINE":
        return "Escore de anomalia (z)", [m.anomaly_z for m in history if m.anomaly_z is not None]
    if mode == "EVENTS":
        return "Razão STA/LTA (dB)", [m.sta_lta_db for m in history if m.sta_lta_db is not None]
    return "", []


def laeq_trend(sensor: SensorNode, count: int = 120) -> list[float]:
    return [
        sample.acoustic.laeq_db
        for sample in list(sensor.telemetry_history)[-count:]
        if sample.acoustic is not None and sample.acoustic.laeq_db is not None
    ]


# ---------------------------------------------------------------------------
# Leitura do ambiente e eventos
# ---------------------------------------------------------------------------


def insights(sensor: SensorNode, metrics: AcousticMetrics) -> list[acoustics.Insight]:
    mode = sensor.sensor_mode.value
    items: list[acoustics.Insight] = []
    if mode == "LEVEL":
        items += acoustics.level_insights(metrics)
    elif mode == "MACHINE":
        items += acoustics.machine_insights(
            metrics, sensor.configuration.stalta_threshold, f0_history(sensor), f0_limits(sensor)
        )
    items += acoustics.character_insights(metrics)
    items.sort(key=lambda item: -item.level)
    return items


def insight_items(sensor: SensorNode, metrics: AcousticMetrics) -> list[Text]:
    result = []
    for item in insights(sensor, metrics):
        level = _INSIGHT_LEVELS[item.level]
        color = _LEVEL_COLORS[level] if item.level else palette.STATE_INFO
        line = Text(f"{pres.symbol(level)} ", style=color)
        line.append(item.title, style=f"bold {palette.TEXT_PRIMARY}")
        line.append(f"  {item.text}", style=palette.TEXT_PRIMARY)
        result.append(line)
    return result


def event_items(sensor: SensorNode, limit: int = 10) -> list[Text]:
    items = []
    for event in list(sensor.acoustic_events)[::-1][:limit]:
        line = Text(pres.fmt_clock(event.received_wall_time), style=palette.TEXT_DIM)
        line.append(f"  {acoustics.event_label(event.event_class)}", style=f"bold {palette.TEXT_PRIMARY}")
        details = [f"pico {_db(event.peak_db)}", f"SEL {_db(event.sel_db)}"]
        if event.duration_ms is not None:
            details.append(f"{event.duration_ms} ms")
        if event.centroid_hz:
            details.append(pres.fmt_number(event.centroid_hz, 0, "Hz"))
        line.append(f"  {', '.join(details)}", style=palette.TEXT_PRIMARY)
        help_text = acoustics.event_help(event.event_class)
        if help_text:
            line.append(f"  ({help_text})", style=palette.TEXT_MUTED)
        items.append(line)
    return items


# ---------------------------------------------------------------------------
# Situação, configuração e resumo
# ---------------------------------------------------------------------------


def microphone_text(sensor: SensorNode) -> Text | str:
    metrics = latest(sensor)
    health = sensor.health
    channel = (health.audio_channel or (metrics.channel if metrics else None) or "").upper()
    channel_text = {"LEFT": "canal esquerdo (L/R em GND)", "RIGHT": "canal direito (L/R em VDD)"}.get(channel, "")
    if metrics is not None and metrics.mic_fault:
        return Text(f"{pres.symbol(pres.Level.CRITICAL)} Sem sinal válido", style=f"bold {palette.STATE_CRITICAL}")
    if metrics is not None and metrics.overload:
        return Text(f"{pres.symbol(pres.Level.ATTENTION)} Em sobrecarga", style=f"bold {palette.STATE_WARNING}")
    state = (health.microphone_state or "").upper()
    if state and state != "OK":
        return Text(f"{pres.symbol(pres.Level.ATTENTION)} {state.title()}", style=f"bold {palette.STATE_WARNING}")
    if metrics is None and not state:
        return pres.MISSING
    return "Sinal presente" + (f", {channel_text}" if channel_text else "")


def platform_text(sensor: SensorNode) -> str:
    chip = (sensor.health.chip or "").upper()
    board = sensor.health.board or ""
    names = {"RP2040": "Raspberry Pi Pico W (RP2040)", "RP2350": "Raspberry Pi Pico 2 W (RP2350)"}
    if chip in names:
        return names[chip]
    return board or pres.MISSING


def config_fields(sensor: SensorNode) -> list[Field]:
    cfg = sensor.configuration
    gain_db = 20.0 * math.log10(cfg.calibration_gain) if cfg.calibration_gain and cfg.calibration_gain > 0 else None
    threshold = pres.fmt_number(cfg.stalta_threshold, 2)
    if cfg.stalta_threshold is not None:
        threshold += {
            "EVENTS": f" (razão STA/LTA, {pres.fmt_number(10.0 * math.log10(cfg.stalta_threshold), 1, 'dB')})",
            "MACHINE": " (escore z de anomalia)",
        }.get(cfg.mode.value, " (STA/LTA em Eventos, escore z em Máquina)")
    return [
        ("Modo", pres.sensor_mode_label(cfg.mode.value)),
        ("Taxa de amostragem (WS)", pres.fmt_number(cfg.sample_rate_effective_hz or cfg.sample_rate_requested_hz, 0, "Hz")),
        ("Pontos da FFT", f"{cfg.window_size or pres.MISSING}"),
        ("Janela", (cfg.window_type or pres.MISSING).title()),
        ("Limiar do modo", threshold),
        (
            "Ganho de calibração",
            f"{pres.fmt_number(cfg.calibration_gain, 4)}"
            + (f" ({'+' if gain_db >= 0 else ''}{pres.fmt_number(gain_db, 2, 'dB')})" if gain_db is not None else ""),
        ),
    ]


def summary_segments(sensor: SensorNode) -> list[str]:
    """Resumo de uma linha para a lista de equipamentos."""

    metrics = latest(sensor)
    if metrics is None:
        return []
    parts = [f"LAeq {_db(metrics.laeq_db, 'A')}"]
    mode = sensor.sensor_mode.value
    if mode == "LEVEL" and metrics.dose_nho01_pct is not None:
        parts.append(f"dose {_pct(metrics.dose_nho01_pct, 1)}")
    elif mode == "MACHINE":
        rpm = acoustics.rpm_from_frequency(metrics.f0_hz)
        if rpm is not None and not acoustics.mains_component(metrics.f0_hz):
            parts.append(f"{pres.fmt_int(round(rpm))} rpm")
        if metrics.anomaly_z is not None:
            parts.append(f"anomalia z {pres.fmt_number(metrics.anomaly_z, 1)}")
    elif mode == "EVENTS" and metrics.events_total is not None:
        parts.append(f"{pres.fmt_int(metrics.events_total)} eventos")
    return parts
