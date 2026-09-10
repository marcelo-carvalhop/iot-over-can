from __future__ import annotations

from pico_tui.core.models import Severity

DTC_CATALOG: dict[int, tuple[str, Severity]] = {
    0x0000: ("Sem DTC ativo", Severity.INFO),

    # 0x1xxx — barramento / aquisição
    0x1001: ("MPU6050 sem comunicação no I2C0 / WHO_AM_I inválido", Severity.CRITICAL),
    0x1002: ("Barramento I2C0 travado ou timeout persistente de leitura", Severity.CRITICAL),

    # 0x2xxx — sensor / evento físico
    0x2001: ("Vetor de gravidade fora da faixa esperada; possível soltura, impacto ou montagem instável", Severity.CRITICAL),
    0x2002: ("Saturação do acelerômetro / clipping mecânico no MPU6050", Severity.WARNING),

    # 0x3xxx — processamento local
    0x3001: ("Sobrecarga no processamento DSP / janela não processada no tempo esperado", Severity.WARNING),

    # 0x4xxx — sistema, energia e rede
    0x4001: ("Tensão de alimentação baixa ou crítica no nó sensor", Severity.CRITICAL),
    0x4002: ("Falha repetida de transmissão UDP após vínculo com gateway", Severity.WARNING),
    0x4003: ("Vínculo rejeitado por credencial/chave de autenticação", Severity.WARNING),
    0x4004: ("Vínculo rejeitado por incompatibilidade de versão do protocolo", Severity.WARNING),
    0x4005: ("Comando de rede rejeitado por contador repetido ou antigo (replay)", Severity.WARNING),
    0x4006: ("Configuração remota rejeitada por campo inválido ou fora da faixa", Severity.WARNING),
}


DTC_CATEGORY_FALLBACK: dict[int, tuple[str, Severity]] = {
    0x1000: ("Falha de barramento ou aquisição ainda não detalhada", Severity.WARNING),
    0x2000: ("Falha física ou mecânica do sensor ainda não detalhada", Severity.WARNING),
    0x3000: ("Falha de processamento DSP ainda não detalhada", Severity.WARNING),
    0x4000: ("Falha de sistema, energia ou rede ainda não detalhada", Severity.WARNING),
}


def dtc_description(code: int) -> str:
    if code in DTC_CATALOG:
        return DTC_CATALOG[code][0]

    category = code & 0xF000
    category_desc = DTC_CATEGORY_FALLBACK.get(
        category,
        ("Código DTC fora das categorias conhecidas do firmware", Severity.WARNING),
    )[0]

    return f"{category_desc} — código bruto 0x{code:04X}"


def dtc_severity(code: int) -> Severity:
    if code in DTC_CATALOG:
        return DTC_CATALOG[code][1]

    category = code & 0xF000
    return DTC_CATEGORY_FALLBACK.get(category, ("", Severity.WARNING))[1]
