# Estrutura do projeto

A organização separa interface, firmware, scripts operacionais e documentação oficial.

```text
iot-over-can/
├── Front/
│   ├── pico_tui/
│   │   ├── core/
│   │   ├── protocol/
│   │   ├── services/
│   │   ├── app.py
│   │   ├── screens.py
│   │   ├── dialogs.py
│   │   ├── widgets.py
│   │   ├── presentation.py
│   │   ├── command_catalog.py
│   │   ├── spectrum.py
│   │   ├── preferences.py
│   │   ├── palette.py
│   │   ├── security.py
│   │   ├── serial_client.py
│   │   └── app.tcss
│   ├── tests/
│   ├── pyproject.toml
│   └── README.md
├── Codigo/
│   ├── common/
│   │   ├── ioc_link/
│   │   └── tests/
│   ├── node-can/
│   │   ├── include/
│   │   ├── src/
│   │   ├── test/host/
│   │   ├── tools/
│   │   └── platformio.ini
│   ├── node-wifi/
│   │   ├── tests/
│   │   ├── CMakeLists.txt
│   │   └── fontes do firmware
│   └── scripts/
├── Documentacao/
│   ├── arquitetura/
│   │   ├── arquitetura.md
│   │   └── plano-de-dados-wireless.md
│   ├── protocolo/protocolo.md
│   ├── interface/
│   │   ├── tui.md
│   │   └── img/
│   ├── modulo-can/modulo-can.md
│   ├── sensor-wireless/sensor-wireless.md
│   └── documentos gerais
├── .github/workflows/
├── README.md
└── MANIFEST.json
```

## `Front/`

Contém a TUI e seus testes. `pyproject.toml` é a fonte única das dependências Python.

Responsabilidades principais:

| Arquivo/pasta | Responsabilidade |
|---|---|
| `app.py` | ciclo de vida, navegação, autorização e execução de ações |
| `screens.py` | telas cheias de contexto |
| `dialogs.py` | decisões modais curtas |
| `widgets.py` | componentes responsivos e sem truncamento |
| `presentation.py` | regras de condição, rótulos e formatação |
| `command_catalog.py` | catálogo declarativo de comandos |
| `spectrum.py` | representação FFT |
| `preferences.py` | preferências não sensíveis |
| `palette.py` | identidade Aço industrial |
| `security.py` | autorização da TUI |
| `core/` | modelos, eventos e store |
| `protocol/` | decodificação dos contratos seriais/CAN |
| `services/` | domínio, demonstração e persistência de log |
| `tests/` | testes automatizados e contratos |

## `Codigo/common/`

Código C portátil compartilhado pelos dois firmwares e testado no computador.

| Arquivo | Responsabilidade |
|---|---|
| `ioc_link/ioc_sha256.*` | SHA-256 e HMAC-SHA256 |
| `ioc_link/ioc_link.*` | derivação de chaves, oferta de vínculo, envelope autenticado e handshake |
| `ioc_link/ioc_wdata.*` | segmentação no CAN e formatos de telemetria, espectro, confirmação, DTC, enlace e configuração |
| `tests/test_ioc_link.c` | testes nativos e vetores para conferência com Python |

## `Codigo/node-can/`

Firmware dos ESP32 + MCP2515. Inclui eleição, liveness CAN, função local, descoberta BLE, associação wireless, publicação dos estados do vínculo e o plano de dados dos sensores associados (`wireless_link.cpp`). `test/host/` contém a simulação que executa `wireless_link.cpp` no computador com rádio, UDP e CAN substituídos. O diretório local `legacy/`, quando existir, é ignorado e não integra build/repositório público.

## `Codigo/node-wifi/`

Firmware do Raspberry Pi Pico W: MPU6050, aquisição, DSP, DTC, BLE advertising e varredura de ofertas, console serial, sessão autenticada com o Node (`edge_network_driver.c`, `edge_link.c`) e segurança. `edge_protocol_definitions.h` é a definição única dos payloads do sensor e também é incluído pelo firmware dos Nodes.

## `Codigo/scripts/`

Scripts de setup da TUI, testes, build, upload e provisionamento. Não contém scripts de inicialização de repositório Git.

## `Documentacao/`

É a fonte pública oficial. Cada assunto possui um documento principal. Imagens específicas da interface ficam em `Documentacao/interface/img/`; não existe árvore paralela `Documentacao/Front`.
