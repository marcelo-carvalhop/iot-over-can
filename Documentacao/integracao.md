# Guia de integração

Para quem vai usar o projeto como base de outro (um fork) ou acrescentar algo a ele: um sensor novo, uma mensagem nova no barramento, outra placa, outra tela. O guia supõe familiaridade com C, C++ e Python; a [Arquitetura](arquitetura.md) explica o funcionamento e o [Protocolo](protocolo.md), os formatos.

## 1. Licença

O projeto é distribuído sob a GNU GPL versão 3 ([`LICENSE`](../LICENSE)). Em termos práticos:

- você pode usar o código para qualquer fim, inclusive comercial, estudá-lo, modificá-lo e redistribuí-lo;
- quem distribui o programa, ou uma versão modificada, em código-fonte ou gravado em placas, precisa entregar junto o código-fonte completo correspondente, sob a mesma licença, mantendo os avisos de autoria e indicando o que mudou;
- uso interno, sem distribuição, não obriga a publicar as modificações.

As dependências têm licenças próprias: Arduino-ESP32, ACAN2515, NimBLE-Arduino, LovyanGFX, Pico SDK, lwIP, Textual e Rich, entre outras. O BTstack, usado no Bluetooth do Pico W, tem licença própria, e a Raspberry Pi oferece uma licença para usá-lo com os chips dela. Confira as licenças antes de distribuir um produto. Este resumo não é aconselhamento jurídico; vale o texto da licença.

## 2. Mapa do repositório

```text
iot-over-can/
├── Codigo/
│   ├── common/
│   │   ├── ioc_link/      biblioteca C do canal de dados e do barramento
│   │   ├── ioc_panel/     biblioteca C do painel: formatos, retrato, visão da rede, página
│   │   └── tests/         testes das bibliotecas, no computador
│   ├── node-can/          firmware da Probe 00 e dos módulos (ESP32, PlatformIO)
│   │   ├── include/  src/
│   │   └── test/host/     simulações: plano de dados e bancada virtual
│   ├── node-wifi/         firmware do sensor de vibração e módulos comuns dos sensores (Pico SDK)
│   ├── node-audio/        firmware do sensor acústico (Pico SDK)
│   ├── node-paper/        firmware do painel, ESP32-2432S028 (PlatformIO)
│   ├── leitor/            leitor da documentação, M5Stack PaperColor (PlatformIO)
│   └── scripts/           gravação, testes e credenciais
├── Front/                 estação de operação (TUI, Python)
│   ├── pico_tui/  replays/  tests/
├── Simulacao/             laboratório virtual: ponte para a TUI, Studio e pontes USB
├── Pesquisa/              ferramenta de coleta e análise de experimentos
├── Documentacao/          esta documentação
└── .github/workflows/     integração contínua
```

## 3. Como o código se organiza

### Firmware dos módulos (`Codigo/node-can`)

Arduino sobre PlatformIO, o mesmo código para a Probe 00 e para os módulos; o número da placa (`IOT_NODE_ID`) e a chave entram na compilação pelo script. `src/main.ino` tem o laço principal, a eleição, o sinal de presença e as vigilâncias. O resto fica em módulos com a mesma forma:

| Arquivo | Responsabilidade |
|---|---|
| `comandos.cpp` | leitura da serial e comandos de controle (`0x080`) |
| `falhas.cpp` | função local e respostas de estado |
| `wireless_discovery.cpp` | escuta BLE, associação e decisão de reassociação |
| `wireless_link.cpp` | canal de dados: ponto de acesso, sessão, transporte no CAN, encaminhamento ao painel; na Probe, remontagem e linhas de texto |
| `net_metrics.cpp` | medição do barramento (só na Probe) |
| `fault_injection.cpp` | ensaios de falha |
| `relogio.cpp` | relógio da rede |
| `pesquisa.cpp` | instrumentação de medição (`CONFIG`, `BENCH`) |

Cada módulo expõe, conforme precisa, `xxxHandleCanMessage(rx)` (devolve `true` se o quadro era dele), `xxxHandleSerialCommand(linha)` e `xxxPoll()`, chamados pelo laço de `main.ino` e por `comandos.cpp`. Toda transmissão passa por `canSend()`, que também entrega o quadro aos painéis conectados, seguida de `netMetricsOnTransmit()`. Identificadores em `include/can_ids.h`; códigos de controle em `include/protocolo.h`.

### Bibliotecas comuns (`Codigo/common`)

C11 portátil, sem entrada e saída, testado no computador e usado pelo firmware, pelas simulações e pelos testes:

