"""Formatação do espectro FFT em texto, com largura adaptável à tela."""
from __future__ import annotations

from pico_tui import presentation as pres
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


# Nomes anteriores, mantidos para compatibilidade com testes e scripts.
_spectrum_frequency = spectrum_frequency
_spectrum_metadata = spectrum_metadata
_spectrum_chart = spectrum_chart


def _spectrum_peaks(spectrum: SpectrumSample, count: int = 5) -> str:
    return "\n".join(["Picos principais", *spectrum_peaks(spectrum, count)])
