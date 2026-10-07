from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FW = ROOT / "Codigo" / "node-wifi"


def test_no_hardcoded_legacy_wifi_credentials_or_static_sensor_ip() -> None:
    source = (FW / "edge_network_driver.c").read_text()
    assert "industrial_dsp_secure" not in source
    assert "CAN_EDGE_GATEWAY_01" not in source
    assert "192, 168, 4, 2" not in source
    assert "dhcp_start" in source
    lwipopts = (FW / "lwipopts.h").read_text()
    assert "#define LWIP_DHCP                   1" in lwipopts


def test_configuration_validation_is_shared() -> None:
    validator = (FW / "config_validation.c").read_text()
    network = (FW / "edge_network_driver.c").read_text()
    serial = (FW / "serial_console.c").read_text()
    main = (FW / "main.c").read_text()
    assert "edge_config_validate" in validator
    assert "edge_config_validate(&cfg" in network
    assert "edge_config_validate(&g_staged" in serial
    assert "main_rate_to_u32" in main


def test_uuid64_and_secure_build_defaults() -> None:
    protocol = (FW / "edge_protocol_definitions.h").read_text()
    cmake = (FW / "CMakeLists.txt").read_text()
    identity = (FW / "device_identity.c").read_text()
    assert "uint64_t         node_uuid" in protocol
    # v0x06: envelope autenticado por HMAC-SHA256 substitui o CLAIM da v0x05.
    assert "NET_PROTOCOL_VERSION    0x06" in protocol
    assert 'set(EDGE_SERIAL_ADMIN_TOKEN "0"' in cmake
    # Sem chave fornecida no build o enlace fica desabilitado (fail-closed).
    assert 'set(EDGE_LINK_DEVICE_KEY ""' in cmake
    assert 'set(EDGE_LINK_MASTER_KEY ""' in cmake
    assert "pico_get_unique_board_id" in identity


def test_unauthenticated_udp_control_path_was_removed() -> None:
    cmake = (FW / "CMakeLists.txt").read_text()
    network = (FW / "edge_network_driver.c").read_text()
    protocol = (FW / "edge_protocol_definitions.h").read_text()
    # O caminho legado (chave compartilhada de 64 bits e mutação UDP sem MAC)
    # não existe mais, nem como opção de build.
    for text in (cmake, network, protocol):
        assert "EDGE_NODE_PRESHARED_KEY" not in text
        assert "EDGE_ALLOW_LEGACY_INSECURE_UDP_CONTROL" not in text
    assert "legacy_udp_control_enabled" not in network
    # Todo datagrama é autenticado e passa pelo contador anti-replay antes de
    # chegar ao tratamento de comandos.
    assert "ioc_env_verify(g_session_key" in network
    assert "ioc_counter_accept(&g_rx_last_counter" in network
    assert network.index("ioc_counter_accept(&g_rx_last_counter") < network.index(
        "handle_session_command(pcb, view.payload"
    )
    assert "request_counter" in network
