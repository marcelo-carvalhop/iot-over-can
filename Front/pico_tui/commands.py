from __future__ import annotations

import secrets

DEFAULT_FSM_MODES = ["IDLE", "ROTATING", "STRUCTURAL", "SEISMIC"]
DEFAULT_WINDOW_TYPES = ["RECT", "HANN", "HAMMING", "FLATTOP", "BLACKMAN"]
DEFAULT_WINDOW_SIZES = (128, 256, 512)
TELEMETRY_PROFILES = {"LOW": 1, "NORMAL": 2, "FAST": 5, "BURST": 10}
FFT_BIN_OPTIONS = (32, 64, 128, 256)

DIRECT_COMMANDS = (
    "STATUS",
    "GET",
    "SET MODE IDLE",
    "SET MODE ROTATING",
    "SET MODE STRUCTURAL",
    "SET MODE SEISMIC",
    "SET RATE <4..1000>",
    "SET WINDOW RECT",
    "SET WINDOW HANN",
    "SET WINDOW HAMMING",
    "SET WINDOW FLATTOP",
    "SET WINDOW BLACKMAN",
    "SET WINDOW_SIZE 128",
    "SET WINDOW_SIZE 256",
    "SET WINDOW_SIZE 512",
    "SET STALTA <valor>",
    "SET GAIN <valor>",
    "APPLY",
    "TELEMETRY ON",
    "TELEMETRY OFF",
    "TELEMETRY ONCE",
    "TELEMETRY FAST",
    "TELEMETRY SLOW",
    "TELEMETRY PERIOD <ms>",
    "FFT ONCE",
    "ACQ POLLING",
    "DTC",
    "DTC CLEAR",
    "NET",
    "NET WIFI STATUS",
    "NET WIFI ON",
    "NET WIFI OFF",
    "VERSION",
    "PING",
    "RESET",
)

REMOVED_DIRECT_COMMANDS = (
    "ACQ DRDY",
    "SLEEP LIGHT",
    "SLEEP SENSOR",
    "SLEEP DEEP",
    "WAKE REASON",
)


def transaction_id() -> str:
    return secrets.token_hex(3).upper()


def direct_status() -> str:
    return "STATUS"


def direct_get() -> str:
    return "GET"


def direct_apply() -> str:
    return "APPLY"


def direct_set(field: str, value: object) -> str:
    return f"SET {field.upper()} {value}"


def direct_telemetry(state: str, value: object | None = None) -> str:
    command = f"TELEMETRY {state.upper()}"
    return f"{command} {value}" if value is not None else command



def direct_acq_polling() -> str:
    return "ACQ POLLING"


def direct_dtc_clear() -> str:
    return "DTC CLEAR"


def direct_fft_once() -> str:
    return "FFT ONCE"


def direct_wifi(state: str) -> str:
    state = state.upper()
    if state in {"ON", "OFF"}:
        return f"NET WIFI {state}"
    return "NET WIFI STATUS"


def gateway_command(target: str, action: str, *, tx: str | None = None, **fields: object) -> str:
    items = ["CMD", f"TARGET={target}", f"ACTION={action.upper()}", f"TX={tx or transaction_id()}"]
    for key, value in fields.items():
        if value is None:
            continue
        items.append(f"{key.upper()}={value}")
    return " ".join(items)


def wireless_association(action: str, node_id: int, wireless_uuid: str, option: str | None = None) -> str:
    """Comando de associação aceito pela Probe 00.

    BIND aceita a opção AUTO (o líder pode reassociar o sensor a outro módulo
    quando o responsável sair do ar ou deixar de alcançá-lo). POLICY altera a
    política de um vínculo existente e exige AUTO ou MANUAL.
    """

    action = action.upper()
    if action not in {"BIND", "UNBIND", "POLICY"}:
        raise ValueError(f"Ação wireless inválida: {action}")
    option = option.upper() if option else None
    if action == "POLICY" and option not in {"AUTO", "MANUAL"}:
        raise ValueError("POLICY exige AUTO ou MANUAL")
    if action == "BIND" and option not in {None, "AUTO"}:
        raise ValueError(f"Opção inválida para BIND: {option}")
    if action == "UNBIND":
        option = None
    command = f"WIRELESS {action} {node_id} {wireless_uuid}"
    return f"{command} {option}" if option else command


FAULT_KINDS = ("POWER_CYCLE", "SILENCE", "AP_OUTAGE", "DROP_SESSION")


def fault(kind: str, node_id: int, seconds: int = 0) -> str:
    """Ensaio de injeção de falha em um módulo (ver metrics/experiments.py)."""

    kind = kind.upper()
    if kind not in FAULT_KINDS:
        raise ValueError(f"Ensaio inválido: {kind}")
    if not 1 <= node_id <= 31:
        raise ValueError(f"Módulo inválido: {node_id}")
    if kind == "DROP_SESSION":
        return f"FAULT DROP_SESSION {node_id}"
    if not 1 <= seconds <= 120:
        raise ValueError("Duração de 1 a 120 s")
    return f"FAULT {kind} {node_id} {seconds}"


def fault_load(percent: int, seconds: int, priority: str = "HIGH") -> str:
    priority = priority.upper()
    if priority not in {"HIGH", "LOW"}:
        raise ValueError(f"Prioridade inválida: {priority}")
    if not 1 <= percent <= 90:
        raise ValueError("Carga de 1 a 90 %")
    if not 1 <= seconds <= 120:
        raise ValueError("Duração de 1 a 120 s")
    return f"FAULT LOAD {percent} {seconds} {priority}"


def fault_cancel(node_id: int | None = None) -> str:
    return f"FAULT CANCEL {node_id}" if node_id else "FAULT CANCEL"


def metrics(state: str) -> str:
    state = state.upper()
    if state not in {"ON", "OFF", "RESET"}:
        raise ValueError(f"Estado inválido: {state}")
    return f"METRICS {state}"
