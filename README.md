# iot-over-can

**iot-over-can** é uma plataforma experimental de IoT embarcada sobre CAN/CAN FD para estudo de redes locais, supervisão segura, diagnóstico distribuído e compatibilidade entre eventos determinísticos e estocásticos.

O projeto utiliza um sensor de vibração como fonte inicial de eventos físicos, mas o foco principal não é a função sensorial. O foco está na rede: como integrar nós heterogêneos, organizar estados, transportar telemetria, tratar falhas, controlar acesso operacional e observar eventos periódicos e imprevisíveis dentro de uma arquitetura local, desconectada da Internet e orientada a segurança.

## Ideia central

A proposta do **iot-over-can** é usar um caso de uso IoT real — aquisição e supervisão de sinais de vibração — como carga experimental para evoluir uma infraestrutura de comunicação embarcada baseada em CAN/CAN FD. O sensor gera dados, diagnósticos e eventos; a TUI controla e observa; os módulos CAN funcionais são os pontos distribuídos de integração entre sensores remotos e a infraestrutura CAN/CAN FD existente.

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
  Codigo/node-wifi/       Firmware do Raspberry Pi Pico W + MPU6050
  esp32-can-legacy/       Base CAN/ESP32 legada preservada como referência histórica

software/
  tui/                    TUI Python/Textual para rede, módulos CAN, sensores remotos e instrumentação

Documentacao/
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

## Baseline enxuta

```text
Aquisição do sensor: POLLING
DRDY/INT: desativado na baseline, reservado como experimento futuro
Sensor inicial: MPU6050
Pico SDA: GP0
Pico SCL: GP1
Pico INT/DRDY: GP2, legado/experimental
Telemetria: serial ASCII
Wi-Fi: desabilitado no boot; ativação apenas por comando da TUI/operador
Segurança: presença YubiKey operacional; OTP permanece fail-closed até validação criptográfica real
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

Build do nó CAN ESP32 via PlatformIO:

```bash
./scripts/build_esp32_can_node.sh
```

Rodar TUI em modo sensor direto:

```bash
./scripts/run_tui.sh
```

Testar TUI:

```bash
./scripts/test_tui.sh
```

Primeiro commit:

```bash
./scripts/first_commit.sh
```


## Política de eventos e liveness

A baseline enxuta remove eventos programados de erro e o modo sintético de bancada. O sensor deve gerar apenas eventos reais: telemetria, saturação, DTCs de aquisição, comunicação, energia e diagnóstico.

O antigo heartbeat acadêmico foi reduzido a um **liveness lease**. Ele não é usado como sincronização lógica pesada nem como exigência de TDMA; serve apenas para indicar presença de aplicação quando a telemetria estiver desligada ou sob demanda.


## DTCs objetivos

A TUI usa um catálogo explícito de DTCs em `Documentacao/protocol/dtc-catalog.md`. Códigos conhecidos do firmware não devem aparecer como “falha desconhecida”. Quando um código futuro ainda não estiver catalogado, a TUI usa fallback por categoria e exibe o código bruto em hexadecimal.

## Firmware CAN em PlatformIO

O firmware dos nós CAN ESP32 + MCP2515 foi reorganizado em:

```text
Codigo/node-can/
```

Essa passa a ser a pasta recomendada para evolução do firmware dos nós CAN. A pasta `Codigo/node-can/legacy/` permanece preservada como referência histórica.

## Segurança operacional

O sensor remoto não depende de acesso direto do operador. Na arquitetura alvo, BLE advertising permanece disponível para descoberta e a conexão Wi-Fi só é provisionada depois que o operador escolhe, pela TUI, qual módulo CAN assumirá o vínculo. A TUI protege as ações do operador e o firmware do sensor aplica sua própria autorização para comandos mutáveis de manutenção.

## Caminho de evolução

A arquitetura alvo não possui gateway wireless central. O `Node 00` é apenas uma **Probe 00** de instrumentação, invisível à lógica distribuída. Cada módulo CAN funcional poderá acumular capacidades independentes de seu papel `LEADER/FOLLOWER`, incluindo sensor local, scanner BLE, ponto de acesso Wi-Fi e ponte para sensores wireless próximos.

O próximo estágio é implementar descoberta BLE distribuída nos módulos CAN: o sensor remoto anuncia seu UUID/perfil, vários nós reportam RSSI pela CAN, o operador escolhe o nó mais adequado pela TUI e somente esse nó inicia o vínculo. Depois da associação, o sensor recebe um identificador lógico `parent.child` e sua interface especializada passa a existir na TUI.

## Seleção de porta serial na TUI

O script `./scripts/run_tui.sh` não força mais `/dev/ttyACM0`. Quando executado sem argumento, a TUI abre uma tela de conexão com as portas detectadas. Para conectar diretamente em uma porta específica:

```bash
./scripts/run_tui.sh /dev/ttyUSB0
```

Dentro da TUI, use o menu `Conexão serial / trocar porta` ou o comando interno:

```text
:connect
:connect /dev/ttyUSB0 gateway
:connect /dev/ttyACM0 sensor
:disconnect
:reconnect
```

## Console de rede CAN

A TUI passa a ter uma tela principal genérica para validação da rede CAN/CAN FD. A telemetria de vibração deixou de ser a tela principal e passa a aparecer como detalhe especializado quando um sensor de vibração lógico é selecionado.

Níveis de seleção:

```text
Rede CAN completa
└── Módulo CAN físico, por exemplo Node 04
    └── Sensor lógico/wireless, por exemplo 04.01
