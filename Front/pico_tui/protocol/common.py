from __future__ import annotations

import math
import shlex
from typing import Any


def parse_key_values(line: str) -> dict[str, str]:
    """Extrai CHAVE=VALOR sem depender da ordem.

    DATA/PAYLOAD podem aparecer como sequência hexadecimal sem aspas, por
    exemplo ``DATA=01 02 03``; tokens seguintes são agregados até surgir
    outra chave.
    """
    result: dict[str, str] = {}
    try:
        parts = shlex.split(line, posix=True)
    except ValueError:
        parts = line.split()
    current_key: str | None = None
    for part in parts[1:]:
        if "=" in part:
            key, value = part.split("=", 1)
            current_key = key.strip().upper()
            result[current_key] = value.strip()
        elif current_key in {"DATA", "PAYLOAD", "VALUES"}:
            result[current_key] = f"{result[current_key]} {part}".strip()
        else:
            current_key = None
    return result


def parse_int(value: str | int | None, default: int | None = None) -> int | None:
    if value is None:
        return default
    if isinstance(value, int):
        return value
    try:
        return int(value, 0)
    except (TypeError, ValueError):
        return default


def parse_float(value: str | float | int | None, default: float | None = None) -> float | None:
    if value is None:
        return default
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return default
    return parsed if math.isfinite(parsed) else default


def parse_bool(value: str | bool | None, default: bool | None = None) -> bool | None:
    if isinstance(value, bool):
        return value
    if value is None:
        return default
    normalized = value.strip().upper()
    if normalized in {"1", "YES", "SIM", "TRUE", "ON", "ACTIVE", "ATIVO"}:
        return True
    if normalized in {"0", "NO", "NAO", "NÃO", "FALSE", "OFF", "INACTIVE", "INATIVO"}:
        return False
    return default


def first(payload: dict[str, Any], *names: str) -> Any:
    for name in names:
        if name.upper() in payload:
            return payload[name.upper()]
    return None
