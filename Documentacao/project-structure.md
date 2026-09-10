# Estrutura do repositório

A estrutura do projeto foi reorganizada para seguir o padrão acadêmico da disciplina
de IoT II. A separação principal é feita entre interface, firmware e documentação.

```text
iot-over-can/
├── Front/
│   ├── pico_tui/
│   ├── tests/
│   ├── pyproject.toml
│   ├── requirements.txt
│   └── README.md
│
├── Codigo/
│   ├── node-can/
│   │   ├── include/
│   │   ├── src/
│   │   ├── tools/
│   │   ├── platformio.ini
│   │   └── legacy/
│   │
│   └── node-wifi/
│       ├── main.c
│       ├── edge_network_driver.c
│       ├── edge_protocol_definitions.h
│       ├── dsp_pipeline.c
│       ├── serial_console.c
│       ├── CMakeLists.txt
│       └── tests/
│
├── Documentacao/
│   ├── architecture/
│   ├── protocol/
│   ├── development/
│   ├── Front/
│   ├── node-can/
│   ├── node-wifi/
│   └── demais documentos do projeto
│
├── scripts/
├── .github/
├── README.md
└── MANIFEST.json
```

## Front

A pasta `Front/` contém a TUI de supervisão, diagnóstico, configuração e
instrumentação da rede. O código Python, testes e metadados de empacotamento
permanecem juntos para permitir instalação com `pip install -e Front`.

## Codigo/node-can

Contém o firmware principal dos módulos ESP32 conectados ao barramento CAN.
A implementação PlatformIO atual fica diretamente nessa pasta. O código
histórico anterior foi preservado em `Codigo/node-can/legacy/` apenas para
rastreabilidade e comparação.

## Codigo/node-wifi

Contém o firmware do sensor remoto baseado em Raspberry Pi Pico 2 W. Esse nó
representa o dispositivo IoT wireless que futuramente será descoberto pelos
módulos CAN e associado ao melhor ponto de entrada escolhido pelo operador.

## Documentacao

Centraliza toda a documentação produzida durante a evolução do projeto,
incluindo arquitetura, protocolos, segurança, DTCs, histórico de integração,
registros de desafios, releases e documentação anteriormente distribuída
junto aos firmwares e à TUI.

O `README.md` da raiz é mantido por convenção do GitHub e existe também uma
cópia de referência em `Documentacao/README-repositorio.md`. O
`Front/README.md` permanece junto ao pacote Python porque é referenciado pelo
`pyproject.toml`; sua cópia integral também está em `Documentacao/Front/README.md`.
