"""Catálogo declarativo de comandos da TUI.

Cada ação descreve, em linguagem de operador, o que acontece quando é
executada. O catálogo não envia nada: a execução fica em
``PicoTuiApp.execute_action``, que reaproveita os métodos de protocolo já
testados (``_send_network_command``, ``_set_telemetry`` etc.).

Escopos:
    network  rede CAN inteira (não exige alvo)
    node     módulo CAN físico (ID 01..FE)
    sensor   sensor lógico ``parent.child``
"""
from __future__ import annotations

from dataclasses import dataclass, field

# Mapeamento do firmware (Codigo/node-can/include/protocolo.h e main.ino):
#   01 = 2000 ms (HB_RATE_VERY_SLOW) ... 05 = 500 ms (HB_RATE_VERY_FAST)
LIVENESS_PERIODS_MS: tuple[tuple[int, int], ...] = (
    (2000, 0x01),
    (1500, 0x02),
    (1000, 0x03),
    (750, 0x04),
    (500, 0x05),
)


@dataclass(frozen=True, slots=True)
class Choice:
    label: str
    value: str


@dataclass(frozen=True, slots=True)
class Parameter:
    name: str
    label: str
    kind: str = "int"                  # "int" ou "choice"
    default: str = ""
    minimum: int | None = None
    maximum: int | None = None
    unit: str = ""
    choices: tuple[Choice, ...] = ()
    help: str = ""


@dataclass(frozen=True, slots=True)
class CommandAction:
    key: str
    scope: str
    group: str
    title: str
    description: str
    confirm: str = ""
    protected: bool = False
    parameter: Parameter | None = None
    profiles: frozenset[str] = field(default_factory=frozenset)
    modes: frozenset[str] = field(default_factory=frozenset)


GROUP_ORDER = ("Consultar", "Operar", "Manutenção")

_LIVENESS_PARAMETER = Parameter(
    name="period_ms",
    label="Intervalo de presença",
    kind="choice",
    default="1000",
    choices=tuple(
        Choice(f"{period} ms" + (" (padrão)" if period == 1000 else ""), str(period))
        for period, _code in LIVENESS_PERIODS_MS
    ),
    help="Intervalo com que cada módulo confirma que continua ativo na rede.",
)

