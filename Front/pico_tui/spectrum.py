"""Formatação do espectro FFT em texto, com largura adaptável à tela."""
from __future__ import annotations

import math

from pico_tui import presentation as pres
from pico_tui.acoustics import pa_to_db
from pico_tui.core.models import SpectrumSample

LABEL_WIDTH = 11  # "  1,23e+03 ┤"


def spectrum_frequency(spectrum: SpectrumSample, bin_index: int) -> float | None:
    if spectrum.sample_rate_hz is None or not spectrum.fft_size:
        return None
    return bin_index * spectrum.sample_rate_hz / spectrum.fft_size


def spectrum_fields(spectrum: SpectrumSample) -> list[tuple[str, str]]:
    bins = len(spectrum.magnitudes)
    resolution = None
    if spectrum.sample_rate_hz is not None and spectrum.fft_size:
        resolution = spectrum.sample_rate_hz / spectrum.fft_size
    max_frequency = spectrum_frequency(spectrum, max(0, bins - 1))
    nyquist = spectrum.sample_rate_hz / 2 if spectrum.sample_rate_hz is not None else None
    return [
        ("Faixas recebidas", str(bins)),
        ("Pontos da FFT", str(spectrum.fft_size or pres.MISSING)),
        ("Taxa de amostragem", pres.fmt_number(spectrum.sample_rate_hz, 2, "Hz")),
        ("Resolução (Δf)", pres.fmt_number(resolution, 4, "Hz")),
        ("Faixa exibida", f"0 a {pres.fmt_number(max_frequency, 2, 'Hz')}"),
        ("Nyquist", pres.fmt_number(nyquist, 2, "Hz")),
        ("Janela", spectrum.window_type or pres.MISSING),
        ("Unidade da magnitude", spectrum.magnitude_unit or "raw"),
    ]


def spectrum_metadata(spectrum: SpectrumSample) -> str:
    return "\n".join(f"{label}: {value}" for label, value in spectrum_fields(spectrum))


