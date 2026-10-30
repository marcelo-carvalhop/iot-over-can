"""Etapas da criação do canal de dados entre um sensor sem fio e o módulo CAN.

Depois do vínculo (BIND), o módulo liga o ponto de acesso Wi-Fi, anuncia a
oferta por BLE, recebe o sensor na rede, confere o HELLO, envia o CHALLENGE e
abre a sessão quando o CONFIRM chega. Desde a v0.22 o módulo relata cada fato
ao CAN (``[GW] WIRELESS_EVENT``); este módulo transforma os relatos em texto
para o registro de mensagens e na lista de etapas da tela do sensor, com o
que verificar quando a criação para.

Com um módulo de firmware anterior não há relatos: as etapas são deduzidas do
estado do enlace (``WIRELESS_LINK``), com menos detalhe.
"""

from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Iterable, Sequence

from pico_tui.core.models import DataLinkEvent, SensorNode

# Motivos do firmware (ioc_wd_event_reason) em português.
REASON_TEXT = {
    "FORMAT": "HELLO mal formado",
    "NOT_BOUND": "o sensor não está associado a este módulo",
    "KEY": "a chave do sensor não confere com a do módulo",
    "PROTOCOL": "versão de protocolo diferente da do módulo",
    "REPLAY": "repetição do HELLO da sessão em vigor",
    "BUSY": "autenticação anterior ainda em andamento",
    "SEND": "o módulo não conseguiu enviar o CHALLENGE",
    "TIMEOUT": "o sensor parou de enviar dados",
    "UNBOUND": "vínculo desfeito",
    "SLEEP": "painel dormindo",
    "TEST": "ensaio de falha",
    "RADIO_OFF": "rádio do módulo desligado",
    "IDLE": "nenhum sensor associado ao módulo",
}

# Fatos que são parte do caminho normal; os demais pedem atenção.
_PROGRESS = {"AP_UP", "OFFER", "STATION_JOIN", "STATION_IP", "HELLO_OK", "SESSION_UP"}
# Recusas que não indicam problema: o sensor repetiu um HELLO.
_BENIGN_REJECTS = {"REPLAY", "BUSY"}
# Estados de associação em que o vínculo já foi confirmado pelo módulo.
_BOUND_STATES = {"BOUND", "ONLINE", "STALE", "LOST"}
HANDSHAKE_TIMEOUT_S = 5


def reason_text(reason: str) -> str:
    return REASON_TEXT.get((reason or "NONE").upper(), (reason or "motivo não informado").lower())


def event_text(event: DataLinkEvent) -> str:
    """Frase curta do fato, sem o identificador do sensor."""

    kind = event.event.upper()
    reason = reason_text(event.reason)
    if kind == "AP_UP":
        text = f"ponto de acesso no ar (canal {event.value})" if event.value else "ponto de acesso no ar"
    elif kind == "AP_DOWN":
        text = f"ponto de acesso desligado: {reason}"
    elif kind == "AP_ERROR":
        text = "o ponto de acesso Wi-Fi não subiu; o módulo tenta de novo"
    elif kind == "NO_KEY":
        text = "módulo sem chave de enlace: não há canal de dados"
    elif kind == "OFFER":
        text = f"oferta anunciada por BLE (canal {event.value})" if event.value else "oferta anunciada por BLE"
    elif kind == "OFFER_ERROR":
        text = "o anúncio BLE da oferta não começou"
    elif kind == "STATION_JOIN":
        text = f"uma estação entrou no ponto de acesso ({_stations(event.stations)})"
    elif kind == "STATION_IP":
        text = f"a estação recebeu endereço IP (final .{event.value})"
    elif kind == "STATION_LEAVE":
        text = f"uma estação saiu do ponto de acesso ({_stations(event.stations)})"
    elif kind == "HELLO_OK":
        text = "HELLO conferido; CHALLENGE enviado"
    elif kind == "HELLO_REJECT" and event.reason.upper() == "NOT_BOUND" and not event.node_level:
        text = f"HELLO enviado ao Módulo {event.parent_node_id:02d}, que não tem este sensor associado"
    elif kind == "HELLO_REJECT":
        text = f"HELLO recusado: {reason}"
    elif kind == "HANDSHAKE_TIMEOUT":
        text = f"o sensor não respondeu ao CHALLENGE em {HANDSHAKE_TIMEOUT_S} s"
    elif kind == "SESSION_UP":
        text = "sessão autenticada"
    elif kind == "SESSION_DOWN":
        text = f"sessão encerrada: {reason}"
    else:
        text = f"{kind.lower()} ({reason})"
    if event.count > 1:
        text += f" ({event.count} vezes)"
    return text


