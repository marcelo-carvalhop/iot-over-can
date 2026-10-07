"""O catálogo de comandos precisa refletir exatamente o firmware dos nós CAN."""
from __future__ import annotations

import re
from pathlib import Path

import pytest

from pico_tui.command_catalog import ACTIONS, LIVENESS_PERIODS_MS, actions_for, can_command
from pico_tui.security import SecurityManager

ROOT = Path(__file__).resolve().parents[2]


def _firmware_liveness_table() -> dict[int, int]:
    """Lê HB_RATE_* de protocolo.h e o período de main.ino."""

    header = (ROOT / "Codigo/node-can/include/protocolo.h").read_text()
    main = (ROOT / "Codigo/node-can/src/main.ino").read_text()
    codes = {name: int(value, 16) for name, value in re.findall(r"#define (HB_RATE_\w+)\s+0x([0-9A-Fa-f]+)", header)}
    periods = {name: int(ms) for name, ms in re.findall(r"case (HB_RATE_\w+):\s*return (\d+);", main)}
    return {periods[name]: code for name, code in codes.items()}


def test_liveness_labels_match_firmware_periods():
    assert dict(LIVENESS_PERIODS_MS) == _firmware_liveness_table()
    assert can_command("network.liveness", value="2000") == "22 30 FF 01"
    assert can_command("network.liveness", value="500") == "22 30 FF 05"


def test_node_commands_use_hex_target():
    assert can_command("node.status", 4) == "22 20 04 00"
    assert can_command("node.sensor_off", 10) == "22 10 0A 00"
    assert can_command("node.sensor_on", 4) == "22 10 04 11"
    assert can_command("node.clear_fault", 4) == "22 10 04 44"
    with pytest.raises(ValueError):
        can_command("node.status", None)
    with pytest.raises(ValueError):
        can_command("node.status", 0xFF)


def test_protected_flag_matches_security_classification():
    security = SecurityManager("off", "/caminho/inexistente")
    for action in ACTIONS:
        if action.scope in {"network", "node"} and action.key != "network.probe_status":
            raw = can_command(action.key, 4, action.parameter.default if action.parameter else None)
            assert security.command_requires_auth(raw) is action.protected, action.key


def test_direct_only_actions_are_filtered_by_mode():
    gateway = [action.key for action in actions_for("sensor", profile="VIBRATION", mode="GATEWAY_CAN")]
    direct = [action.key for action in actions_for("sensor", profile="VIBRATION", mode="SENSOR_DIRECT")]
    assert "sensor.stop_now" not in gateway
    assert "sensor.stop_now" in direct


def test_every_action_has_plain_language_text():
    for action in ACTIONS:
        assert action.title and action.description
        assert action.title[0].isupper()
        assert not action.title.isupper()