ACTIONS: tuple[CommandAction, ...] = (
    # ---------------------------------------------------------------- rede
    CommandAction(
        "network.status", "network", "Consultar",
        "Solicitar estado de todos os módulos",
        "Pede a cada módulo CAN que informe papel, estado e capacidades.",
    ),
    CommandAction(
        "network.probe_status", "network", "Consultar",
        "Atualizar dados da Probe 00",
        "Lê versão, barramento e taxa de bits da sonda de instrumentação.",
        modes=frozenset({"GATEWAY_CAN", "DEMO"}),
    ),
    CommandAction(
        "network.election", "network", "Operar",
        "Iniciar eleição de líder",
        "Todos os módulos disputam novamente o papel de líder da rede.",
        confirm="A rede ficará alguns instantes sem líder definido durante a eleição. Continuar?",
        protected=True,
    ),
    CommandAction(
        "network.liveness", "network", "Operar",
        "Ajustar intervalo de presença (liveness)",
        "Define a cada quanto tempo os módulos anunciam que estão ativos.",
        protected=True,
        parameter=_LIVENESS_PARAMETER,
    ),
    # ---------------------------------------------------------------- módulo
    CommandAction(
        "node.status", "node", "Consultar",
        "Solicitar estado do módulo",
        "Pede ao módulo que informe papel, estado e capacidades.",
    ),
    CommandAction(
        "node.sensor_on", "node", "Operar",
        "Reativar sensor local do módulo",
        "Volta a publicar as leituras do sensor local na rede CAN.",
        protected=True,
    ),
    CommandAction(
        "node.sensor_off", "node", "Operar",
        "Desativar sensor local do módulo",
        "O módulo continua na rede, mas deixa de publicar leituras do sensor local.",
        confirm="O módulo deixará de publicar leituras do sensor local até ser reativado. Continuar?",
        protected=True,
    ),
    CommandAction(
        "node.clear_fault", "node", "Manutenção",
        "Limpar falha do módulo",
        "Remove o estado de falha registrado e devolve o módulo à operação normal.",
        confirm="Limpe a falha apenas depois de corrigir a causa. Continuar?",
        protected=True,
    ),
    # ---------------------------------------------------------------- sensor
    CommandAction(
        "sensor.status", "sensor", "Consultar",
        "Atualizar estado do sensor",
        "Lê modo, aquisição, configuração e diagnósticos atuais.",
    ),
    CommandAction(
        "sensor.telemetry_once", "sensor", "Consultar",
        "Ler uma amostra agora",
        "Solicita uma única leitura de telemetria, sem ligar o envio contínuo.",
        protected=True,
    ),
    CommandAction(
        "sensor.fft", "sensor", "Consultar",
        "Ver espectro de frequência (FFT)",
        "Solicita um espectro do buffer atual e abre o gráfico.",
        protected=True,
        profiles=frozenset({"VIBRATION"}),
        parameter=Parameter(
            name="bins",
            label="Resolução do espectro",
            kind="choice",
            default="64",
            choices=(
                Choice("32 faixas", "32"),
                Choice("64 faixas (padrão)", "64"),
                Choice("128 faixas", "128"),
                Choice("256 faixas", "256"),
            ),
            help="Mais faixas mostram mais detalhe, mas aumentam o tráfego na rede.",
        ),
    ),
    CommandAction(
        "sensor.dtc_refresh", "sensor", "Consultar",
        "Atualizar diagnósticos (DTC)",
        "Lê novamente os códigos de diagnóstico ativos no sensor.",
    ),
    CommandAction(
        "sensor.wifi_status", "sensor", "Consultar",
        "Consultar estado do Wi-Fi",
        "Informa se o rádio Wi-Fi do sensor está ligado e vinculado.",
    ),
    CommandAction(
        "sensor.telemetry_on", "sensor", "Operar",
        "Ligar envio contínuo de telemetria",
        "O sensor passa a enviar leituras no intervalo configurado.",
        protected=True,
    ),
    CommandAction(
        "sensor.telemetry_off", "sensor", "Operar",
        "Desligar envio contínuo de telemetria",
        "O sensor para de enviar leituras periódicas; diagnósticos continuam ativos.",
        protected=True,
    ),
    CommandAction(
        "sensor.telemetry_period", "sensor", "Operar",
        "Definir intervalo da telemetria",
        "Tempo entre duas leituras enviadas pelo sensor.",
        protected=True,
        parameter=Parameter(
            name="period_ms",
            label="Intervalo entre leituras",
            kind="int",
            default="1000",
            minimum=50,
            maximum=60000,
            unit="ms",
            help="Valores menores aumentam o tráfego e o consumo de bateria.",
        ),
    ),
    CommandAction(
        "sensor.configure", "sensor", "Operar",
        "Alterar configuração de aquisição",
        "Modo, taxa de amostragem, janela, limiar STA/LTA e ganho.",
        protected=True,
        profiles=frozenset({"VIBRATION"}),
    ),
    CommandAction(
        "sensor.stop_now", "sensor", "Operar",
        "Interromper telemetria imediatamente",
        "Envia Ctrl+C ao console do sensor. Use quando o fluxo de dados travar a serial.",
        modes=frozenset({"SENSOR_DIRECT"}),
    ),
    CommandAction(
        "sensor.restart_acq", "sensor", "Manutenção",
        "Reiniciar aquisição (POLLING)",
        "Reinicia a leitura do acelerômetro no modo operacional POLLING.",
        protected=True,
    ),
    CommandAction(
        "sensor.dtc_clear", "sensor", "Manutenção",
        "Limpar diagnósticos (DTC)",
        "Apaga os códigos ativos. Se a causa persistir, eles voltarão a aparecer.",
        confirm="Apagar todos os diagnósticos ativos deste sensor?",
        protected=True,
    ),
    CommandAction(
        "sensor.wifi_on", "sensor", "Manutenção",
        "Ligar Wi-Fi do sensor",
        "Ativa o rádio Wi-Fi, desligado por padrão por segurança.",
        confirm="O Wi-Fi do sensor ficará ativo e visível na rede local. Continuar?",
        protected=True,
    ),
    CommandAction(
        "sensor.wifi_off", "sensor", "Manutenção",
        "Desligar Wi-Fi do sensor",
        "Desativa o rádio Wi-Fi e mantém o BLE disponível para descoberta.",
        protected=True,
    ),
)

ACTIONS_BY_KEY: dict[str, CommandAction] = {action.key: action for action in ACTIONS}


def actions_for(scope: str, *, profile: str = "", mode: str = "") -> list[CommandAction]:
    """Ações aplicáveis a um alvo, já ordenadas por grupo."""

    profile = (profile or "").upper()
    result = []
    for action in ACTIONS:
        if action.scope != scope:
            continue
        if action.profiles and profile not in action.profiles:
            continue
        if action.modes and mode and mode not in action.modes:
            continue
        result.append(action)
    result.sort(key=lambda item: GROUP_ORDER.index(item.group))
    return result


def liveness_code(period_ms: int) -> int:
    for period, code in LIVENESS_PERIODS_MS:
        if period == period_ms:
            return code
    raise ValueError(f"Intervalo de presença sem código no firmware: {period_ms} ms")


def can_command(action_key: str, node_id: int | None = None, value: str | None = None) -> str:
    """Linha textual ``22 ...`` enviada à Probe 00 para ações de rede e de módulo."""

    if action_key == "network.status":
        return "22 20 FF 00"
    if action_key == "network.election":
        return "22 00 FF 01"
    if action_key == "network.liveness":
        return f"22 30 FF {liveness_code(int(value or 1000)):02X}"
    if node_id is None:
        raise ValueError(f"A ação {action_key} exige um módulo de destino")
    if not 0 < node_id < 0xFF:
        raise ValueError(f"ID de módulo inválido: {node_id}")
    if action_key == "node.status":
        return f"22 20 {node_id:02X} 00"
    if action_key == "node.sensor_off":
        return f"22 10 {node_id:02X} 00"
    if action_key == "node.sensor_on":
        return f"22 10 {node_id:02X} 11"
    if action_key == "node.clear_fault":
        return f"22 10 {node_id:02X} 44"
    raise ValueError(f"A ação {action_key} não gera comando CAN")
