"""Gravação da sessão serial no formato das gravações da bancada virtual.

Cada linha recebida vira ``@<ms> <linha>``, com o tempo desde o início da
gravação; os comandos enviados entram como comentários (``# t=<ms> comando:``).
O arquivo abre com ``python -m pico_tui --replay <arquivo>`` e pode virar
caso de teste, como as gravações de ``Front/replays``. O cabeçalho guarda o
modo pedido na conexão (``modo=gateway``, ``sensor`` ou ``auto``), que a
reprodução respeita.

Segredos não vão para o disco: o argumento de ``AUTH ...`` e os valores de
chaves do tipo ``*KEY*=``, ``*TOKEN*=``, ``*SECRET*=``, ``*PASS*=`` e ``*PIN*=``
são trocados por ``***``. Uma falha de escrita (disco cheio, permissão) só
encerra a gravação; a TUI continua.
"""
from __future__ import annotations

import re
import time
from datetime import UTC, datetime
from pathlib import Path
from typing import TextIO

_SECRET_VALUE = re.compile(r"(?i)\b([A-Z0-9_]*(?:KEY|TOKEN|SECRET|PASS|PIN)[A-Z0-9_]*)=(\S+)")
_AUTH_ARGUMENT = re.compile(r"(?i)\b(AUTH\s+\S+)\s+\S+")
_MODE = re.compile(r"\bmodo=(\w+)")


def redact(text: str) -> str:
    text = _SECRET_VALUE.sub(lambda match: f"{match.group(1)}=***", text)
    return _AUTH_ARGUMENT.sub(r"\1 ***", text)


def recorded_mode(lines: list[str], default: str = "gateway") -> str:
    """Modo gravado no cabeçalho; as gravações da bancada virtual não têm (gateway)."""

    for line in lines[:5]:
        if line.startswith("#") and (match := _MODE.search(line)):
            return match.group(1).lower()
    return default


class SessionRecorder:
    FLUSH_EVERY = 50

    def __init__(self, path: Path, *, port: str, mode: str) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self.path = path
        self._file: TextIO | None = path.open("w", encoding="utf-8")
        self._start = time.monotonic()
        self._pending = 0
        self._write(
            f"# iot-over-can: sessão gravada em {datetime.now(UTC).astimezone().isoformat(timespec='seconds')}, "
            f"porta {port}, modo={mode.lower()}\n"
        )

    @property
    def active(self) -> bool:
        return self._file is not None

    def _ms(self) -> int:
        return 1 + int((time.monotonic() - self._start) * 1000.0)

    def line(self, text: str) -> None:
        text = text.rstrip("\r\n")
        if text:
            self._write(f"@{self._ms()} {redact(text)}\n")

    def command(self, text: str) -> None:
        self._write(f"# t={self._ms()} comando: {redact(text.strip())}\n")

    def _write(self, data: str) -> None:
        if self._file is None:
            return
        try:
            self._file.write(data)
            self._pending += 1
            if self._pending >= self.FLUSH_EVERY:
                self._file.flush()
                self._pending = 0
        except OSError:
            self.close()

    def close(self) -> None:
        file, self._file = self._file, None
        if file is not None:
            try:
                file.close()
            except OSError:
                pass
