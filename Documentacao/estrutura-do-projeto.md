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
│   ├── node-can/
│   │   ├── include/
│   │   ├── src/
│   │   ├── tools/
│   │   └── platformio.ini
│   ├── node-wifi/
│   │   ├── tests/
│   │   ├── CMakeLists.txt
│   │   └── fontes do firmware
│   └── scripts/
├── Documentacao/
│   ├── arquitetura/arquitetura.md
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

## `Codigo/node-can/`

Firmware dos ESP32 + MCP2515. Inclui eleição, liveness CAN, função local, descoberta BLE, associação wireless e publicação dos estados do vínculo. O diretório local `legacy/`, quando existir, é ignorado e não integra build/repositório público.

## `Codigo/node-wifi/`

Firmware do Raspberry Pi Pico W: MPU6050, aquisição, DSP, DTC, BLE advertising, console serial, rede e segurança.

## `Codigo/scripts/`

Scripts de setup da TUI, testes, build, upload e provisionamento. Não contém scripts de inicialização de repositório Git.

## `Documentacao/`

É a fonte pública oficial. Cada assunto possui um documento principal. Imagens específicas da interface ficam em `Documentacao/interface/img/`; não existe árvore paralela `Documentacao/Front`.
