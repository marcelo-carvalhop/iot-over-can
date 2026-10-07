"""Camada de apresentação pura (sem Textual).

Tudo o que transforma o estado técnico em linguagem de operador fica aqui:
avaliação de condição (normal/atenção/crítico), rótulos em português,
formatação de números e idades e o empacotamento de segmentos em linhas
(``pack_segments``), usado para nunca cortar texto em telas estreitas.

Como não depende da interface, este módulo é coberto por testes unitários
rápidos e pode ser reutilizado por outras saídas (relatórios, exportações).
"""
from __future__ import annotations

import time
from collections.abc import Iterable
from dataclasses import dataclass, field
from enum import IntEnum

from pico_tui.core.models import (
    AppState,
    ConnectionMode,
    ConnectionState,
    DataQuality,
    NodeStatus,
    PhysicalNode,
    SensorNode,
    Severity,
)
from pico_tui.dtc_catalog import dtc_description

MISSING = "—"


class Level(IntEnum):
    """Condição operacional, em ordem crescente de gravidade."""

    NO_DATA = 0
    NORMAL = 1
    ATTENTION = 2
    CRITICAL = 3


LEVEL_LABELS = {
    Level.NO_DATA: "Sem dados",
    Level.NORMAL: "Normal",
    Level.ATTENTION: "Atenção",
    Level.CRITICAL: "Crítico",
}

UNICODE_SYMBOLS = {
    Level.NO_DATA: "○",
    Level.NORMAL: "●",
    Level.ATTENTION: "▲",
    Level.CRITICAL: "■",
}

ASCII_SYMBOLS = {
    Level.NO_DATA: "-",
    Level.NORMAL: "o",
    Level.ATTENTION: "!",
    Level.CRITICAL: "X",
}

_symbols = dict(UNICODE_SYMBOLS)


def use_ascii_symbols(enabled: bool) -> None:
    """Troca os símbolos de estado por ASCII puro (consoles sem Unicode)."""

    _symbols.clear()
    _symbols.update(ASCII_SYMBOLS if enabled else UNICODE_SYMBOLS)


def symbol(level: Level) -> str:
    return _symbols[level]


def level_label(level: Level) -> str:
    return LEVEL_LABELS[level]


def severity_level(severity: Severity | None) -> Level:
    if severity == Severity.CRITICAL:
        return Level.CRITICAL
    if severity == Severity.WARNING:
        return Level.ATTENTION
    return Level.NORMAL


@dataclass(slots=True)
class Condition:
    level: Level
    reasons: list[str] = field(default_factory=list)

    @property
    def summary(self) -> str:
        return self.reasons[0] if self.reasons else level_label(self.level)


@dataclass(slots=True)
class AttentionItem:
    level: Level
    target: str          # "node:04" ou "sensor:04.01"
    title: str           # "Sensor 04.01"
    reason: str


# ---------------------------------------------------------------------------
# Avaliação de condição
# ---------------------------------------------------------------------------


def sensor_condition(sensor: SensorNode) -> Condition:
    reasons_critical: list[str] = []
    reasons_attention: list[str] = []

    if sensor.status == NodeStatus.LOST:
        reasons_critical.append("Comunicação perdida")
    elif sensor.status == NodeStatus.STALE:
        reasons_attention.append("Sem dados recentes")

    for record in sorted(sensor.active_dtcs.values(), key=lambda item: item.code):
        if not record.active:
            continue
        text = f"DTC 0x{record.code:04X}: {dtc_description(record.code)}"
        if record.severity == Severity.CRITICAL:
            reasons_critical.append(text)
        elif record.severity == Severity.WARNING:
            reasons_attention.append(text)

    sample = sensor.latest_telemetry
    if sample is not None and sample.clipping is True:
        reasons_attention.append("Acelerômetro saturado")
    if sensor.quality in {DataQuality.DEGRADED, DataQuality.INVALID}:
        reasons_attention.append(f"Qualidade do dado {quality_label(sensor.quality).lower()}")

    if reasons_critical:
        return Condition(Level.CRITICAL, reasons_critical + reasons_attention)
    if reasons_attention:
        return Condition(Level.ATTENTION, reasons_attention)
    if sensor.status == NodeStatus.UNKNOWN and sample is None:
        return Condition(Level.NO_DATA, ["Aguardando primeira leitura"])
    return Condition(Level.NORMAL, [])


