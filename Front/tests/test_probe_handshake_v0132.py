from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_probe_firmware_accepts_textual_introspection_without_can_injection():
    source = (ROOT / "Codigo" / "node-can" / "src" / "comandos.cpp").read_text()
    assert 'strcmp(command, "PROBE_VERSION") == 0' in source
    assert 'strcmp(command, "PROBE_STATUS") == 0' in source
    assert 'strcmp(command, "GW_VERSION") == 0' in source
    assert 'PROBE_VERSION FIRMWARE=0.13.2' in source
    assert 'PROBE_STATUS NODE=0 STATE=ONLINE CAN=CLASSIC' in source
    assert source.index("handleProbeIntrospectionCommand(buffer)") < source.index("sscanf(")


def test_tui_uses_probe_names_and_keeps_gateway_alias_compatibility():
    app = (ROOT / "Front" / "pico_tui" / "app.py").read_text()
    router = (ROOT / "Front" / "pico_tui" / "protocol" / "router.py").read_text()
    decoder = (ROOT / "Front" / "pico_tui" / "protocol" / "gateway_text.py").read_text()
    assert 'self._send_raw("PROBE_VERSION")' in app
    assert 'self._send_raw("PROBE_STATUS")' in app
    assert '"PROBE_",' in router
    assert '{"PROBE_VERSION", "GW_VERSION"}' in decoder
    assert '{"PROBE_STATUS", "GW_STATUS"}' in decoder


def test_probe_zero_does_not_advertise_ble_scan_capability():
    source = (ROOT / "Codigo" / "node-can" / "src" / "main.ino").read_text()
    assert '[PROBE 00] CAPS=CAN_MONITOR,SERIAL_INSTRUMENTATION LOCAL_PROFILE=NONE' in source
