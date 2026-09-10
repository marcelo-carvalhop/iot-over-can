# Build e testes

## Firmware Pico

```bash
cd Codigo/node-wifi
rm -rf build
mkdir build
cd build
cmake -DPICO_BOARD=pico2_w ..
cmake --build . -j$(nproc)
```

Teste serial esperado no boot:

```text
STATUS
```

Resultado esperado:

```text
STATUS NET=DISABLED ... ACQ=POLLING ... DTC=0x0000 ... DRDY_IRQ=0 DRDY_MISSED=0
```

Para habilitar Wi-Fi manualmente:

```text
NET WIFI ON
NET WIFI STATUS
```

Para desligar:

```text
NET WIFI OFF
```

Comandos mutáveis do sensor exigem autorização também no firmware. Para bancada/manutenção direta:

```text
AUTH STATUS
AUTH UNLOCK <token-64-bit>
AUTH LOCK
```

`RESET` exige confirmação:

```text
RESET
RESET CONFIRM
```

## TUI

```bash
cd Front
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -e .
```

Execução com YubiKey por presença:

```bash
iot-over-can-tui --port /dev/ttyACM0 --mode sensor --security-mode presence
```

Execução de desenvolvimento sem proteção:

```bash
iot-over-can-tui --port /dev/ttyACM0 --mode sensor --security-mode off
```

Comandos internos relevantes:

```text
:wifi on
:wifi off
:wifi status
:security
:lock
:security
:lock
```

## Verificações da finalização v0.12.0

Na finalização desta baseline foram executados:

```text
Python compileall: PASS
Testes Python sem runtime Textual: 50 passed, 1 deselected
Teste C nativo de config_validation: PASS
```

O teste desmarcado importa a camada visual `Textual`, indisponível no ambiente de empacotamento. Os testes de contrato, protocolo, estado, segurança e integração que não dependem dessa biblioteca foram executados.

O build completo do Pico contra o Pico SDK real e o build PlatformIO do ESP32 não foram executados localmente nesta finalização porque as respectivas toolchains não estão disponíveis no ambiente. O workflow de CI contém jobs específicos para esses builds e eles devem ser confirmados no runner ou no ambiente de desenvolvimento antes de marcar uma versão de firmware como validada em hardware.

## ESP32 CAN Node — PlatformIO

Build:

```bash
./scripts/build_esp32_can_node.sh
```

Ou diretamente:

```bash
cd Codigo/node-can
pio run
```

Upload:

```bash
pio run -t upload
```

Monitor serial:

```bash
pio device monitor -b 115200
```

Observação: esta sessão não validou o build PlatformIO porque o ambiente de execução não possui PlatformIO instalado.

## Seleção de porta serial

A TUI não presume mais `/dev/ttyACM0`. Execute sem argumento para abrir o seletor de portas:

```bash
./scripts/run_tui.sh
```

Ou informe uma porta explicitamente:

```bash
./scripts/run_tui.sh /dev/ttyUSB0
```

Em runtime, use `:connect` para abrir novamente o seletor ou `:connect /dev/ttyUSB0 gateway` para trocar diretamente.

## NODE_ID por build/upload no PlatformIO

Build do nó CAN com ID 1:

```bash
./scripts/build_esp32_can_node.sh 1
```

Upload do nó CAN com ID 2 em uma porta específica:

```bash
./scripts/upload_esp32_can_node.sh 2 /dev/ttyUSB0
```

Alternativa direta:

```bash
cd Codigo/node-can
IOT_NODE_ID=3 pio run -t upload --upload-port /dev/ttyUSB0
```

## TUI offline-first

Instalação única:

```bash
./scripts/setup_tui.sh
```

Execução sem hardware:

```bash
./scripts/run_tui.sh
```

A interface deve abrir em estado `DISCONNECTED`.

Para conectar depois:

```text
F3
:connect
```

Não é necessário executar `pip install -e .` a cada inicialização.
