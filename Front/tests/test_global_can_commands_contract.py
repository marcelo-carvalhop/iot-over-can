from pathlib import Path

from pico_tui.command_catalog import ACTIONS_BY_KEY, actions_for, can_command

ROOT = Path(__file__).resolve().parents[1] / "pico_tui"


def test_network_actions_include_election_and_global_status():
    keys = [action.key for action in actions_for("network", mode="GATEWAY_CAN")]
    assert "network.election" in keys
    assert "network.status" in keys
    assert can_command("network.election") == "22 00 FF 01"
    assert can_command("network.status") == "22 20 FF 00"
    assert ACTIONS_BY_KEY["network.election"].confirm


def test_election_command_does_not_require_selected_node():
    source = (ROOT / "app.py").read_text()
    assert 'cmd in {"election", "eleicao", "eleição"}' in source
    assert 'await self._send_network_command("Iniciar eleição da rede", "22 00 FF 01")' in source


def test_can_without_node_opens_network_commands():
    source = (ROOT / "app.py").read_text()
    assert "if node is None:" in source
    assert 'self.open_commands("network")' in source
