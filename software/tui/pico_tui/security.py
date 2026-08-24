from __future__ import annotations

import json
import os
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

try:
    from serial.tools import list_ports
except Exception:  # pragma: no cover - pyserial dependency may be absent in docs builds
    list_ports = None  # type: ignore[assignment]


YUBICO_USB_VENDOR_ID = "1050"


READ_ONLY_COMMANDS = (
    "STATUS",
    "GET",
    "VERSION",
    "PING",
    "NET",
    "NET WIFI STATUS",
    "DTC",
)

MUTATING_PREFIXES = (
    "SET ",
    "APPLY",
    "TELEMETRY ",
    "FFT ",
    "SIMULATE ",
    "ACQ ",
    "DTC CLEAR",
    "RESET",
    "NET WIFI ON",
    "NET WIFI OFF",
    "WIFI ",
    "CMD ",
)


@dataclass(slots=True)
class SecurityDecision:
    allowed: bool
    reason: str = ""


class SecurityManager:
    """Proteção operacional local da TUI.

    Modos:
    - off: não bloqueia comandos.
    - presence: exige YubiKey USB presente para comandos mutáveis.
    - otp: exige desbloqueio temporário por OTP/local token configurado.

    Esta camada protege a operação da TUI e reforça a aparência/rotina de
    segurança física. Para autenticação criptográfica forte, use uma etapa
    futura com FIDO2/HMAC challenge-response.
    """

    def __init__(self, mode: str = "presence", config_path: str | Path | None = None, unlock_seconds: int = 300) -> None:
        self.mode = (mode or "presence").lower()
        if self.mode not in {"off", "presence", "otp"}:
            self.mode = "presence"
        self.config_path = Path(config_path) if config_path else Path.home() / ".config" / "pico_tui" / "security.json"
        self.unlock_seconds = unlock_seconds
        self._otp_public_ids: set[str] = set()
        self._unlocked_until = 0.0
        self._last_reason = ""
        self._load_config()

    def _load_config(self) -> None:
        try:
            data = json.loads(self.config_path.read_text())
        except Exception:
            return
        if isinstance(data, dict):
            ids = data.get("otp_public_ids", [])
            if isinstance(ids, list):
                self._otp_public_ids = {str(item).strip().lower() for item in ids if str(item).strip()}
            if isinstance(data.get("unlock_seconds"), int):
                self.unlock_seconds = int(data["unlock_seconds"])

    @property
    def last_reason(self) -> str:
        return self._last_reason

    @property
    def unlocked(self) -> bool:
        if self.mode == "off":
            return True
        if self.mode == "presence":
            return self.yubikey_present()
        if self.mode == "otp":
            return time.monotonic() < self._unlocked_until
        return False

    def status_label(self) -> str:
        if self.mode == "off":
            return "SEC=OFF"
        if self.mode == "presence":
            return "SEC=YUBIKEY" if self.yubikey_present() else "SEC=LOCKED"
        if self.mode == "otp":
            remaining = int(max(0.0, self._unlocked_until - time.monotonic()))
            return f"SEC=OTP:{remaining}s" if remaining else "SEC=LOCKED"
        return "SEC=UNKNOWN"

    def command_requires_auth(self, command: str) -> bool:
        cmd = " ".join(command.strip().upper().split())
        if not cmd:
            return False
        if cmd in READ_ONLY_COMMANDS:
            return False
        if cmd.startswith("NET ") and cmd not in {"NET WIFI ON", "NET WIFI OFF"}:
            return False
        return any(cmd == prefix.strip() or cmd.startswith(prefix) for prefix in MUTATING_PREFIXES)

    def authorize_command(self, command: str) -> SecurityDecision:
        if not self.command_requires_auth(command):
            return SecurityDecision(True)

        if self.mode == "off":
            return SecurityDecision(True)

        if self.mode == "presence":
            if self.yubikey_present():
                return SecurityDecision(True)
            self._last_reason = "YubiKey não detectada. Comando bloqueado."
            return SecurityDecision(False, self._last_reason)

        if self.mode == "otp":
            if self.unlocked:
                return SecurityDecision(True)
            self._last_reason = "TUI bloqueada. Use :unlock <OTP-da-YubiKey>."
            return SecurityDecision(False, self._last_reason)

        self._last_reason = "Modo de segurança inválido."
        return SecurityDecision(False, self._last_reason)

    def unlock_with_otp(self, otp: str) -> SecurityDecision:
        token = (otp or "").strip().lower()
        if self.mode != "otp":
            return SecurityDecision(False, "Modo OTP não está ativo.")
        if len(token) < 16:
            return SecurityDecision(False, "OTP curto demais.")

        if self._otp_public_ids:
            public_id = token[:12]
            if public_id not in self._otp_public_ids:
                return SecurityDecision(False, "OTP de YubiKey não cadastrada.")
        else:
            # Para bancada: permite uso sem arquivo de cadastro, mas ainda exige
            # um token físico digitado pelo operador. Documentado como proteção
            # operacional, não como autenticação criptográfica completa.
            if len(token) < 32:
                return SecurityDecision(False, "Configure otp_public_ids ou use um OTP completo.")

        self._unlocked_until = time.monotonic() + self.unlock_seconds
        return SecurityDecision(True, f"TUI desbloqueada por {self.unlock_seconds}s.")

    def lock(self) -> None:
        self._unlocked_until = 0.0

    def yubikey_present(self) -> bool:
        return _yubikey_present_sysfs() or _yubikey_present_serial_ports()


def _yubikey_present_sysfs() -> bool:
    root = Path("/sys/bus/usb/devices")
    if not root.exists():
        return False
    try:
        for path in root.glob("*/idVendor"):
            try:
                if path.read_text().strip().lower() == YUBICO_USB_VENDOR_ID:
                    return True
            except OSError:
                continue
    except OSError:
        return False
    return False


def _yubikey_present_serial_ports() -> bool:
    if list_ports is None:
        return False
    try:
        for port in list_ports.comports():
            fields: Iterable[str] = (
                str(getattr(port, "vid", "") or ""),
                str(getattr(port, "manufacturer", "") or ""),
                str(getattr(port, "product", "") or ""),
                str(getattr(port, "description", "") or ""),
                str(getattr(port, "hwid", "") or ""),
            )
            joined = " ".join(fields).lower()
            if "yubico" in joined or "yubikey" in joined or "1050" in joined:
                return True
    except Exception:
        return False
    return False
