from __future__ import annotations

from pico_tui.core.models import Severity

DTC_CATALOG: dict[int, tuple[str, Severity]] = {
    0x0000: ("Sem DTC ativo", Severity.INFO),
    0x1002: ("I²C bus stuck / falha persistente de barramento", Severity.CRITICAL),
    0x2002: ("Clipping / saturação do acelerômetro", Severity.WARNING),
    0x4002: ("Falha de transmissão / rede UDP", Severity.WARNING),
}


def dtc_description(code: int) -> str:
    return DTC_CATALOG.get(code, ("DTC não catalogado", Severity.INFO))[0]


def dtc_severity(code: int) -> Severity:
    return DTC_CATALOG.get(code, ("DTC não catalogado", Severity.WARNING))[1]
