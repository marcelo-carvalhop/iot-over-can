from pathlib import Path


def test_run_script_does_not_hardcode_serial_port():
    root = Path(__file__).resolve().parents[2]
    script = (root / "scripts" / "run_tui.sh").read_text()
    assert '/dev/ttyACM0}"' not in script
    assert 'PORT="${1:-}"' in script


def test_run_script_does_not_reinstall_every_start():
    root = Path(__file__).resolve().parents[2]
    script = (root / "scripts" / "run_tui.sh").read_text()
    assert 'command -v iot-over-can-tui' in script
    assert 'exec iot-over-can-tui' in script


def test_offline_first_boot_contract():
    app_source = (Path(__file__).resolve().parents[1] / "pico_tui" / "app.py").read_text()
    assert "Política offline-first" in app_source
    assert "TUI iniciada sem conexão serial" in app_source
    assert "_open_connection_dialog(exit_on_cancel=True)" not in app_source
