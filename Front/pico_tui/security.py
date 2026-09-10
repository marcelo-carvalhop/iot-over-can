from __future__ import annotations

import json
import os
import time
from dataclasses import dataclass
from pathlib import Path

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
    "ACQ ",
    "DTC CLEAR",
    "RESET",
    "NET WIFI ON",
    "NET WIFI OFF",
    "NET WIFI PROVISION ",
    "NET WIFI CLEAR",
    "AUTH UNLOCK ",
    "WIFI ",
    "CMD ",
)


@dataclass(slots=True)
class SecurityDecision:
    allowed: bool
    reason: str = ""


class SecurityManager:
    """Operational authorization for the TUI, fail-closed on configuration errors.

    ``presence`` remains an operational physical-presence policy. It is not
    cryptographic proof of a specific YubiKey. ``otp`` is intentionally
    fail-closed until a real OTP/FIDO verifier is integrated; the previous
    prefix/length check was not authentication.
    """

    def __init__(self, mode: str = "presence", config_path: str | Path | None = None, unlock_seconds: int = 300) -> None:
        self.mode = (mode or "presence").lower()
        if self.mode not in {"off", "presence", "otp"}:
            self.mode = "presence"
        default_path = Path.home() / ".config" / "iot-over-can" / "security.json"
        legacy_path = Path.home() / ".config" / "pico_tui" / "security.json"
        if config_path:
            self.config_path = Path(config_path)
        elif default_path.exists() or not legacy_path.exists():
            self.config_path = default_path
        else:
            self.config_path = legacy_path
        self.unlock_seconds = unlock_seconds
        self._otp_public_ids: set[str] = set()
        self._device_admin_token: str = ""
        self._unlocked_until = 0.0
        self._last_reason = ""
        self._config_error = ""
        self._last_unlock_attempt = 0.0
        self._load_config()

    def _load_config(self) -> None:
        self._config_error = ""
        if not self.config_path.exists():
            return
        try:
            st = self.config_path.stat()
            if os.name == "posix":
                if hasattr(os, "getuid") and st.st_uid != os.getuid():
                    raise PermissionError("security.json não pertence ao usuário atual")
                if st.st_mode & 0o077:
                    raise PermissionError("security.json deve usar permissões 0600")
            data = json.loads(self.config_path.read_text())
            if not isinstance(data, dict):
                raise ValueError("security.json deve conter um objeto JSON")
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            self._config_error = str(exc)
            self._last_reason = f"Configuração de segurança inválida: {exc}"
            return

        ids = data.get("otp_public_ids", [])
        if isinstance(ids, list):
            self._otp_public_ids = {str(item).strip().lower() for item in ids if str(item).strip()}
        if isinstance(data.get("unlock_seconds"), int):
            self.unlock_seconds = max(30, min(3600, int(data["unlock_seconds"])))
        token = str(data.get("device_admin_token", "")).strip()
        if token:
            try:
                parsed = int(token, 0)
                if parsed <= 0 or parsed > 0xFFFFFFFFFFFFFFFF:
                    raise ValueError
                self._device_admin_token = hex(parsed)
            except ValueError:
                self._config_error = "device_admin_token deve ser inteiro hexadecimal/decimal de 64 bits não-zero"
                self._last_reason = self._config_error

    @property
    def last_reason(self) -> str:
        return self._last_reason

    @property
    def config_error(self) -> str:
        return self._config_error

    @property
    def device_admin_token(self) -> str:
        return self._device_admin_token

    @property
    def unlocked(self) -> bool:
        if self._config_error:
            return False
        if self.mode == "off":
            return True
        if self.mode == "presence":
            return self.yubikey_present()
        if self.mode == "otp":
            return time.monotonic() < self._unlocked_until
        return False

    def status_label(self) -> str:
        if self._config_error:
            return "SEC=CONFIG_ERROR"
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
        if cmd in {"AUTH STATUS", "AUTH LOCK"}:
            return False
        if cmd.startswith("NET ") and cmd not in {"NET WIFI ON", "NET WIFI OFF"} and not cmd.startswith("NET WIFI PROVISION") and cmd != "NET WIFI CLEAR":
            return False
        return any(cmd == prefix.strip() or cmd.startswith(prefix) for prefix in MUTATING_PREFIXES)

    def authorize_command(self, command: str) -> SecurityDecision:
        if not self.command_requires_auth(command):
            return SecurityDecision(True)
        if self._config_error:
            return SecurityDecision(False, self._last_reason or "Configuração de segurança inválida.")
        if self.mode == "off":
            return SecurityDecision(True, "TUI em modo sem proteção; firmware ainda pode exigir token local.")
        if self.mode == "presence":
            if self.yubikey_present():
                return SecurityDecision(True)
            self._last_reason = "YubiKey não detectada. Comando bloqueado."
            return SecurityDecision(False, self._last_reason)
        if self.mode == "otp":
            self._last_reason = "Modo OTP bloqueado: verificação criptográfica Yubico/FIDO2 ainda não implementada."
            return SecurityDecision(False, self._last_reason)
        self._last_reason = "Modo de segurança inválido."
        return SecurityDecision(False, self._last_reason)

    def unlock_with_otp(self, otp: str) -> SecurityDecision:
        del otp
        now = time.monotonic()
        if now - self._last_unlock_attempt < 2.0:
            return SecurityDecision(False, "Aguarde antes de uma nova tentativa de desbloqueio.")
        self._last_unlock_attempt = now
        if self.mode != "otp":
            return SecurityDecision(False, "Modo OTP não está ativo.")
        self._last_reason = "OTP local desativado: a validação anterior por prefixo/tamanho não era criptográfica."
        return SecurityDecision(False, self._last_reason)

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
            vid = getattr(port, "vid", None)
            if isinstance(vid, int) and vid == int(YUBICO_USB_VENDOR_ID, 16):
                return True
    except (OSError, AttributeError):
        return False
    return False
