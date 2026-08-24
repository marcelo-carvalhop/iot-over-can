# iot-over-can

**iot-over-can** é uma plataforma experimental de IoT embarcada sobre CAN/CAN FD para estudo de redes locais, supervisão segura, diagnóstico distribuído e compatibilidade entre eventos determinísticos e estocásticos.

O projeto utiliza um sensor de vibração como fonte inicial de eventos físicos, mas o foco principal não é a função sensorial. O foco está na rede: como integrar nós heterogêneos, organizar estados, transportar telemetria, tratar falhas, controlar acesso operacional e observar eventos periódicos e imprevisíveis dentro de uma arquitetura local, desconectada da Internet e orientada a segurança.

## Ideia central

A proposta do **iot-over-can** é usar um caso de uso IoT real — aquisição e supervisão de sinais de vibração — como carga experimental para evoluir uma infraestrutura de comunicação embarcada baseada em CAN/CAN FD. O sensor gera dados, diagnósticos e eventos; a TUI controla e observa; o gateway CAN/CAN FD é o caminho de integração com uma rede distribuída mais ampla.

Assim, o sensor de vibração não define o projeto. Ele é o primeiro gerador de eventos usado para validar a rede.

## Objetivos

- Experimentar uma arquitetura IoT local, offline e controlada.
- Integrar telemetria, diagnóstico e comandos sobre uma rede embarcada.
- Estudar compatibilidade entre eventos determinísticos, como ciclos de supervisão, heartbeats e comandos confirmados, e eventos estocásticos, como variações físicas, saturações, falhas, atrasos e entrada/saída de nós.
- Avaliar mecanismos de segurança operacional, incluindo ativação explícita de conectividade e proteção de comandos sensíveis na TUI.
- Criar uma base evolutiva para disciplinas de IoT, sistemas distribuídos embarcados e futuro TCC.

## Estrutura

```text
firmware/
  pico-edge-sensor/       Firmware do Raspberry Pi Pico 2 W + MPU6050
  esp32-can-legacy/       Base CAN/ESP32 legada preservada para evolução do gateway

software/
  tui/                    TUI Python/Textual com controle serial, segurança e comandos do sensor

docs/
  protocol/               Contratos seriais, CAN e gateway
  architecture/           Notas de integração e arquitetura
  build-and-test.md       Instruções de build e testes
  security-model.md       Modelo de segurança operacional

scripts/
  build_pico.sh           Build limpo do firmware Pico
  run_tui.sh              Execução da TUI em modo sensor direto
  test_tui.sh             Testes/checagem Python da TUI
  first_commit.sh         Inicialização do repositório Git
```

## Baseline técnica atual

```text
Aquisição do sensor: POLLING
DRDY/INT: desativado na baseline, reservado como experimento futuro
Sensor inicial: MPU6050
Pico SDA: GP0
Pico SCL: GP1
Pico INT/DRDY: GP2, legado/experimental
Telemetria: serial ASCII
Wi-Fi: desabilitado no boot; ativação apenas por comando da TUI/operador
Segurança: TUI com modo de presença/OTP para YubiKey
Bateria: opcional; 255 e 65535 representam N/A
```

Estado saudável esperado no sensor:

```text
STATUS ... ACQ=POLLING ... DTC=0x0000 DTC_COUNT=0 ... DRDY_IRQ=0 DRDY_MISSED=0
```

## Fluxo rápido

Build do firmware do Pico:

```bash
./scripts/build_pico.sh
```

Rodar TUI em modo sensor direto:

```bash
./scripts/run_tui.sh /dev/ttyACM0
```

Testar TUI:

```bash
./scripts/test_tui.sh
```

Primeiro commit:

```bash
./scripts/first_commit.sh
```

## Segurança operacional

A conexão Wi-Fi do nó sensor não deve ocorrer automaticamente no boot. Ela deve depender de uma escolha explícita do operador pela TUI. A TUI pode exigir presença de YubiKey ou OTP para comandos sensíveis, reforçando a proposta de sistema offline, controlado e com superfície de ataque reduzida.

## Caminho de evolução

A versão atual fecha a baseline do sensor e da TUI. A próxima etapa natural é transformar a base ESP32/CAN legada em um gateway CAN FD dedicado, preferencialmente em uma nova pasta futura:

```text
firmware/esp32-canfd-gateway/
```

Esse gateway será o elo entre sensores IoT locais, supervisão segura e rede distribuída determinística sobre CAN/CAN FD.
