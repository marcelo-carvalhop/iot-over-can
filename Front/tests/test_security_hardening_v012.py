from __future__ import annotations

import json
import os
from pathlib import Path

from pico_tui.security import SecurityManager


def _write_secure(path: Path, payload: dict) -> None:
    path.write_text(json.dumps(payload))
    if os.name == "posix":
        path.chmod(0o600)


def test_device_admin_token_is_loaded_from_secure_file(tmp_path: Path) -> None:
    cfg = tmp_path / "security.json"
    _write_secure(cfg, {"device_admin_token": "0x1234", "unlock_seconds": 300})
    sec = SecurityManager("off", cfg)
    assert sec.config_error == ""
    assert sec.device_admin_token == "0x1234"


def test_insecure_permissions_fail_closed(tmp_path: Path) -> None:
    if os.name != "posix":
        return
    cfg = tmp_path / "security.json"
    cfg.write_text('{"device_admin_token":"0x1234"}')
    cfg.chmod(0o644)
    sec = SecurityManager("presence", cfg)
    assert sec.config_error
    assert sec.unlocked is False
    assert sec.status_label() == "SEC=CONFIG_ERROR"


def test_otp_mode_no_longer_accepts_prefix_or_length_only(tmp_path: Path) -> None:
    cfg = tmp_path / "security.json"
    _write_secure(cfg, {"otp_public_ids": ["cccccccccccc"]})
    sec = SecurityManager("otp", cfg)
    result = sec.unlock_with_otp("cccccccccccc" + "x" * 40)
    assert result.allowed is False
    assert "não" in result.reason.lower() or "desativado" in result.reason.lower()


def test_wifi_provisioning_and_auth_unlock_are_mutating() -> None:
    sec = SecurityManager("off", "/path/that/does/not/exist")
    assert sec.command_requires_auth("NET WIFI PROVISION node-2 secretpass") is True
    assert sec.command_requires_auth("NET WIFI CLEAR") is True
    assert sec.command_requires_auth("AUTH UNLOCK 0x1234") is True