| Arquivo | Conteúdo |
|---|---|
| `ioc_link/ioc_sha256.*` | SHA-256 e HMAC-SHA256 |
| `ioc_link/ioc_link.*` | derivação de chaves, oferta BLE, envelope UDP e desafio-resposta |
| `ioc_link/ioc_wdata.*` | segmentação no CAN e formatos das transferências |
| `ioc_link/ioc_canbits.*` | duração exata de um quadro CAN clássico |
| `ioc_link/ioc_failover.*` | decisão de reassociação |
| `ioc_link/ioc_bindstore.*` | tabela de vínculos gravada na NVS |
| `ioc_link/ioc_time.*` | quadro de hora e regra de adoção |
| `ioc_link/ioc_secpdu.*` | mensagem autenticada para o barramento (preparação para CAN FD; não usada) |
| `ioc_link/ioc_bench.*` | medição do custo da criptografia |
| `ioc_panel/ioc_panel_proto.*` | mensagens do painel |
| `ioc_panel/ioc_panel_cache.*` | retrato e caixa postal guardados pelo módulo |
| `ioc_panel/ioc_view.*` | visão da rede a partir dos quadros |
| `ioc_panel/ioc_page.*` | relevância e composição da página |

### Firmware dos sensores (`Codigo/node-wifi`, `Codigo/node-audio`)

Pico SDK com CMake. Os módulos comuns ficam em `node-wifi`: canal de dados (`edge_network_driver.c`, `edge_link.c`), BLE (`edge_ble_beacon.c`), identidade (`device_identity.c`), alarmes (`edge_diagnostics.c`), bateria, console e validação de configuração. As mensagens do canal de dados estão em `edge_protocol_definitions.h`, incluído também pelo firmware dos módulos. Cada sensor implementa os ganchos de `edge_app.h`:

| Gancho | O que o sensor informa ou faz |
|---|---|
| `edge_app_profile_id` | o perfil anunciado |
| `edge_app_fill_config_menu` | o menu de configuração enviado ao módulo |
| `edge_app_effective_sample_rate_hz`, `edge_app_effective_window_size` | taxa e janela em uso |
| `edge_app_queue_config` | aplicar uma configuração recebida |
| `edge_app_handle_session_command` | comandos próprios do sensor recebidos pela sessão |

O sensor de vibração os implementa em `edge_app_vibration.c`; o acústico, em `node-audio/audio_app.c`, com aquisição (`inmp441_i2s.*`) e processamento (`audio_core1.c`, `acoustic_dsp.c`, `audio_fft.c`) no segundo núcleo.

### Firmware do painel (`Codigo/node-paper`)

`panel_app.*` reúne a lógica do painel sem depender de placa; `panel_signal.*`, o LED e os sons. O hardware entra pela interface `PanelHal`, implementada para a ESP32-2432S028 em `src/cyd/` (ambiente `cyd` do PlatformIO) e, na bancada virtual, por `sim_network.cpp`.

### Leitor da documentação (`Codigo/leitor`)

Firmware à parte, sem relação com a rede. `tools/documentos.py` converte o README e os documentos de `Documentacao/` num cabeçalho C (`src/documentos.h`, gerado a cada compilação e fora do repositório): um bloco por linha, com o tipo do bloco no primeiro byte e marcas de negrito, código e itálico. A lista de documentos fica em `DOCUMENTOS`, nesse script; um caractere sem equivalente na fonte do leitor faz o teste falhar e deve ser acrescentado a `TROCAS`. `src/leitor_layout.*` quebra as linhas e pagina sem depender de placa; `src/main.cpp` desenha as telas, lê os botões e guarda a posição na NVS. `tests/` roda a diagramação e o firmware no computador, sobre imitações das bibliotecas da placa.

### Estação de operação (`Front/pico_tui`)

| Camada | Arquivos |
|---|---|
| transporte | `serial_client.py` |
| decodificação | `protocol/`: `router.py` escolhe o decodificador; `legacy_gateway.py` lê as linhas `[GW]`; `gateway_text.py`, as demais linhas da Probe; `sensor_direct.py`, o console USB de um sensor |
| eventos | `core/events.py`, `core/event_bus.py` |
| domínio | `services/controller.py` transforma eventos em estado; `core/state_store.py` e `core/models.py` guardam o estado |
| telas | `app.py` (navegação e ações), `screens.py`, `metrics_screens.py`, `supervisor.py`, `dialogs.py`, `widgets.py`, `app.tcss` |
| apoio | `command_catalog.py` (ações), `presentation.py` (condição e rótulos), `security.py` (autorização), `alarms.py`, `acoustics.py`, `metrics/`, `history/` |
| operador | `operators.py` (cadastro, senha, sessão), `audit.py` (o que vai para o histórico e quem autorizou), `operators_cli.py` (`iot-over-can-operators`) |

