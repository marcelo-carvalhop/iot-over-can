from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_operational_scripts_live_under_codigo():
    assert (ROOT / "Codigo" / "scripts" / "run_tui.sh").exists()
    assert (ROOT / "Codigo" / "scripts" / "build_pico.sh").exists()
    assert not (ROOT / "scripts").exists()
    assert not (ROOT / "Codigo" / "scripts" / "first_commit.sh").exists()


def test_documentation_is_consolidated_in_portuguese_paths():
    expected = [
        "Documentacao/arquitetura/arquitetura.md",
        "Documentacao/protocolo/protocolo.md",
        "Documentacao/build_e_teste.md",
        "Documentacao/modelo_de_seguranca.md",
        "Documentacao/estrutura-do-projeto.md",
        "Documentacao/descrição-do-projeto.md",
    ]
    for path in expected:
        assert (ROOT / path).exists(), path
    assert not (ROOT / "Documentacao" / "architecture").exists()
    assert not (ROOT / "Documentacao" / "protocol").exists()


def test_front_dependencies_have_single_source():
    pyproject = (ROOT / "Front" / "pyproject.toml").read_text()
    assert '"textual==8.2.8"' in pyproject
    assert '"rich==15.0.0"' in pyproject
    assert '"pyserial==3.5"' in pyproject
    assert not (ROOT / "Front" / "requirements.txt").exists()
    assert not (ROOT / "Front" / "requirements-dev.txt").exists()


def test_local_only_history_is_ignored():
    ignore = (ROOT / ".gitignore").read_text()
    assert "Codigo/node-can/legacy/" in ignore
    assert "Documentacao/release-v*.md" in ignore


def test_tui_uses_full_screen_contexts_and_visible_wireless_management():
    screens = (ROOT / "Front" / "pico_tui" / "screens.py").read_text()
    app = (ROOT / "Front" / "pico_tui" / "app.py").read_text()
    for klass in (
        "HomeScreen",
        "NodeScreen",
        "SensorScreen",
        "CommandScreen",
        "NetworkScreen",
        "MessagesScreen",
        "WirelessScreen",
        "HelpScreen",
    ):
        assert f"class {klass}" in screens
    assert 'Binding("f7", "show_wireless"' in app
    assert 'Binding("w", "show_wireless"' in app
    assert 'class NetworkTreePanel' not in screens
    assert 'class NodeTelemetryPanel' not in screens