```

Use F4 com um módulo CAN selecionado para enviar comandos/configurações ao nó. Use F4 com um sensor selecionado para abrir a configuração especializada do sensor.

## NODE_ID no upload PlatformIO

O firmware dos nós CAN aceita o ID por variável de build:

```bash
./scripts/build_esp32_can_node.sh 1
./scripts/upload_esp32_can_node.sh 1 /dev/ttyUSB0
```

Assim não é mais necessário alterar o código-fonte, salvar e recompilar manualmente para cada placa.

## Inicialização da TUI sem hardware

A TUI usa política **offline-first**. Nenhuma porta serial é obrigatória para abrir a interface.

Primeira instalação:

```bash
./scripts/setup_tui.sh
```

Execução normal:

```bash
./scripts/run_tui.sh
```

Com nenhum dispositivo conectado, a TUI abre em `DISCONNECTED`. A conexão pode ser feita depois com F3, pelo menu principal ou com `:connect`.

Se uma porta for informada e falhar, a aplicação permanece aberta em modo offline em vez de encerrar.

## Comandos globais da rede CAN

A eleição e a consulta global de status são ações da rede, portanto não exigem selecionar um módulo.

Na TUI:

```text
Menu > Comandos da Rede CAN
Ctrl+L
:can
:election
:canstatus
```

Comandos principais:

```text
22 00 FF 01   iniciar eleição
22 20 FF 00   solicitar status global
22 30 FF 01   liveness rápido
22 30 FF 03   liveness normal
22 30 FF 05   liveness lento
```

Comandos administrativos direcionados, como desativar ou reativar um nó, continuam disponíveis na tela exclusiva do módulo CAN selecionado.

## Nó CAN como unidade edge

Cada módulo CAN possui duas dimensões independentes:

```text
role: LEADER / FOLLOWER
capabilities: CAN, LOCAL_SENSOR, LOCAL_SENSOR_DEMO, ...
```

Na baseline atual, o valor `0xAA` é tratado como dado do sensor local de demonstração do módulo:

```text
LOCAL_PROFILE=DEMO_BYTE
LOCAL_SENSOR_DEMO_VALUE=0xAA
```

Esse valor não cria um sensor wireless fictício na TUI.

A tela exclusiva de cada módulo CAN mostra estado, papel, capacidades, sensor local, estado wireless e sensores wireless associados. Recursos wireless (`WIFI_AP`, `BLE_SCAN`, associação por beacon) estão representados no modelo, mas o firmware de discovery/associação ainda é a próxima etapa de implementação.

A interface especializada de vibração é orientada por perfil e só fica disponível quando um sensor `PROFILE=VIBRATION` está selecionado.


## Baseline v0.12 — hardening da fronteira IoT

A revisão de código de 09/09/2026 foi incorporada à baseline. As mudanças centrais são: validação única de configuração, identidade UUID64 derivada do dispositivo, autorização também no firmware do Pico, remoção de SSID/senha/chaves fixas do código, DHCP, jitter de descoberta, token de sessão de 64 bits, contador simples anti-replay, DTCs de segurança mais específicos e CI de firmware/TUI.

Antes do primeiro build seguro do Pico:

```bash
./scripts/provision_sensor_security.sh
./scripts/build_pico.sh
```

O arquivo `.env.local` gerado contém segredos locais de build e é ignorado pelo Git. A TUI usa `~/.config/iot-over-can/security.json`, que deve ter permissões `0600`.

O caminho UDP antigo permanece somente como compatibilidade de bancada e suas operações mutáveis ficam desabilitadas por padrão. Ele não é considerado a solução final de associação wireless. A próxima etapa é BLE advertising no sensor, scan BLE nos nós CAN e seleção do nó de associação pela TUI.

## Registro de desafios

O histórico de problemas encontrados, respectivas causas, correções aplicadas e itens ainda abertos é mantido em `Documentacao/development/challenges-and-resolutions.md`. O registro é parte da documentação técnica da baseline e deve ser atualizado sempre que uma mudança de arquitetura, protocolo, segurança, TUI ou processo de build resolver um novo problema relevante.

## Estrutura acadêmica do repositório

O projeto utiliza a estrutura exigida para a disciplina:

```text
Front/                 TUI
Codigo/node-can/       firmware dos módulos CAN
Codigo/node-wifi/      firmware do sensor wireless
Documentacao/          documentação consolidada
```

As demais pastas de apoio do repositório, como `scripts/` e `.github/`,
permanecem na raiz.

Para instalar a TUI:

```bash
./scripts/setup_tui.sh
```

Para executá-la:

```bash
./scripts/run_tui.sh
```

Para compilar o nó CAN:

```bash
./scripts/build_esp32_can_node.sh 1
```

Para compilar o nó wireless:

```bash
./scripts/build_pico.sh
```


### Descoberta BLE — v0.13.0

A baseline atual implementa descoberta distribuída: o Pico W anuncia UUID/perfil por
BLE; cada módulo CAN funcional mede RSSI e reporta o candidato pelo CAN clássico; a
Probe 00 apenas observa e entrega a informação à TUI. Associação Wi-Fi e criação de
subnó lógico ainda não fazem parte desta versão.
