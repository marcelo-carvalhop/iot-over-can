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