def node_condition(node: PhysicalNode) -> Condition:
    reasons_critical: list[str] = []
    reasons_attention: list[str] = []
    if node.status == NodeStatus.LOST:
        reasons_critical.append("Comunicação perdida")
    elif node.status == NodeStatus.STALE:
        reasons_attention.append("Sem dados recentes")
    elif node.status == NodeStatus.OFFLINE:
        reasons_attention.append("Módulo desativado")

    can_state = (node.can_state or "").upper()
    if "BUS_OFF" in can_state or "BUSOFF" in can_state:
        reasons_critical.append("Controlador CAN em bus-off")
    elif "PASSIVE" in can_state or can_state in {"ERROR", "FALHA"}:
        reasons_attention.append(f"Estado CAN {node.can_state}")

    for record in sorted(node.active_dtcs.values(), key=lambda item: item.code):
        if not record.active:
            continue
        text = f"DTC 0x{record.code:04X}: {dtc_description(record.code)}"
        if record.severity == Severity.CRITICAL:
            reasons_critical.append(text)
        elif record.severity == Severity.WARNING:
            reasons_attention.append(text)

    if reasons_critical:
        return Condition(Level.CRITICAL, reasons_critical + reasons_attention)
    if reasons_attention:
        return Condition(Level.ATTENTION, reasons_attention)
    if not node.last_seen_monotonic and node.status == NodeStatus.UNKNOWN:
        return Condition(Level.NO_DATA, ["Aguardando comunicação"])
    return Condition(Level.NORMAL, [])


def attention_items(state: AppState) -> list[AttentionItem]:
    """Tudo o que exige ação ou observação, do mais grave para o menos grave."""

    items: list[AttentionItem] = []
    for node_id, node in sorted(state.nodes.items()):
        condition = node_condition(node)
        if condition.level >= Level.ATTENTION:
            for reason in condition.reasons:
                items.append(AttentionItem(condition.level, f"node:{node_id:02d}", f"Módulo {node_id:02d}", reason))
        for _child_id, sensor in sorted(node.sensors.items()):
            sensor_cond = sensor_condition(sensor)
            if sensor_cond.level >= Level.ATTENTION:
                for reason in sensor_cond.reasons:
                    items.append(
                        AttentionItem(sensor_cond.level, f"sensor:{sensor.logical_id}", f"Sensor {sensor.logical_id}", reason)
                    )
    items.sort(key=lambda item: (-int(item.level), item.target))
    return items


@dataclass(slots=True)
class SystemCondition:
    level: Level
    headline: str
    connected: bool
    modules_total: int = 0
    modules_ok: int = 0
    sensors_total: int = 0
    sensors_ok: int = 0
    attention: list[AttentionItem] = field(default_factory=list)


def system_condition(state: AppState) -> SystemCondition:
    connected = state.connection_state not in {ConnectionState.DISCONNECTED}
    modules_total = len(state.nodes)
    modules_ok = sum(1 for node in state.nodes.values() if node_condition(node).level == Level.NORMAL)
    sensors = [sensor for node in state.nodes.values() for sensor in node.sensors.values()]
    sensors_ok = sum(1 for sensor in sensors if sensor_condition(sensor).level == Level.NORMAL)
    items = attention_items(state)

    network_critical: list[str] = []
    network_attention: list[str] = []
    if state.connection_state == ConnectionState.INCOMPATIBLE:
        network_critical.append("Firmware incompatível com esta versão da TUI")
    if state.network.bus_off:
        network_critical.append("Barramento CAN em bus-off")
    elif state.network.error_passive:
        network_attention.append("Barramento CAN em estado error-passive")
    elif state.network.error_warning:
        network_attention.append("Barramento CAN com alerta de erros")

    base = SystemCondition(
        level=Level.NORMAL,
        headline="",
        connected=connected,
        modules_total=modules_total,
        modules_ok=modules_ok,
        sensors_total=len(sensors),
        sensors_ok=sensors_ok,
        attention=items,
    )

    if not connected:
        base.level = Level.NO_DATA
        base.headline = "Desconectado"
        return base
    if state.connection_state in {ConnectionState.CONNECTING, ConnectionState.SYNCHRONIZING, ConnectionState.RECONNECTING}:
        base.level = Level.NO_DATA
        base.headline = connection_label(state.connection_state)
        return base

    worst_item = max((item.level for item in items), default=Level.NORMAL)
    if network_critical or worst_item == Level.CRITICAL:
        base.level = Level.CRITICAL
        base.headline = network_critical[0] if network_critical else _count_phrase(items, Level.CRITICAL)
    elif network_attention or worst_item == Level.ATTENTION:
        base.level = Level.ATTENTION
        base.headline = network_attention[0] if network_attention else _count_phrase(items, Level.ATTENTION)
    elif modules_total == 0 and not sensors:
        base.level = Level.NORMAL
        base.headline = "Conectado, aguardando módulos"
    else:
        base.level = Level.NORMAL
        base.headline = "Operação normal"
    return base


