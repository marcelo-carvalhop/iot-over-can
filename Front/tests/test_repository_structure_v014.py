from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_operational_scripts_live_under_codigo():
    assert (ROOT / "Codigo" / "scripts" / "run_tui.sh").exists()
    assert (ROOT / "Codigo" / "scripts" / "build_pico.sh").exists()
    assert not (ROOT / "scripts").exists()
    assert not (ROOT / "Codigo" / "scripts" / "first_commit.sh").exists()


def test_documentation_is_consolidated_in_portuguese_paths():
    expected = [
        "README.md",
        "LICENSE",
        "Documentacao/guia-de-uso.md",
        "Documentacao/arquitetura.md",
        "Documentacao/protocolo.md",
        "Documentacao/integracao.md",
        "Documentacao/tui.md",
        "Documentacao/sensor-acustico.md",
        "Documentacao/seguranca.md",
        "Documentacao/validacao.md",
        "Documentacao/desafios.md",
    ]
    for path in expected:
        assert (ROOT / path).exists(), path
    # Um documento por assunto, todos em Documentacao/; o que é antigo fica em historico/.
    current = sorted(p.name for p in (ROOT / "Documentacao").glob("*.md"))
    assert current == sorted(Path(p).name for p in expected if p.startswith("Documentacao/"))
    assert not (ROOT / "Documentacao" / "architecture").exists()
    assert not (ROOT / "Documentacao" / "protocol").exists()


def test_reader_carries_every_document():
    # O leitor da documentação (Codigo/leitor) leva o README e cada documento de Documentacao/.
    import importlib.util
    import sys

    spec = importlib.util.spec_from_file_location("leitor_documentos", ROOT / "Codigo/leitor/tools/documentos.py")
    documentos = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = documentos  # as dataclasses do módulo procuram o próprio módulo aqui
    spec.loader.exec_module(documentos)
    carried = sorted(path for path, _title, _summary in documentos.DOCUMENTOS)
    expected = sorted(["README.md"] + [f"Documentacao/{p.name}" for p in (ROOT / "Documentacao").glob("*.md")])
    assert carried == expected


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