As telas recebem cópias do estado e nunca o alteram; toda ação volta ao `app.py`, que a traduz em comando.

### Laboratório e pesquisa

`Codigo/node-can/test/host/` compila o firmware real dos módulos no computador, com substitutos para o hardware em `stubs/`. `sim_network.cpp` é a bancada virtual (vários módulos num processo) e `sim_wireless_link.cpp`, a simulação do canal de dados de um módulo. `Simulacao/` liga a bancada virtual à estação e a sensores reais pela USB. `Pesquisa/` coleta e analisa experimentos.

## 4. Ambiente de desenvolvimento

```bash
./Codigo/scripts/setup_tui.sh                  # estação e dependências de teste
./Codigo/scripts/test_native_firmware.sh       # bibliotecas, simulações e bancada virtual
./Codigo/scripts/test_tui.sh                   # testes da estação
python3 -m unittest discover -s Simulacao/tests
./Codigo/scripts/build_esp32_can_node.sh 1     # firmware de um módulo
./Codigo/scripts/build_pico.sh                 # sensor de vibração
./Codigo/scripts/build_pico.sh pico_w --audio  # sensor acústico
./Codigo/scripts/build_paper.sh                # painel
./Codigo/scripts/build_leitor.sh               # leitor da documentação
```

A bancada virtual é a principal ferramenta de desenvolvimento: roda o firmware real e mostra o que cada módulo escreve.

```bash
./Codigo/scripts/build_network_sim.sh /tmp/sim_network
/tmp/sim_network --list
/tmp/sim_network owner_failover --verbose
```

Quando uma mudança no firmware altera o que a Probe escreve, refaça as gravações usadas pelos testes da estação: `./Codigo/scripts/record_network_scenarios.sh`. Um teste compara as gravações versionadas com uma execução nova.

## 5. Receitas

### Novo tipo de sensor sem fio

1. **Perfil.** Acrescente o número em `edge_protocol_definitions.h`. `0x03` está reservado para sensores de temperatura; use um número livre a partir de `0x05`. Dê nome ao perfil na Probe (`wireless_discovery.cpp`) e na estação.
2. **Firmware.** Crie `Codigo/node-<nome>/` a partir de `node-audio/CMakeLists.txt`, ligando os módulos comuns de `node-wifi`, e implemente os ganchos de `edge_app.h`. Acrescente as faixas de configuração em `config_validation`.
3. **Telemetria.** Se o relatório não cabe nos tipos existentes, crie um tipo de transferência em `ioc_wdata.h` (o próximo livre é 11; o campo tem 5 bits) com codificação, decodificação e testes. O módulo converte a mensagem do sensor nesse tipo em `wireless_link.cpp`, e a Probe a escreve como linha `TEL ... PROFILE=<nome>`.
4. **Estação.** Decodifique a linha em `protocol/`, guarde os campos no modelo e crie a vista do sensor (`acoustic_view.py` é o exemplo). Nomeie os modos pelo perfil e acrescente os comandos próprios em `command_catalog.py`.
5. **Testes.** Biblioteca (`Codigo/common/tests`), simulação do canal de dados (`sim_wireless_link.cpp`), um sensor simulado na bancada virtual e os testes da estação.
6. **Compilação.** Um alvo no script de compilação e um passo na integração contínua.

### Nova mensagem no barramento CAN