def event_level(event: DataLinkEvent) -> str:
    """Nível no registro de mensagens."""

    kind = event.event.upper()
    reason = event.reason.upper()
    if kind in _PROGRESS:
        return "INFO"
    if kind == "HELLO_REJECT" and reason in _BENIGN_REJECTS:
        return "DEBUG"
    if kind == "SESSION_DOWN" and reason == "SLEEP":
        return "DEBUG"
    if kind in {"AP_DOWN", "STATION_LEAVE"} and reason in {"IDLE", "NONE"}:
        return "INFO"
    return "WARNING"


def _stations(count: int) -> str:
    return "nenhuma no total" if count == 0 else ("1 no total" if count == 1 else f"{count} no total")


# ---------------------------------------------------------------------------
# Etapas
# ---------------------------------------------------------------------------


DONE = "done"
CURRENT = "current"
FAILED = "failed"
PENDING = "pending"


@dataclass(slots=True)
class Stage:
    key: str
    label: str
    state: str = PENDING
    detail: str = ""
    wall_time: float = 0.0       # quando a etapa foi concluída (ou falhou)
    since_bind_s: int | None = None


@dataclass(slots=True)
class ChannelView:
    stages: list[Stage]
    hint: str = ""
    waiting_s: float | None = None     # há quanto tempo a etapa atual espera
    previous_drop: str = ""            # por que a sessão anterior caiu

    @property
    def current(self) -> Stage | None:
        for stage in self.stages:
            if stage.state in {CURRENT, FAILED}:
                return stage
        return None

    @property
    def complete(self) -> bool:
        return all(stage.state == DONE for stage in self.stages)


STAGE_LABELS = (
    ("bind", "Vínculo confirmado pelo módulo"),
    ("ap", "Ponto de acesso Wi-Fi do módulo no ar"),
    ("offer", "Oferta anunciada por BLE"),
    ("station", "Sensor na rede Wi-Fi do módulo"),
    ("hello", "HELLO do sensor conferido"),
    ("session", "Sessão autenticada"),
    ("telemetry", "Primeira leitura no CAN"),
)

# Rótulo curto da etapa em espera (linha da tela inicial).
WAITING_LABELS = {
    "bind": "aguardando a confirmação do vínculo",
    "ap": "aguardando o ponto de acesso do módulo",
    "offer": "aguardando a oferta BLE do módulo",
    "station": "aguardando o sensor entrar na rede Wi-Fi do módulo",
    "hello": "sensor na rede; aguardando o HELLO",
    "session": "CHALLENGE enviado; aguardando a resposta do sensor",
    "telemetry": "sessão autenticada; aguardando a primeira leitura",
}


def _merged(sensor: SensorNode, node_events: Iterable[DataLinkEvent]) -> list[DataLinkEvent]:
    events = list(sensor.data_link_events)
    events.extend(event for event in node_events if event.event.upper().startswith("STATION_"))
    events.sort(key=lambda event: event.received_wall_time)
    return events