def _count_phrase(items: Iterable[AttentionItem], level: Level) -> str:
    targets = {item.target for item in items if item.level == level}
    count = len(targets)
    if level == Level.CRITICAL:
        return "1 equipamento em estado crítico" if count == 1 else f"{count} equipamentos em estado crítico"
    return "1 equipamento requer atenção" if count == 1 else f"{count} equipamentos requerem atenção"


# ---------------------------------------------------------------------------
# Rótulos em linguagem de operador
# ---------------------------------------------------------------------------

_CONNECTION_LABELS = {
    ConnectionState.DISCONNECTED: "Desconectado",
    ConnectionState.CONNECTING: "Conectando",
    ConnectionState.CONNECTED: "Conectado",
    ConnectionState.SYNCHRONIZING: "Sincronizando",
    ConnectionState.READY: "Conectado",
    ConnectionState.DEGRADED: "Conexão degradada",
    ConnectionState.RECONNECTING: "Reconectando",
    ConnectionState.INCOMPATIBLE: "Firmware incompatível",
}

_MODE_LABELS = {
    ConnectionMode.GATEWAY_CAN: "Probe 00 na rede CAN",
    ConnectionMode.SENSOR_DIRECT: "Sensor direto por USB",
    ConnectionMode.DEMO: "Demonstração sem hardware",
    ConnectionMode.AUTO: "Detectando protocolo",
    ConnectionMode.UNKNOWN: "Detectando protocolo",
}

_ROLE_LABELS = {"LEADER": "Líder", "FOLLOWER": "Seguidor", "GATEWAY": "Gateway"}

_PROFILE_LABELS = {
    "VIBRATION": "Vibração",
    "DEMO_BYTE": "Byte de demonstração",
    "NONE": "Nenhum",
    "": "Perfil não informado",
}

_SENSOR_MODE_LABELS = {
    "IDLE": "Ocioso",
    "ROTATING": "Máquina rotativa",
    "STRUCTURAL": "Estrutural",
    "SEISMIC": "Sísmico",
    "UNKNOWN": MISSING,
}

_QUALITY_LABELS = {
    DataQuality.REAL: "Medição real",
    DataQuality.SIMULATED: "Simulada",
    DataQuality.PLACEHOLDER: MISSING,
    DataQuality.DEGRADED: "Degradada",
    DataQuality.INVALID: "Inválida",
    DataQuality.STALE: "Desatualizada",
    DataQuality.LOST: "Perdida",
    DataQuality.UNKNOWN: MISSING,
}

_CAPABILITY_LABELS = {
    "CAN": "CAN",
    "LOCAL_SENSOR": "Sensor local",
    "LOCAL_SENSOR_DEMO": "Sensor local de demonstração",
    "BLE_SCAN": "Varredura BLE",
    "WIFI_AP": "Ponto de acesso Wi-Fi",
    "WIRELESS_BRIDGE": "Ponte para sensores sem fio",
    "CAN_MONITOR": "Monitor CAN",
    "SERIAL_INSTRUMENTATION": "Instrumentação serial",
}

_DISCOVERY_LABELS = {
    "NOT_IMPLEMENTED": "Indisponível neste firmware",
    "SCANNING": "Procurando sensores",
    "IDLE": "Parada",
    "OFF": "Desligada",
    "ON": "Ligada",
    "UNKNOWN": MISSING,
}

