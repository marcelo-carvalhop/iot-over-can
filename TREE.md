# Árvore do repositório

```text
iot-over-can/
├── .github/
│   └── workflows/
│       └── tui-ci.yml
├── docs/
│   ├── architecture/
│   │   └── tui-integration-notes.md
│   ├── protocol/
│   │   ├── gateway-can-recommendations.md
│   │   ├── gateway-protocol-reference.md
│   │   └── serial-baseline-protocol.md
│   ├── build-and-test.md
│   ├── first-commit.md
│   ├── github-repository-metadata.md
│   ├── integration-changelog.md
│   ├── project-description.md
│   ├── project-structure.md
│   ├── README.md
│   ├── requirements-coverage.md
│   └── security-model.md
├── firmware/
│   ├── esp32-can-legacy/
│   │   ├── can_ids.h
│   │   ├── comandos.cpp
│   │   ├── comandos.h
│   │   ├── falhas.cpp
│   │   ├── falhas.h
│   │   ├── geral_v8.ino
│   │   ├── node_config.h
│   │   ├── node_types.h
│   │   └── protocolo.h
│   ├── pico-edge-sensor/
│   │   ├── battery_monitor.c
│   │   ├── battery_monitor.h
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
│   │   ├── CMakeLists.txt
│   │   ├── dsp_pipeline.c
│   │   ├── dsp_pipeline.h
│   │   ├── edge_network_driver.c
│   │   ├── edge_network_driver.h
│   │   ├── edge_protocol_definitions.h
│   │   ├── GATEWAY_CAN_RECOMMENDATIONS.md
│   │   ├── lwipopts.h
│   │   ├── main.c
│   │   ├── mpu6050_dma_driver.c
│   │   ├── mpu6050_dma_driver.h
│   │   ├── pico_sdk_import.cmake
│   │   ├── README.md
│   │   ├── serial_console.c
│   │   └── serial_console.h
│   └── README.md
├── scripts/
│   ├── build_pico.sh
│   ├── first_commit.sh
│   ├── run_tui.sh
│   ├── test_tui.sh
│   └── tree.sh
├── software/
│   ├── tui/
│   │   ├── pico_tui/
│   │   │   ├── core/
│   │   │   │   ├── __init__.py
│   │   │   │   ├── event_bus.py
│   │   │   │   ├── events.py
│   │   │   │   ├── models.py
│   │   │   │   └── state_store.py
│   │   │   ├── protocol/
│   │   │   │   ├── __init__.py
│   │   │   │   ├── can_id.py
│   │   │   │   ├── common.py
│   │   │   │   ├── crc.py
│   │   │   │   ├── fragments.py
│   │   │   │   ├── gateway_text.py
│   │   │   │   ├── legacy_gateway.py
│   │   │   │   ├── router.py
│   │   │   │   ├── sensor_direct.py
│   │   │   │   └── sequence.py
│   │   │   ├── services/
│   │   │   │   ├── __init__.py
│   │   │   │   ├── controller.py
│   │   │   │   ├── demo.py
│   │   │   │   └── log_manager.py
│   │   │   ├── __init__.py
│   │   │   ├── __main__.py
│   │   │   ├── app.py
│   │   │   ├── app.tcss
│   │   │   ├── commands.py
│   │   │   ├── dtc_catalog.py
│   │   │   ├── palette.py
│   │   │   ├── screens.py
│   │   │   ├── security.py
│   │   │   ├── serial_client.py
│   │   │   └── widgets.py
│   │   ├── tests/
│   │   │   ├── __init__.py
│   │   │   ├── fake_firmware.py
│   │   │   ├── test_app.py
│   │   │   ├── test_protocol.py
│   │   │   └── test_state.py
│   │   ├── .gitignore
│   │   ├── BASELINE_PROTOCOL.md
│   │   ├── CHANGELOG.md
│   │   ├── GATEWAY_PROTOCOL_REFERENCE.md
│   │   ├── INTEGRATION_NOTES.md
│   │   ├── pyproject.toml
│   │   ├── pytest.ini
│   │   ├── README.md
│   │   ├── requirements-dev.txt
│   │   ├── requirements.txt
│   │   └── REQUIREMENTS_COVERAGE.md
│   └── README.md
├── .editorconfig
├── .gitattributes
├── .gitignore
├── MANIFEST.json
├── README.md
└── TREE.md
```