def channel_view(
    sensor: SensorNode,
    node_events: Sequence[DataLinkEvent] = (),
    *,
    now: float | None = None,
) -> ChannelView:
    """Etapas da criação do canal de dados do sensor, da mais antiga à atual."""

    now = time.time() if now is None else now
    stages = {key: Stage(key, label) for key, label in STAGE_LABELS}
    order = [key for key, _label in STAGE_LABELS]
    is_panel = (sensor.profile_id or "").upper() == "PANEL"
    if is_panel:
        order.remove("telemetry")  # o painel não envia leituras ao CAN
        stages.pop("telemetry")
    previous_drop = ""
    reject: DataLinkEvent | None = None

    def mark(key: str, state: str, event: DataLinkEvent | None, detail: str = "") -> None:
        stage = stages[key]
        stage.state = state
        stage.detail = detail
        stage.wall_time = event.received_wall_time if event is not None else 0.0
        stage.since_bind_s = event.since_s if event is not None and not event.node_level else None

    def reset(*keys: str) -> None:
        for key in keys:
            if key in stages:
                stages[key] = Stage(key, dict(STAGE_LABELS)[key])

    def reached(event: DataLinkEvent) -> None:
        # Um HELLO recebido por este módulo prova as etapas anteriores: o
        # ponto de acesso estava no ar e o sensor entrou na rede.
        for key in ("ap", "offer", "station"):
            if stages[key].state != DONE:
                mark(key, DONE, event, "deduzido do HELLO")

    bound = (sensor.association_state or "").upper() in _BOUND_STATES
    if bound:
        stages["bind"].state = DONE
        stages["bind"].wall_time = sensor.data_link_bound_wall

    for event in _merged(sensor, node_events):
        kind = event.event.upper()
        reason = event.reason.upper()
        if kind == "AP_UP":
            mark("ap", DONE, event, f"canal {event.value}" if event.value else "")
        elif kind in {"AP_DOWN", "AP_ERROR", "NO_KEY"}:
            reset("offer", "station", "hello", "session", "telemetry")
            mark("ap", FAILED, event, event_text(event))
        elif kind == "OFFER":
            mark("offer", DONE, event, f"canal {event.value}" if event.value else "")
        elif kind == "OFFER_ERROR":
            mark("offer", FAILED, event, event_text(event))
        elif kind in {"STATION_JOIN", "STATION_IP"}:
            if stages["offer"].state == DONE and stages["session"].state != DONE:
                detail = f"endereço final .{event.value}" if kind == "STATION_IP" else event_text(event)
                mark("station", DONE, event, detail)
        elif kind == "STATION_LEAVE":
            if event.stations == 0 and stages["session"].state != DONE and stages["station"].state == DONE:
                reset("station", "hello")
                stages["station"].detail = "a estação saiu do ponto de acesso"
        elif kind == "HELLO_OK":
            reached(event)
            mark("hello", DONE, event, "CHALLENGE enviado")
            reset("session")
            reject = None
        elif kind == "HELLO_REJECT":
            if reason in _BENIGN_REJECTS:
                continue
            reject = event
            if reason != "NOT_BOUND":
                reached(event)
            if stages["session"].state != DONE:
                mark("hello", FAILED, event, event_text(event))
        elif kind == "HANDSHAKE_TIMEOUT":
            if stages["session"].state != DONE:
                mark("session", FAILED, event, event_text(event))
        elif kind == "SESSION_UP":
            reached(event)
            if stages["hello"].state != DONE:
                mark("hello", DONE, event)
            mark("session", DONE, event)
            reject = None
            previous_drop = ""
        elif kind == "SESSION_DOWN":
            reset("offer", "station", "hello", "session", "telemetry")
            previous_drop = f"a sessão anterior caiu às {time.strftime('%H:%M:%S', time.localtime(event.received_wall_time))}: {reason_text(event.reason)}"

    # Sem relatos (firmware anterior à v0.22, ou a TUI abriu depois), o
    # estado do enlace ainda diz até onde a criação chegou. Ele não informa o
    # ponto de acesso: OFFERING continua durante uma queda do AP, por isso
    # essas duas etapas só são deduzidas quando não há relato nenhum delas.
    link = (sensor.data_link_state or "NONE").upper()
    link_wall = sensor.data_link_state_wall
    implied: tuple[str, ...] = ()
    if link in {"OFFERING", "SLEEPING"}:
        implied = ("ap", "offer")
    elif link == "HANDSHAKE":
        implied = ("ap", "offer", "station", "hello")
    elif link == "SECURE":
        implied = ("ap", "offer", "station", "hello", "session")
    elif link == "NO_KEY" and stages["ap"].state != FAILED:
        stages["ap"].state = FAILED
        stages["ap"].detail = "módulo sem chave de enlace: não há canal de dados"
    for key in implied:
        stage = stages[key]
        if stage.state == DONE:
            continue
        if stage.state == FAILED and (key in {"ap", "offer"} or stage.wall_time >= link_wall):
            continue  # a falha relatada é mais recente que o estado do enlace
        stage.state = DONE
        stage.detail = "informado pelo estado do enlace"
    if link != "SECURE" and stages["session"].state == DONE and link_wall > stages["session"].wall_time:
        # A sessão caiu e o relato se perdeu: o estado do enlace prevalece.
        reset("session", "telemetry")
    if "telemetry" in stages and stages["session"].state == DONE:
        sample = sensor.latest_telemetry
        session_wall = stages["session"].wall_time
        if sample is not None and sample.received_wall_time >= session_wall:
            stages["telemetry"].state = DONE
            stages["telemetry"].wall_time = sample.received_wall_time

    # A primeira etapa não concluída é a atual; as seguintes ficam pendentes.
    ordered = [stages[key] for key in order]
    waiting_s: float | None = None
    last_done = sensor.data_link_bound_wall or 0.0
    current: Stage | None = None
    for stage in ordered:
        if current is not None:
            if stage.state != PENDING:
                stage.state = PENDING
                stage.detail = ""
            continue
        if stage.state == DONE:
            last_done = max(last_done, stage.wall_time)
            continue
        current = stage
        if stage.state == PENDING:
            stage.state = CURRENT
        if last_done:
            waiting_s = max(0.0, now - last_done)
    view = ChannelView(ordered, waiting_s=waiting_s, previous_drop=previous_drop)
    view.hint = _hint(view, sensor, reject)
    return view


