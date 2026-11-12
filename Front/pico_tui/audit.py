"""Registro das ações do operador: o que foi pedido, por quem e com que resultado.

Todo comando sai para a rede pela TUI, então é nela que se sabe quem estava na
estação. Cada comando que altera a rede, cada entrada e saída de operador e
cada recusa vira um ``OperatorAction`` no barramento de eventos. O
``HistoryService`` grava no banco, com o autor, na categoria AUDIT; o
``LogEvent`` correspondente vai para as mensagens e para o arquivo JSONL da
sessão, que é uma segunda cópia, independente do banco.

Nada que seja segredo chega aqui: ``redact_command`` apaga senhas, tokens e
chaves antes de qualquer registro.
"""
from __future__ import annotations

import time
from collections.abc import Callable
from dataclasses import dataclass, field

from pico_tui.command_catalog import describe_can_command
from pico_tui.core.events import LogEvent, OperatorAction
from pico_tui.history.storage import AUDIT_CATEGORY  # noqa: F401  (reexportado para quem consome a trilha)
from pico_tui.operators import Actor
from pico_tui.security import canonical_can_command
from pico_tui.services.recorder import redact

AUDIT_SOURCE = "AUDITORIA"
NO_ACTOR_LABEL = "Sem identificação"


def redact_command(command: str) -> str:
    """O comando como pode aparecer no histórico: sem senhas, tokens nem chaves."""

    text = " ".join(str(command).split())
    if text.upper().startswith("NET WIFI PROVISION"):
        parts = text.split()
        return " ".join([*parts[:4], "***"]) if len(parts) > 4 else text
    return redact(text)


@dataclass(frozen=True, slots=True)
class CommandInfo:
    kind: str
    title: str
    command: str
    node: int | None = None
    sensor: str | None = None
    uuid: str | None = None
    action: str = ""
    fields: dict[str, str] = field(default_factory=dict)


_GATEWAY_LABELS = {
    "CONFIG": "Configuração alterada",
    "TELEMETRY": "Telemetria",
    "TELEMETRY_PERIOD": "Período da telemetria alterado",
    "TELEMETRY_RATE": "Taxa da telemetria alterada",
    "FFT": "Espectro (FFT) solicitado",
    "ACQ": "Modo de aquisição alterado",
    "DTC_CLEAR": "Falhas (DTC) apagadas",
    "WIFI": "Wi-Fi do módulo",
}
# Concordância: "telemetria ligada", "Wi-Fi ligado".
_STATE_WORDS = {"TELEMETRY": {"ON": "ligada", "OFF": "desligada"}, "WIFI": {"ON": "ligado", "OFF": "desligado"}}

_DIRECT_TITLES = (
    ("SET ", "SENSOR_CONFIG", "Parâmetro do sensor alterado"),
    ("APPLY", "SENSOR_CONFIG", "Configuração aplicada ao sensor"),
    ("TELEMETRY ", "SENSOR_TELEMETRY", "Telemetria do sensor alterada"),
    ("FFT ", "SENSOR_COMMAND", "Espectro (FFT) solicitado"),
    ("ACQ ", "SENSOR_COMMAND", "Modo de aquisição alterado"),
    ("DTC CLEAR", "SENSOR_DTC_CLEAR", "Falhas (DTC) apagadas"),
    ("RESET", "SENSOR_RESET", "Reinício solicitado"),
    ("NET WIFI ON", "WIFI_CONTROL", "Wi-Fi do módulo ligado"),
    ("NET WIFI OFF", "WIFI_CONTROL", "Wi-Fi do módulo desligado"),
    ("NET WIFI PROVISION", "WIFI_PROVISION", "Credenciais de Wi-Fi gravadas no módulo"),
    ("NET WIFI CLEAR", "WIFI_PROVISION", "Credenciais de Wi-Fi apagadas do módulo"),
    ("AUTH UNLOCK", "DEVICE_UNLOCK", "Desbloqueio de manutenção do firmware"),
    ("WIFI ", "WIFI_CONTROL", "Wi-Fi do módulo alterado"),
    ("MEASURE ", "SENSOR_COMMAND", "Medição reiniciada"),
    ("BASELINE ", "SENSOR_COMMAND", "Assinatura de referência alterada"),
    ("CAL ", "SENSOR_COMMAND", "Calibração do sensor"),
    ("FAULT ", "FAULT_COMMAND", "Ensaio de falha solicitado"),
)