1. **Identificador.** A posição define a prioridade. Mensagens de coordenação ficam abaixo de `0x200`; dados de sensores, no fim. Se mais de um equipamento vai transmitir, reserve uma faixa `BASE + NODE_ID`, um identificador por transmissor. Faixas ainda livres: `0x000`–`0x00F`, `0x011`–`0x04F`, `0x051`–`0x05F`, `0x061`–`0x07F`, `0x081`–`0x0FF`, `0x101`–`0x11F`, `0x140`–`0x1EF`, `0x1F1`–`0x1FF`, `0x201`–`0x27F`, `0x2C0`–`0x2FF`, `0x307`–`0x31F`, `0x360`–`0x37F` e `0x3A0`–`0x7EE`.
2. **Definição.** Declare o identificador em `Codigo/node-can/include/can_ids.h`. Se o painel precisar da mensagem, copie-o para `ioc_panel/ioc_panel_canids.h` e acrescente a conferência em `Codigo/common/tests/test_ioc_panel.c`.
3. **Formato.** Escreva codificação e decodificação num cabeçalho C de `Codigo/common`, com testes.
4. **Firmware.** Trate o quadro num módulo (`xxxHandleCanMessage`, chamado em `handleReceivedCanMessage`) e transmita com `canSend()`. Para a estação saber que a Probe entende a mensagem, acrescente um item em `FEATURES` (nas duas linhas de versão: `main.ino` e `comandos.cpp`).
5. **Probe e estação.** A Probe escreve uma linha `[GW] ...`; a estação a lê em `protocol/legacy_gateway.py`. Dê nome ao identificador em `metrics/catalog.py`.
6. **Painel.** Defina em `ioc_panel_cache.c` se o módulo guarda a mensagem para o retrato, para a caixa postal, só a encaminha ao vivo ou a descarta.
7. **Gravações e documentação.** Refaça as gravações da bancada virtual e acrescente a mensagem ao [mapa de identificadores](protocolo.md#1-mapa-de-identificadores-can).

O relógio da rede seguiu exatamente esses passos: `ioc_time.h`, `relogio.cpp`, `can_ids.h`, `ioc_panel_canids.h`, `legacy_gateway.py` e `catalog.py`.

### Novo comando de texto na Probe

Escreva `xxxHandleSerialCommand(const char*)`, que devolve `false` se a linha não é dele, e chame-o em `handleSerialCommands()` (`comandos.cpp`), antes da leitura dos comandos de quatro bytes. Se o comando altera a rede, inclua o prefixo em `MUTATING_PREFIXES` (`Front/pico_tui/security.py`): a estação passa a pedir autorização. Descreva-o em `PROBE_TEXT_COMMANDS` (`command_catalog.py`), que alimenta o lançador de comandos, e dê a ele um título em `audit.py` (`_DIRECT_TITLES` ou uma função própria) para o histórico dizer o que foi feito; `test_audit.py` falha se um prefixo mutável ficar sem título.

### Nova tela ou painel na estação

Derive de `BaseScreen` (`screens.py`), monte a barra de teclas só com as ações da tela e implemente `refresh_view` com o estado recebido. Use os componentes de `widgets.py` (`Fields`, `Paragraphs`, `WrappedLines`): eles quebram o texto em vez de cortá-lo. `test_layout_no_truncation.py` percorre as telas em 48×18, 60×24, 80×24, 100×30 e 160×48 e falha se algo for cortado. Uma ação que altera a rede deve passar por `app.py`, para receber a autorização, a confirmação e o registro de quem a fez.

### Identificação do operador

Quem está na estação é decidido em um só lugar, `OperatorSession` (`operators.py`). Todo comando que sai passa por `PicoTuiApp._send_raw`, que pergunta `authorize(mutating=...)` e registra o resultado com o `Actor` devolvido. Três extensões comuns:

- **Outro jeito de se identificar (FIDO2, cartão, login único).** Faça o verificador devolver `Actor(name=..., method="fido2")` depois da prova e use-o no lugar de `OperatorSession.login` (derive a classe e sobrescreva `login`). O histórico, a tela e a CLI usam `Actor.method` e `Actor.verified`; nada mais muda. O modelo e seus limites estão em [Segurança](seguranca.md#5-identificação-do-operador).
- **Registrar uma ação nova.** Monte o evento com `make_action(tipo, título, actor, ...)` e publique-o com `app._audit(ação, actor)`. O `HistoryService` o grava na faixa AUDIT, com o autor e dentro da cadeia de hashes; o `LogEvent` correspondente vai para as mensagens e para o arquivo da sessão. Não ponha segredos nos detalhes: passe o texto por `redact_command`.
- **Creditar uma resposta assíncrona a quem a pediu.** `AuthorizationLedger.remember(...)` guarda quem enviou o comando e `claim(...)` credita a resposta do firmware. O `HistoryService` já o consulta nos eventos de associação.

O banco ganha as colunas `events.actor`, `events.chain` e `sessions.station`; bancos antigos são migrados ao abrir (`HistoryStore._migrate`). `OperatorAction` (`core/events.py`) é o evento que leva a ação até o histórico. Os testes ficam em `test_operators.py`, `test_audit.py`, `test_history_actor.py` e `test_app_operator.py`.

### Outro hardware para o painel

Implemente `PanelHal` (`panel_app.h`):

| Grupo | Métodos |
|---|---|
| tempo | `millis`, `epochNow`, `clockValid`, `setClock` |
| rede | `joinNode`, `leaveNode`, `linkUp`, `sendDatagram` |
| tela | `render`, `renderBusy` |
| sinais | `leds`, `tone` |
| placa | `readSensors`, `keyboardPresent`, `sleepFor`, `powerOff` |
| memória | `storeU32`, `loadU32` |
| registro | `log` |

`render` recebe a página composta (`ioc_dl`, 400×600). Uma tela menor pode compor a própria página a partir de `app.view()` e de `ioc_page_assess`, como faz `src/cyd/main_cyd.cpp`. O UUID precisa ser único por aparelho (a CYD usa o endereço MAC). Crie um ambiente no `platformio.ini` e um passo na integração contínua.

### Outra placa para os módulos

Os pinos do MCP2515 (SPI, `MCP_CS`, `MCP_INT`), o cristal (`MCP_QUARTZ_HZ`, 8 MHz) e os LEDs estão no início de `src/main.ino`. BLE e Wi-Fi juntos não cabem na partição padrão do ESP32: o `platformio.ini` usa `min_spiffs.csv`. Outros controladores CAN pedem outro driver no lugar do ACAN2515.

## 6. Uma instalação nova

- **Credenciais próprias.** Gere-as com `provision_sensor_security.sh` no computador que grava as placas. A chave `000102…1f` que aparece nos testes e nas simulações é pública e nunca deve ser gravada em hardware.
- **Sensores sem a chave mestra.** Grave cada sensor de uso permanente com as chaves só dele (`--sensor <UUID>`).
- **Sem ensaios.** Compile os módulos com `-D IOT_FAULT_INJECTION=0` e não deixe a Probe 00 ligada a uma rede em operação: ela pode ocupar o barramento.
- **Autorização na estação.** Use o modo `presence` (padrão), não `--security-mode off`.
- **Identificação do operador.** Cadastre os operadores (`iot-over-can-operators add`) e use `--operator-mode required` se quiser que nada altere a rede sem alguém identificado.
- **Limites.** Leia os [limites conhecidos](arquitetura.md#10-limites-conhecidos-e-evoluções-previstas) e o [modelo de segurança](seguranca.md) antes de usar fora da bancada.

## 7. Convenções do projeto

- Textos, mensagens e documentação em português.
- Um transmissor por identificador CAN; dados de sensores na menor prioridade.
- Número 0 é a Probe 00; módulos de 1 a 31, sempre fornecidos pelo script.
- A estação não oferece uma ação sem um caminho real para entregá-la.
- Mudança de protocolo vem com teste na biblioteca, na bancada virtual e na estação, e com as gravações refeitas.
- Segredos nunca entram no repositório (`*.local` está no `.gitignore`).
- Scripts são versionados como executáveis (`git ls-files -s Codigo/scripts` mostra `100755`).
- Cada assunto tem um documento em `Documentacao/`; ao mudar o comportamento, atualize o documento do assunto. `historico/` não é revisado.

## 8. Testes e integração contínua

`.github/workflows/ci.yml` roda a cada envio:

| Trabalho | O que verifica |
|---|---|
| `tui-tests` | testes da estação e `test_native_firmware.sh` (bibliotecas, fuzz dos decodificadores, sensor acústico, simulações, bancada virtual e leitor) |
| `usb-hil-regressions` | laboratório virtual (`Simulacao/tests`) |
| `pico-firmware` | sensor de vibração (Pico W) e sensor acústico (Pico W e Pico 2 W) |
| `esp32-platformio` | Probe 00 e módulos |
| `panel-firmware` | painel (ESP32-2432S028), com e sem chave |
| `reader-firmware` | leitor da documentação (M5Stack PaperColor) |
| `research-toolkit` | ferramenta de pesquisa |

Para acrescentar um cenário à bancada virtual, escreva a função em `sim_network.cpp` com os auxiliares existentes (`formNetwork`, `bindSensorA`, `probeCommand`, `runUntilTrue`, `logHas`, `CHECK`) e inclua-a na tabela de cenários. `test_native_firmware.sh` executa todos; refaça as gravações em seguida.

## 9. Compatibilidade entre versões

- Grave a Probe 00 e todos os módulos com a mesma versão: os formatos do barramento mudam juntos.
- O protocolo do canal de dados é o `6`. Um sensor com protocolo anterior é descoberto e associado, mas não abre sessão.
- A estação usa um recurso da Probe só se ele aparece em `FEATURES`; com uma Probe antiga, o recurso fica indisponível, sem erro.
- O módulo ignora sinais desconhecidos no HELLO do painel; um painel novo funciona com um módulo antigo, sem a hora.
