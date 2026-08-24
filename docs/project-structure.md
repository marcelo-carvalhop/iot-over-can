# Estrutura do pacote

```text
edge_project_integrated_secure_v1/
├── sensor_pico_polling_secure/
│   ├── main.c
│   ├── edge_network_driver.c/h
│   ├── serial_console.c/h
│   └── demais arquivos do firmware Pico
├── tui_secure/
│   ├── pico_tui/
│   │   ├── app.py
│   │   ├── security.py
│   │   ├── commands.py
│   │   └── protocol/
│   └── README.md
├── esp32_can_legacy_base/
│   ├── geral_v8.ino
│   ├── comandos.cpp/h
│   ├── falhas.cpp/h
│   └── protocolo/can ids
└── docs/
    ├── INTEGRATION_CHANGELOG.md
    ├── SECURITY_MODEL.md
    ├── BUILD_AND_TEST.md
    └── PROJECT_STRUCTURE.md
```