_STATUS_LABELS = {
    NodeStatus.ONLINE: "Comunicando",
    NodeStatus.AGING: "Comunicando (atraso leve)",
    NodeStatus.STALE: "Sem dados recentes",
    NodeStatus.LOST: "Comunicação perdida",
    NodeStatus.OFFLINE: "Desativado",
    NodeStatus.UNKNOWN: "Aguardando dados",
}


def connection_label(state: ConnectionState) -> str:
    return _CONNECTION_LABELS.get(state, state.value)


def mode_label(mode: ConnectionMode) -> str:
    return _MODE_LABELS.get(mode, mode.value)


def role_label(role: str) -> str:
    return _ROLE_LABELS.get((role or "").upper(), "Papel não informado")


def profile_label(profile: str) -> str:
    key = (profile or "").upper()
    return _PROFILE_LABELS.get(key, key.title())


def sensor_mode_label(mode: str) -> str:
    return _SENSOR_MODE_LABELS.get((mode or "UNKNOWN").upper(), mode)


def quality_label(quality: DataQuality) -> str:
    return _QUALITY_LABELS.get(quality, quality.value)


def capability_label(capability: str) -> str:
    return _CAPABILITY_LABELS.get(capability.upper(), capability)


def discovery_label(value: str) -> str:
    return _DISCOVERY_LABELS.get((value or "UNKNOWN").upper(), value)


def status_label(status: NodeStatus) -> str:
    return _STATUS_LABELS.get(status, status.value)


# ---------------------------------------------------------------------------
# Formatação
# ---------------------------------------------------------------------------


def fmt_number(value: object, decimals: int = 3, unit: str = "") -> str:
    if value is None:
        return MISSING
    try:
        number = float(value)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return str(value)
    if number != number:  # NaN
        return MISSING
    text = f"{number:.{decimals}f}".replace(".", ",")
    return f"{text} {unit}".strip()


def fmt_int(value: object) -> str:
    if value is None:
        return MISSING
    try:
        return f"{int(value):,}".replace(",", ".")  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return str(value)


def fmt_age(seconds: float | None) -> str:
    if seconds is None:
        return MISSING
    seconds = max(0.0, seconds)
    if seconds < 1.0:
        return "agora"
    if seconds < 60:
        return f"há {seconds:.0f} s"
    if seconds < 3600:
        return f"há {seconds // 60:.0f} min"
    return f"há {seconds // 3600:.0f} h"


def fmt_bool(value: bool | None, yes: str = "Sim", no: str = "Não") -> str:
    if value is None:
        return MISSING
    return yes if value else no


def fmt_clock(wall_time: float) -> str:
    return time.strftime("%H:%M:%S", time.localtime(wall_time))


def sensor_age(sensor: SensorNode) -> float | None:
    if sensor.latest_telemetry is not None:
        return time.time() - sensor.latest_telemetry.received_wall_time
    if sensor.last_seen_monotonic:
        return time.monotonic() - sensor.last_seen_monotonic
    return None


def node_age(node: PhysicalNode) -> float | None:
    if node.last_seen_monotonic:
        return time.monotonic() - node.last_seen_monotonic
    return None


# ---------------------------------------------------------------------------
# Empacotamento de segmentos (texto sem fragmentação)
# ---------------------------------------------------------------------------


def pack_segments(lengths: list[int], width: int, gap: int = 3) -> list[list[int]]:
    """Distribui segmentos indivisíveis em linhas de no máximo ``width`` células.

    Cada segmento (por exemplo "F4 Comandos" ou "Conectado a /dev/ttyUSB0")
    é mantido inteiro numa mesma linha. Um segmento maior que a largura total
    ocupa uma linha sozinho e é quebrado por palavras pelo renderizador.
    Retorna a lista de linhas, cada uma com os índices dos segmentos.
    """

    width = max(1, width)
    lines: list[list[int]] = []
    current: list[int] = []
    used = 0
    for index, length in enumerate(lengths):
        if not current:
            current = [index]
            used = length
            continue
        if used + gap + length <= width:
            current.append(index)
            used += gap + length
        else:
            lines.append(current)
            current = [index]
            used = length
    if current:
        lines.append(current)
    return lines
