# Árvore do repositório

```text
iot-over-can/
├── .github/
│   └── workflows/
│       ├── ci.yml
│       └── tui-ci.yml
├── Codigo/
│   ├── node-can/
│   │   ├── include/
│   │   │   ├── can_ids.h
│   │   │   ├── comandos.h
│   │   │   ├── falhas.h
│   │   │   ├── node_config.h
│   │   │   ├── node_types.h
│   │   │   └── protocolo.h
│   │   ├── legacy/
│   │   │   ├── can_ids.h
│   │   │   ├── comandos.cpp
│   │   │   ├── comandos.h
│   │   │   ├── falhas.cpp
│   │   │   ├── falhas.h
│   │   │   ├── geral_v8.ino
│   │   │   ├── node_config.h
│   │   │   ├── node_types.h
│   │   │   └── protocolo.h
│   │   ├── src/
│   │   │   ├── comandos.cpp
│   │   │   ├── falhas.cpp
│   │   │   └── main.ino
│   │   ├── tools/
│   │   │   └── node_id.py
│   │   └── platformio.ini
│   └── node-wifi/
│       ├── tests/
│       │   └── test_config_validation.c
│       ├── battery_monitor.c
│       ├── battery_monitor.h
│       ├── CMakeLists.txt
│       ├── config_validation.c
│       ├── config_validation.h
│       ├── device_identity.c
│       ├── device_identity.h
│       ├── dsp_pipeline.c
│       ├── dsp_pipeline.h
│       ├── edge_network_driver.c
│       ├── edge_network_driver.h
│       ├── edge_protocol_definitions.h
│       ├── lwipopts.h
│       ├── main.c
│       ├── mpu6050_dma_driver.c
│       ├── mpu6050_dma_driver.h
│       ├── pico_sdk_import.cmake
│       ├── serial_console.c
│       └── serial_console.h
├── Documentacao/
│   ├── architecture/
│   │   ├── adr-001-lean-firmware-and-liveness.md
│   │   ├── node-capabilities-and-profile-ui.md
│   │   ├── security-review-implementation-v012.md
│   │   ├── tui-integration-notes.md
│   │   └── tui-network-console.md
│   ├── development/
│   │   └── challenges-and-resolutions.md
│   ├── Front/
│   │   ├── BASELINE_PROTOCOL.md
│   │   ├── CHANGELOG.md
│   │   ├── GATEWAY_PROTOCOL_REFERENCE.md
│   │   ├── INTEGRATION_NOTES.md
│   │   ├── README.md
│   │   └── REQUIREMENTS_COVERAGE.md
│   ├── node-can/
│   │   └── README.md
│   ├── node-wifi/
│   │   ├── CHANGELOG_ASCII_CONSOLE.md
│   │   ├── CHANGELOG_COMPILE_FIX.md
│   │   ├── CHANGELOG_DRDY_NO_DMA.md
│   │   ├── CHANGELOG_DTC_SPAM_FIX.md
│   │   ├── CHANGELOG_FINAL_ACQ_RECOVERY.md
│   │   ├── CHANGELOG_POLLING_BASELINE.md
│   │   ├── CHANGELOG_POLLING_STABLE.md
│   │   ├── CHANGELOG_SECURE_WIFI_TUI_GATE.md
│   │   ├── CHANGELOG_STABLE_DRDY_POLICY.md
│   │   ├── CHANGELOG_STREAM_CONTROL.md
│   │   ├── CHANGELOG_TELEMETRY_FIX.md
│   │   ├── CHANGELOG_TUI_READY.md
│   │   ├── CHANGELOG_WATCHDOG_FIX.md
│   │   ├── GATEWAY_CAN_RECOMMENDATIONS.md
│   │   └── README.md
│   ├── protocol/
│   │   ├── dtc-catalog.md
│   │   ├── gateway-can-recommendations.md
│   │   ├── gateway-protocol-reference.md
│   │   └── serial-baseline-protocol.md
│   ├── build-and-test.md
│   ├── firmware-overview.md
│   ├── first-commit.md
│   ├── front-overview.md
│   ├── github-repository-metadata.md
│   ├── integration-changelog.md
│   ├── project-description.md
│   ├── project-structure.md
│   ├── README-repositorio.md
│   ├── README.md
│   ├── release-v0.12.0.md
│   ├── release-v0.12.1.md
│   ├── requirements-coverage.md
│   ├── security-model.md
│   └── TREE.md
├── Front/
│   ├── pico_tui/
│   │   ├── core/
│   │   │   ├── __init__.py
│   │   │   ├── event_bus.py
│   │   │   ├── events.py
│   │   │   ├── models.py
│   │   │   └── state_store.py
│   │   ├── protocol/
│   │   │   ├── __init__.py
│   │   │   ├── can_id.py
│   │   │   ├── common.py
│   │   │   ├── crc.py
│   │   │   ├── fragments.py
│   │   │   ├── gateway_text.py
│   │   │   ├── legacy_gateway.py
│   │   │   ├── router.py
│   │   │   ├── sensor_direct.py
│   │   │   └── sequence.py
│   │   ├── services/
│   │   │   ├── __init__.py
│   │   │   ├── controller.py
│   │   │   ├── demo.py
│   │   │   └── log_manager.py
│   │   ├── __init__.py
│   │   ├── __main__.py
│   │   ├── app.py
│   │   ├── app.tcss
│   │   ├── commands.py
│   │   ├── dtc_catalog.py
│   │   ├── palette.py
│   │   ├── screens.py
│   │   ├── security.py
│   │   ├── serial_client.py
│   │   └── widgets.py
│   ├── tests/
│   │   ├── __init__.py
│   │   ├── fake_firmware.py
│   │   ├── test_app.py
│   │   ├── test_can_node_capabilities.py
│   │   ├── test_can_node_interaction_contract.py
│   │   ├── test_can_node_screen_contract.py
│   │   ├── test_dtc_catalog.py
│   │   ├── test_firmware_review_contract_v012.py
│   │   ├── test_global_can_commands_contract.py
│   │   ├── test_offline_empty_state_regression.py
│   │   ├── test_offline_startup_contract.py
│   │   ├── test_profile_driven_tui_contract.py
│   │   ├── test_protocol.py
│   │   ├── test_review_v012_integration_contract.py
│   │   ├── test_security_hardening_v012.py
│   │   ├── test_serial_port_markup_regression.py
│   │   └── test_state.py
│   ├── .gitignore
│   ├── pyproject.toml
│   ├── pytest.ini
│   ├── README.md
│   ├── requirements-dev.txt
│   └── requirements.txt
├── scripts/
│   ├── build_esp32_can_node.sh
│   ├── build_pico.sh
│   ├── first_commit.sh
│   ├── provision_sensor_security.sh
│   ├── run_tui.sh
│   ├── setup_tui.sh
│   ├── test_native_firmware.sh
│   ├── test_tui.sh
│   ├── tree.sh
│   └── upload_esp32_can_node.sh
├── .editorconfig
├── .gitattributes
├── .gitignore
├── MANIFEST.json
└── README.md
```