def spectrum_chart(spectrum: SpectrumSample, width: int = 72, height: int = 12) -> str:
    """Gráfico de barras em texto. ``width`` é a largura total disponível."""

    values = spectrum.magnitudes
    if not values:
        return pres.MISSING
    chart_width = max(8, min(width - LABEL_WIDTH, len(values)))
    buckets: list[tuple[float, int]] = []
    for column in range(chart_width):
        start = column * len(values) // chart_width
        end = max(start + 1, (column + 1) * len(values) // chart_width)
        local = values[start:end]
        local_index = max(range(len(local)), key=lambda idx: local[idx])
        buckets.append((float(local[local_index]), start + local_index))
    peak_value = max(value for value, _ in buckets)
    scale = peak_value if peak_value > 0 else 1.0
    lines: list[str] = []
    for row in range(height, 0, -1):
        threshold = scale * row / height
        label = f"{threshold:>9.3g} ┤".replace(".", ",")
        body = "".join("█" if value >= threshold else " " for value, _ in buckets)
        lines.append(label + body)
    lines.append(f"{0:>9.3g} └".replace(".", ",") + "─" * chart_width)

    max_frequency = spectrum_frequency(spectrum, len(values) - 1)
    if max_frequency is None:
        left, right = "faixa 0", f"faixa {len(values) - 1}"
        axis_name = "faixa espectral"
    else:
        left, right = "0 Hz", pres.fmt_number(max_frequency, 2, "Hz")
        axis_name = "frequência"
    gap = max(1, chart_width - len(left) - len(right))
    lines.append(" " * LABEL_WIDTH + left + " " * gap + right)
    lines.append(f"Magnitude [{spectrum.magnitude_unit or 'raw'}] por {axis_name}")
    return "\n".join(lines)


def spectrum_peaks(spectrum: SpectrumSample, count: int = 5) -> list[str]:
    if not spectrum.magnitudes:
        return []
    ranked = sorted(enumerate(spectrum.magnitudes), key=lambda item: item[1], reverse=True)[:count]
    rows = []
    for position, (index, value) in enumerate(ranked, start=1):
        frequency = spectrum_frequency(spectrum, index)
        frequency_text = pres.fmt_number(frequency, 3, "Hz") if frequency is not None else f"faixa {index}"
        rows.append(f"{position}. {frequency_text}  magnitude {pres.fmt_number(value, 6)}  faixa {index}")
    return rows


# ---------------------------------------------------------------------------
# Sensor acústico: magnitudes em Pa eficaz, exibidas em dB SPL
# ---------------------------------------------------------------------------

ACOUSTIC_DYNAMIC_RANGE_DB = 60.0


def spectrum_levels_db(spectrum: SpectrumSample) -> list[float | None]:
    """Nível de cada faixa em dB re 20 µPa (None para faixas zeradas)."""

    return [pa_to_db(value) for value in spectrum.magnitudes]


def acoustic_spectrum_chart(spectrum: SpectrumSample, width: int = 72, height: int = 12) -> str:
    """Gráfico em dB SPL, com os 60 dB abaixo do topo (faixas mais fracas no piso)."""

    levels = spectrum_levels_db(spectrum)
    present = [level for level in levels if level is not None]
    if not present:
        return pres.MISSING
    top = math.ceil(max(present) / 10.0) * 10.0
    floor = top - ACOUSTIC_DYNAMIC_RANGE_DB
    chart_width = max(8, min(width - LABEL_WIDTH, len(levels)))
    columns: list[float] = []
    for column in range(chart_width):
        start = column * len(levels) // chart_width
        end = max(start + 1, (column + 1) * len(levels) // chart_width)
        local = [level for level in levels[start:end] if level is not None]
        columns.append(max(local) if local else floor)
    lines: list[str] = []
    for row in range(height, 0, -1):
        threshold = floor + (top - floor) * row / height
        body = "".join("█" if value >= threshold else " " for value in columns)
        lines.append(f"{threshold:>6.0f} dB ┤" + body)
    lines.append(f"{floor:>6.0f} dB └" + "─" * chart_width)
    max_frequency = spectrum_frequency(spectrum, len(levels) - 1)
    left, right = ("0 Hz", pres.fmt_number(max_frequency, 0, "Hz")) if max_frequency is not None else ("faixa 0", f"faixa {len(levels) - 1}")
    gap = max(1, chart_width - len(left) - len(right))
    lines.append(" " * LABEL_WIDTH + left + " " * gap + right)
    lines.append("Nível de pressão sonora [dB re 20 µPa] por frequência; cada coluna mostra a faixa mais forte")
    return "\n".join(lines)


def acoustic_spectrum_peaks(spectrum: SpectrumSample, count: int = 5) -> list[str]:
    levels = spectrum_levels_db(spectrum)
    ranked = sorted((item for item in enumerate(levels) if item[1] is not None), key=lambda item: item[1], reverse=True)
    rows = []
    for position, (index, level) in enumerate(ranked[:count], start=1):
        frequency = spectrum_frequency(spectrum, index)
        frequency_text = pres.fmt_number(frequency, 1, "Hz") if frequency is not None else f"faixa {index}"
        rows.append(f"{position}. {frequency_text}  {pres.fmt_number(level, 1, 'dB SPL')}  faixa {index}")
    return rows


def acoustic_spectrum_fields(spectrum: SpectrumSample) -> list[tuple[str, str]]:
    fields = [(label, value) for label, value in spectrum_fields(spectrum) if label != "Unidade da magnitude"]
    present = [level for level in spectrum_levels_db(spectrum) if level is not None]
    fields.append(("Unidade", "dB SPL (pressão eficaz por faixa, re 20 µPa)"))
    fields.append(("Faixa mais forte", pres.fmt_number(max(present), 1, "dB SPL") if present else pres.MISSING))
    fields.append(("Média do intervalo", "Welch: média dos blocos de um segundo"))
    return fields


# Nomes anteriores, mantidos para compatibilidade com testes e scripts.
_spectrum_frequency = spectrum_frequency
_spectrum_metadata = spectrum_metadata
_spectrum_chart = spectrum_chart


def _spectrum_peaks(spectrum: SpectrumSample, count: int = 5) -> str:
    return "\n".join(["Picos principais", *spectrum_peaks(spectrum, count)])
