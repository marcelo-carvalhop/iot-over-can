from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FRONT = ROOT / "Front" / "pico_tui"


def read(name: str) -> str:
    return (FRONT / name).read_text(encoding="utf-8")


def test_v016_exposes_wireless_management_as_visible_global_screen() -> None:
    app = read("app.py")
    screens = read("screens.py")
    assert 'Binding("f7", "show_wireless"' in app
    assert 'Binding("w", "show_wireless"' in app
    assert 'class WirelessScreen(BaseScreen)' in screens
    assert 'F7/w", "Sensores sem fio"' in app


def test_v016_keeps_ctrl_a_only_as_compatibility_alias() -> None:
    app = read("app.py")
    assert 'Binding("ctrl+a", "show_wireless"' in app
    assert "Compatibilidade com a interface anterior" in app


def test_v016_supports_explicit_bind_and_unbind_flows() -> None:
    app = read("app.py")
    dialogs = read("dialogs.py")
    assert "async def request_wireless_bind" in app
    assert "async def request_wireless_unbind" in app
    assert 'wireless_association("BIND"' in app
    assert 'wireless_association("UNBIND"' in app
    assert "class WirelessNodeScreen" in dialogs
    assert "aguardando UNBOUND" in app


def test_v016_does_not_offer_nonfunctional_wireless_data_plane_actions() -> None:
    app = read("app.py")
    screens = read("screens.py")
    assert "plano de dados wireless indisponível" in app
    assert "wireless_without_data_plane" in screens
    # v0.17: as ações voltam quando o módulo informa sessão autenticada.
    assert "not sensor.has_data_plane" in screens
    assert "Telemetria, FFT e configuração voltam a ser oferecidas" in screens


def test_v016_sensor_screen_describes_logical_link_without_calling_it_telemetry() -> None:
    screens = read("screens.py")
    assert '"Vínculo wireless"' in screens
    assert '"RSSI do vínculo"' in screens
    assert '"Plano de dados"' in screens
    assert '"Aguardando sessão autenticada com o módulo"' in screens
    assert 'Binding("d", "unbind"' in screens


def test_v016_uses_single_full_screen_subject_architecture() -> None:
    screens = read("screens.py")
    for klass in (
        "HomeScreen",
        "SensorScreen",
        "NodeScreen",
        "CommandScreen",
        "NetworkScreen",
        "MessagesScreen",
        "WirelessScreen",
        "FftScreen",
        "HelpScreen",
    ):
        assert f"class {klass}" in screens


def test_v016_version_is_consistent() -> None:
    pyproject = (ROOT / "Front" / "pyproject.toml").read_text(encoding="utf-8")
    init = (FRONT / "__init__.py").read_text(encoding="utf-8")
    assert 'version = "0.17.0"' in pyproject
    assert '__version__ = "0.17.0"' in init
