# Integração segura — revisão 1

## Bases integradas

Esta revisão organiza o projeto em três blocos:

```text
sensor_pico_polling_secure/   firmware Pico 2 W + MPU6050
tui_secure/                   TUI Textual com proteção operacional
esp32_can_legacy_base/        base CAN distribuída anterior
```

## Decisão principal

O firmware do Pico permanece em `ACQ=POLLING` como baseline oficial. O DRDY continua fora do ciclo atual.

## Mudanças funcionais no firmware do Pico

- Wi-Fi não conecta automaticamente no boot.
- Estado inicial da rede: `NET=DISABLED`.
- Comandos novos:

```text
NET WIFI STATUS
NET WIFI ON
NET WIFI OFF
```

- `NET WIFI ON` inicia a pilha CYW43, conecta ao Soft-AP esperado e entra em `DISCOVERY`.
- `NET WIFI OFF` remove o PCB UDP, limpa sessão e volta para `DISABLED`.
- `FFT ONCE` agora entra na condição de processamento de buffer, mesmo sem telemetria contínua.

## Mudanças funcionais na TUI

- Adicionada camada `SecurityManager`.
- `--security-mode presence` exige YubiKey USB presente para comandos mutáveis.
- `--security-mode otp` permite desbloqueio temporário com `:unlock <otp>`.
- `--security-mode off` desativa a proteção operacional.
- Comandos internos novos:

```text
:wifi on
:wifi off
:wifi status
:security
:unlock <otp>
:lock
```

- Atalhos novos:

```text
Ctrl+W  Wi-Fi ON
Ctrl+Y  estado da segurança
```

## Comandos protegidos

A camada de segurança bloqueia comandos mutáveis, incluindo:

```text
SET ...
APPLY
TELEMETRY ...
FFT ...
SIMULATE ...
ACQ ...
DTC CLEAR
RESET
NET WIFI ON
NET WIFI OFF
CMD ...
```

Comandos de leitura, como `STATUS`, `GET`, `VERSION`, `PING`, `NET`, `NET WIFI STATUS` e `DTC`, permanecem permitidos.

## Limite assumido

A base `esp32_can_legacy_base` ainda é a rede CAN clássica/legada. Ela foi preservada para integração posterior ao gateway real com MCP2518FD/CAN FD. Esta revisão fecha principalmente a integração operacional entre TUI e Pico.

## Lean firmware baseline

- Removido modo `SIMULATE ON/OFF` do sensor Pico.
- Removido caminho de geração sintética de buffers no firmware principal.
- Removidos comandos de falha programada do firmware ESP32 legado.
- `PING/PONG` mantido como liveness lease leve, não como sincronização lógica acadêmica.
- Documentada a decisão em `Documentacao/architecture/adr-001-lean-firmware-and-liveness.md`.

## DTC catalog and PlatformIO

- Catálogo da TUI expandido para todos os DTCs definidos pelo firmware Pico.
- Removida resposta genérica para DTCs conhecidos.
- Adicionado fallback por categoria com código hexadecimal bruto para DTCs futuros.
- Criado `Documentacao/protocol/dtc-catalog.md`.
- Criado projeto PlatformIO em `Codigo/node-can/`.
- Adicionado script `scripts/build_esp32_can_node.sh`.

## PlatformIO compile fix

- Corrigido `falhas.cpp`, removendo retorno órfão deixado pela remoção do modo de simulação de falha.
- A função `getSensorValueForFaultAnalysis()` permanece como ponto de extensão para medição real futura.

## Serial port selection

- `scripts/run_tui.sh` não força mais `/dev/ttyACM0`.
- A TUI abre o seletor de portas quando nenhuma porta é informada.
- O menu `Conexão serial / trocar porta` permite mudar de porta em runtime.
- O comando interno `:connect` abre o seletor; `:connect <porta> [auto|gateway|sensor]` conecta diretamente.
- A tela de conexão mostra descrição e HWID das portas detectadas.

## CAN network console and NODE_ID build parameter

- A TUI ganhou uma tela principal genérica da rede CAN.
- A telemetria de vibração passou a ser detalhe especializado ao selecionar sensor lógico.
- Seleção de módulo CAN físico agora é independente da seleção de sensor.
- F4 em módulo CAN abre ações rápidas de configuração/comando do nó.
- Comando interno `:can` envia comando ao barramento/gateway ou abre o modal do nó selecionado.
- Firmware PlatformIO dos nós CAN aceita `IOT_NODE_ID` no build/upload.
- Adicionado `scripts/upload_esp32_can_node.sh`.

## TUI 0.10.0 — offline-first

- Distribuição renomeada de `pico-canfd-tui` para `iot-over-can-tui`.
- Executável principal renomeado para `iot-over-can-tui`.
- Alias `pico-tui` mantido temporariamente para compatibilidade.
- TUI passa a abrir normalmente sem hardware e sem porta serial.
- Falha em uma porta informada não encerra mais a aplicação.
- F3 abre a configuração de conexão serial.
- `scripts/setup_tui.sh` realiza a instalação inicial.
- `scripts/run_tui.sh` não reinstala mais o pacote em toda execução.

## Offline empty-state regression fix

- Corrigido `NameError: sensor_count is not defined` no `QuickStatusPanel`.
- A contagem de sensores Wi-Fi agora é calculada antes do ramo `sensor is None`.
- A TUI pode renderizar o estado vazio com zero módulos e zero sensores.

