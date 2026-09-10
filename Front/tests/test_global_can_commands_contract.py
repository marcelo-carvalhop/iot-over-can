from pathlib import Path


ROOT = Path(__file__).resolve().parents[1] / "pico_tui"


def test_global_can_command_screen_has_election():
    source = (ROOT / "screens.py").read_text()
    assert "class CanNetworkCommandScreen" in source
    assert '("Iniciar eleição da rede", "22 00 FF 01")' in source
    assert '("Solicitar status global", "22 20 FF 00")' in source


def test_election_command_does_not_require_selected_node():
    source = (ROOT / "app.py").read_text()
    assert 'cmd in {"election", "eleicao", "eleição"}' in source
    assert 'await self._send_network_command("Iniciar eleição da rede", "22 00 FF 01")' in source


def test_can_without_node_opens_global_commands():
    source = (ROOT / "app.py").read_text()
    assert "if node is None:" in source
    assert "push_screen_wait(CanNetworkCommandScreen())" in source