def _int(token: str | None) -> int | None:
    try:
        return int(str(token), 0)
    except (TypeError, ValueError):
        return None


def _node_of(target: str) -> int | None:
    head = target.split(".", 1)[0]
    return int(head) if head.isdigit() else None


def _wireless(text: str, parts: list[str]) -> CommandInfo:
    action = parts[1].upper() if len(parts) > 1 else ""
    node = _int(parts[2]) if len(parts) > 2 else None
    uuid = parts[3] if len(parts) > 3 else None
    option = parts[4].upper() if len(parts) > 4 else ""
    where = f"Node {node:02d}" if node is not None else "Node ?"
    if action == "BIND" and uuid:
        suffix = " (reassociação automática)" if option == "AUTO" else ""
        return CommandInfo("SENSOR_BIND", f"Entrada do sensor {uuid} autorizada no {where}{suffix}",
                           text, node, None, uuid, "BIND")
    if action == "UNBIND" and uuid:
        return CommandInfo("SENSOR_UNBIND", f"Sensor {uuid} desassociado do {where}", text, node, None, uuid, "UNBIND")
    if action == "POLICY" and uuid:
        return CommandInfo("SENSOR_POLICY", f"Reassociação do sensor {uuid} no {where}: {option or '?'}",
                           text, node, None, uuid, "POLICY")
    return CommandInfo("WIRELESS_COMMAND", f"Comando de sensores sem fio: {text}", text, node, None, uuid, action)


def _gateway(text: str, parts: list[str]) -> CommandInfo:
    fields: dict[str, str] = {}
    for item in parts[1:]:
        key, separator, value = item.partition("=")
        if separator:
            fields[key.upper()] = value
    fields.pop("TX", None)
    target = fields.pop("TARGET", "")
    action = fields.pop("ACTION", "?").upper()
    label = _GATEWAY_LABELS.get(action, f"Comando {action}")
    state = _STATE_WORDS.get(action, {}).get(fields.get("STATE", "").upper())
    if state:
        label = f"{label} {state}"
    kind = "SENSOR_CONFIG" if action == "CONFIG" else "SENSOR_COMMAND"
    where = f" (sensor {target})" if target else ""
    return CommandInfo(kind, label + where, text, _node_of(target), target or None, None, action, fields)


def _can_admin(text: str, canonical: str) -> CommandInfo:
    node = int(canonical.split()[2], 16)
    title = describe_can_command(canonical) or f"Comando administrativo da rede CAN: {canonical}"
    return CommandInfo("NETWORK_COMMAND", title, text, None if node == 0xFF else node, None, None, canonical)


def classify_command(command: str) -> CommandInfo:
    """Transforma um comando que altera a rede numa frase para o histórico."""

    text = redact_command(command)
    upper = text.upper()
    parts = text.split()
    if upper.startswith("WIRELESS "):
        return _wireless(text, parts)
    if upper.startswith("CMD "):
        return _gateway(text, parts)
    canonical = canonical_can_command(upper)
    if canonical is not None and canonical.startswith(("22 00 ", "22 10 ", "22 30 ")):
        return _can_admin(text, canonical)
    for prefix, kind, title in _DIRECT_TITLES:
        if upper == prefix.strip() or upper.startswith(prefix):
            return CommandInfo(kind, title, text, None, None, None, prefix.strip())
    return CommandInfo("COMMAND", f"Comando enviado: {text}", text)


# ---------------------------------------------------------------------------
# Eventos
# ---------------------------------------------------------------------------


def actor_label(actor: Actor | None) -> str:
    return actor.label() if actor is not None else NO_ACTOR_LABEL