def _hint(view: ChannelView, sensor: SensorNode, reject: DataLinkEvent | None) -> str:
    stage = view.current
    if stage is None:
        return ""
    module = f"Módulo {sensor.parent_node_id:02d}"
    ssid = f"IOC-{sensor.parent_node_id:02d}"
    if stage.key == "bind":
        return "O módulo ainda não confirmou o vínculo; ele responde em poucos segundos."
    if stage.key == "ap":
        if stage.state == FAILED and "chave" in stage.detail:
            return (
                f"O {module} não tem chave de enlace provisionada e, sem ela, não abre o canal de dados. "
                "Grave a chave no módulo e reinicie-o."
            )
        if stage.state == FAILED:
            return f"O ponto de acesso {ssid} está desligado ({stage.detail}). O canal volta quando ele subir."
        return f"Aguardando o {module} ligar o ponto de acesso {ssid}."
    if stage.key == "offer":
        if stage.state == FAILED:
            return "O anúncio BLE da oferta não começou; o módulo tenta de novo a cada rodízio."
        return f"Aguardando o {module} anunciar a oferta por BLE."
    if stage.key == "station":
        text = (
            f"O {module} anuncia a oferta, mas o sensor ainda não entrou na rede {ssid}. O sensor precisa "
            "ouvir a oferta por BLE e se conectar ao ponto de acesso: confira se ele está ligado e ao "
            "alcance do módulo."
        )
        if stage.detail:
            text = f"{stage.detail.capitalize()}. " + text
        return text
    if stage.key == "hello":
        if stage.state == FAILED and reject is not None:
            reason = reject.reason.upper()
            if reason == "KEY":
                return (
                    "O sensor se apresentou, mas a chave dele não confere com a do módulo: os dois foram "
                    "provisionados com chaves mestras diferentes. Reprovisione o sensor."
                )
            if reason == "PROTOCOL":
                return "Sensor e módulo usam versões diferentes do protocolo de enlace; atualize o firmware de um deles."
            if reason == "NOT_BOUND":
                return (
                    f"O sensor está chamando o Módulo {reject.parent_node_id:02d}, que não o tem associado: ele "
                    "guardou um vínculo antigo ou atendeu a oferta de outro módulo. Desassocie e associe de novo."
                )
            if reason == "SEND":
                return "O módulo conferiu o HELLO, mas não conseguiu enviar o CHALLENGE; o sensor vai repetir."
            return f"O HELLO foi recusado: {reason_text(reject.reason)}."
        return f"O sensor entrou na rede {ssid}, mas o HELLO ainda não chegou ao módulo (UDP 4242)."
    if stage.key == "session":
        if stage.state == FAILED:
            return (
                f"O módulo enviou o CHALLENGE e o sensor não respondeu em {HANDSHAKE_TIMEOUT_S} s. Costuma ser "
                "rádio instável (no módulo, Wi-Fi e BLE dividem a mesma antena); o sensor repete o HELLO sozinho."
            )
        return "CHALLENGE enviado; aguardando o CONFIRM do sensor."
    if stage.key == "telemetry":
        return "Sessão autenticada; aguardando a primeira leitura do sensor no CAN."
    return ""


def waiting_summary(sensor: SensorNode, node_events: Sequence[DataLinkEvent] = (), *, now: float | None = None) -> str:
    """Uma linha para a lista de equipamentos: em que etapa o canal está."""

    view = channel_view(sensor, node_events, now=now)
    stage = view.current
    if stage is None:
        return ""
    if stage.state == FAILED:
        text = f"canal de dados: {stage.detail or WAITING_LABELS[stage.key]}"
    else:
        text = f"canal de dados: {WAITING_LABELS[stage.key]}"
    if view.waiting_s is not None and view.waiting_s >= 1:
        text += f", {_duration(view.waiting_s)}"
    return text


def _duration(seconds: float) -> str:
    seconds = int(seconds)
    if seconds < 60:
        return f"há {seconds} s"
    if seconds < 3600:
        minutes, rest = divmod(seconds, 60)
        return f"há {minutes} min {rest:02d} s" if minutes < 10 else f"há {minutes} min"
    return f"há {seconds // 3600} h {seconds % 3600 // 60:02d} min"


def duration(seconds: float) -> str:
    return _duration(seconds)