## Serial port markup regression fix

- Corrigido `MarkupError` ao exibir HWID de portas como CP2102.
- `OptionList` de portas agora usa `markup=False`.
- HWID continua visível como texto literal para facilitar identificação de múltiplos dispositivos.

## Global CAN network commands

- Adicionada tela `Comandos da Rede CAN`.
- Eleição global não depende mais de nó selecionado.
- `Ctrl+L` abre os comandos globais.
- `:election` envia `22 00 FF 01`.
- `:canstatus` envia `22 20 FF 00`.
- `:can` sem nó selecionado abre a tela global.
- Comandos direcionados permanecem na tela exclusiva do módulo CAN.

## TUI 0.11.0 — node capabilities and profile-driven views

- `PhysicalNode` agora separa `role` de `capabilities`.
- `0xAA` passou a ser `LOCAL_SENSOR_DEMO_VALUE`, pertencente ao módulo CAN.
- Telemetria local legada não cria mais um falso sensor wireless `child_id=0`.
- Adicionada tela exclusiva `CanNodeDetailScreen`.
- Tela do módulo mostra identidade, role, CAN, capacidades, sensor local, wireless e filhos.
- Nós físicos passaram a ser selecionáveis no navegador.
- Painéis de vibração iniciam ocultos e só aparecem para `PROFILE=VIBRATION`.
- Probe 00 foi retirado da topologia funcional e tratado como instrumentação.
- Corrigido `StateStore.find_sensor()` após regressão de liveness.

## TUI 0.11.1 — CAN node interaction fix

- Clicar em um módulo CAN abre diretamente sua tela exclusiva.
- A tela do módulo passou a ser viva e atualiza a cada 250 ms.
- `LOCAL_SENSOR_DEMO_VALUE=0xAA` e rodada são atualizados enquanto a janela está aberta.
- Contador RX do módulo é incrementado ao receber telemetria local legada.
- Adicionada tela própria de DTC para módulos CAN.
- F8/DTC funciona tanto para módulo CAN quanto para sensor wireless.
- Tela do módulo possui botões visíveis `DTC`, `Configurar` e `Fechar`.


## TUI/Firmware 0.12.0 — code-review hardening

- validação única de `Payload_Configuration` em `config_validation.c/.h`;
- eliminação do cast de `sample_rate_hz` sem checagem prévia;
- UUID fixo substituído por UUID64 derivado de `pico_unique_id`;
- protocolo wireless elevado para `0x05`, com token de sessão de 64 bits;
- contador monotônico adicionado aos comandos de controle legados;
- parser UDP rejeita datagramas fora do tamanho esperado;
- mutações UDP antigas desabilitadas por default;
- SSID, senha e chave fixa removidos do código-fonte;
- Wi-Fi provisionado em runtime e endereçado por DHCP;
- beacon UDP legado recebe jitter 1,5–3,5 s;
- firmware serial exige `AUTH UNLOCK` para comandos mutáveis;
- `RESET` exige confirmação em duas etapas;
- TUI carrega `device_admin_token`, exige `0600` e falha fechada em erro;
- validação OTP antiga removida/fail-closed;
- detecção de YubiKey deixa de confiar em strings e usa VID USB;
- `serial_client.py` captura falhas de transporte específicas e limita rajadas;
- DTCs separados para autenticação, versão, replay e configuração inválida;
- workflow CI adicionado para TUI, Pico e PlatformIO.


### Fechamento v0.12 — ACK, autorização e preparação para discovery

- Comandos CAN direcionados ao sensor local do módulo passam a produzir `CMD_ACK`
  explícito (`0x23 0x52`) com resultado `APPLIED` ou `REJECTED`.
- A TUI passa a aguardar confirmação `AUTH_UNLOCKED` do firmware do sensor antes
  de reenviar o comando mutável que estava pendente; a autorização não é mais
  presumida apenas pelo lado do cliente.
- O `Payload_ConfigAck` ecoa `request_counter`, preparando correlação entre
  requisição e resposta e reduzindo ambiguidade operacional.
- Ao desligar Wi-Fi, o firmware desabilita o modo STA sem desmontar
  necessariamente toda a camada CYW43, deixando a base pronta para a futura
  descoberta BLE. A descoberta BLE distribuída ainda não faz parte desta versão.
- A confirmação de execução CAN não autentica a origem do frame. Autenticação
  de origem/MAC nos comandos CAN permanece item de evolução do protocolo.

## Finalização documental da baseline v0.12.0

- adicionado `Documentacao/development/challenges-and-resolutions.md` com histórico de desafios, causas, soluções, estado e baseline;
- separados itens resolvidos/mitigados dos desafios ainda abertos;
- corrigidas referências documentais ao antigo gateway wireless central;
- corrigidos caminhos antigos de build da TUI e do firmware Pico;
- descrição do modo OTP alinhada ao comportamento atual: fail-closed até validação criptográfica real;
- manifesto e documentação preparados para o pacote final da etapa de hardening v0.12.0.

## v0.12.2 — DHCP build hotfix

- Corrigido `LWIP_DHCP=0` incompatível com as chamadas `dhcp_start()`/`dhcp_stop()`.
- Adicionado teste de regressão para a configuração do lwIP.
- `build_pico.sh` só informa sucesso quando `edge_node_firmware.uf2` existe.