def make_action(kind: str, title: str, actor: Actor | None, *, outcome: str = "OK", severity: str | None = None,
                node_id: int | None = None, sensor_id: str | None = None, **details: object) -> OperatorAction:
    payload: dict[str, object] = {"actor": actor.record() if actor is not None else {"id": None}}
    payload.update({key: value for key, value in details.items() if value not in (None, "")})
    if severity is None:
        severity = "INFO" if outcome == "OK" else "WARNING"
    return OperatorAction(kind=kind, title=title, outcome=outcome, severity=severity,
                          actor=actor.id if actor is not None else None, node_id=node_id,
                          sensor_id=sensor_id, details=payload)


def command_action(info: CommandInfo, actor: Actor | None, *, outcome: str = "OK", reason: str = "") -> OperatorAction:
    prefix = {"DENIED": "Recusado", "FAILED": "Falhou"}.get(outcome)
    title = f"{prefix}: {info.title}" if prefix else info.title
    return make_action(
        info.kind if outcome == "OK" else f"COMMAND_{outcome}", title, actor, outcome=outcome,
        node_id=info.node, sensor_id=info.sensor, command=info.command, uuid=info.uuid,
        action=info.action, fields=info.fields or None, reason=reason,
        requested_kind=info.kind if outcome != "OK" else None,
    )


def log_event_for(action: OperatorAction, actor: Actor | None) -> LogEvent:
    """Linha legível para as mensagens e para o JSONL da sessão (segunda cópia do registro)."""

    level = "INFO" if action.outcome == "OK" else "WARNING"
    return LogEvent(level, f"{actor_label(actor)}: {action.title}", AUDIT_SOURCE,
                    {"kind": action.kind, "outcome": action.outcome, "actor": action.actor, **action.details})


# ---------------------------------------------------------------------------
# Quem autorizou cada associação
# ---------------------------------------------------------------------------

_STAMPED_STATES = {
    "BIND": frozenset({"ASSOCIATING", "BOUND", "ONLINE", "REJECTED"}),
    "UNBIND": frozenset({"UNBOUND"}),
}
_FINAL_STATES = {
    "BIND": frozenset({"ONLINE", "REJECTED"}),
    "UNBIND": frozenset({"UNBOUND"}),
}


@dataclass(slots=True)
class _Pending:
    actor: str
    action: str
    expires: float


class AuthorizationLedger:
    """Lembra quem pediu cada associação até o firmware responder.

    O resultado de uma associação chega depois, em eventos assíncronos do
    firmware (ASSOCIATING, BOUND, ONLINE...). Um evento só é atribuído a uma
    pessoa quando é a resposta a um comando que ela enviou há pouco tempo; as
    mudanças que a rede faz sozinha (sensor que some, reassociação automática)
    ficam sem autor, que é o retrato correto.
    """

    TTL_S = 120.0

    def __init__(self, ttl_s: float = TTL_S, clock: Callable[[], float] = time.monotonic) -> None:
        self.ttl_s = ttl_s
        self._clock = clock
        self._pending: dict[tuple[int, str], _Pending] = {}

    def remember(self, node: int | None, uuid: str | None, action: str, actor: Actor | None) -> None:
        if actor is None or node is None or not uuid or action not in _STAMPED_STATES:
            return
        now = self._clock()
        self._pending = {key: item for key, item in self._pending.items() if item.expires > now}
        self._pending[(int(node), uuid.lower())] = _Pending(actor.id, action, now + self.ttl_s)

    def claim(self, node: int, uuid: str, state: str) -> dict[str, str] | None:
        key = (int(node), str(uuid).lower())
        item = self._pending.get(key)
        if item is None:
            return None
        if item.expires <= self._clock():
            del self._pending[key]
            return None
        state = state.upper()
        if state not in _STAMPED_STATES[item.action]:
            return None
        if state in _FINAL_STATES[item.action]:
            del self._pending[key]
        return {"authorized_by": item.actor, "authorized_action": item.action}
