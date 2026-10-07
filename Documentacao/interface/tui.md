# TUI — interface de operação do iot-over-can

A TUI é a estação de instrumentação e operação da bancada. Ela apresenta o estado mantido pelo domínio, recebe comandos do operador e usa a Probe 00 como acesso ao barramento CAN. A interface não participa da eleição nem substitui a lógica distribuída dos Nodes.

A implementação está em `Front/pico_tui/` e usa Textual 8.2.8, Rich 15.0.0 e pySerial 3.5.

## Objetivos de interface

A TUI segue seis regras principais:

1. **uma tela por assunto**: visão geral, módulo, sensor, comandos, rede, mensagens, sensores sem fio, espectro e ajuda são telas distintas;
2. **operações importantes ficam visíveis**: a barra de teclas mostra as ações disponíveis; associação wireless não depende de um `Ctrl` oculto;
3. **estado antes de detalhe**: a tela inicial responde primeiro se há algo que exige atenção;
4. **nenhuma ação deve fingir suporte inexistente**: sensores wireless associados só recebem telemetria/configuração via Node enquanto o plano de dados tem sessão autenticada;
5. **sem truncamento horizontal**: conteúdo quebra em linhas e o layout responde à largura/altura do terminal;
6. **cor tem significado operacional**: fundos neutros em repouso; cor saturada reservada a atenção, crítico e interação.

A estrutura original analisada nos arquivos de referência concentrava árvore, tabelas, telemetria, configuração, eventos e linha de comando simultaneamente e possuía dezenas de atalhos. O desenho adotado substitui essa concentração por telas de contexto e por um conjunto pequeno de ações globais. O documento de referência anexado especifica essa direção, inclusive a organização em telas cheias, a redução de atalhos e a paleta industrial.

## Arquitetura da apresentação

```text
┌─────────────────────────────────────────────────────────────────┐
│ Apresentação Textual                                             │
│ app.py       ciclo de vida, navegação e execução de ações       │
│ screens.py   telas cheias                                       │
│ dialogs.py   decisões modais curtas                              │
│ widgets.py   componentes responsivos                            │
│ app.tcss     layout e pontos de quebra                          │
│ palette.py   identidade visual                                  │
├─────────────────────────────────────────────────────────────────┤
│ Apoio à apresentação                                             │
│ presentation.py    condição, rótulos e formatação pt-BR         │
│ command_catalog.py catálogo declarativo de ações                 │
│ spectrum.py        gráfico e metadados FFT                       │
│ preferences.py     preferências não sensíveis                    │
├─────────────────────────────────────────────────────────────────┤
│ Domínio                                                          │
│ core/models.py, core/state_store.py, services/controller.py      │
├─────────────────────────────────────────────────────────────────┤
│ Protocolo e transporte                                           │
│ protocol/*, serial_client.py, event_bus.py, events.py            │
└─────────────────────────────────────────────────────────────────┘
```

Fluxo de atualização:

```text
serial → DecoderRouter → EventBus → DomainController → StateStore
                                                   │
                                 snapshot enxuto   ▼
PicoTuiApp._refresh_ui ───────────────────────→ tela visível
```

A interface recebe cópias do estado. Widgets não alteram diretamente `StateStore`; ações voltam ao app e são traduzidas para comandos de protocolo. O ciclo visual usa `REFRESH_SECONDS = 0.5`. O snapshot usado pela apresentação limita históricos de telemetria e quadros recentes para evitar copiar buffers inteiros a cada atualização.

## Navegação

A base da pilha é sempre `HomeScreen`:

```text
Início
├── Módulo NN
│   └── Sensor NN.CC
│       └── Espectro FFT
├── Comandos
├── Rede CAN
├── Mensagens
├── Sensores sem fio
└── Ajuda
```

`Enter` abre o item realçado. `Esc` volta. `F2`/`i` retorna diretamente ao Início. Rede, Mensagens e Sensores sem fio são telas de primeiro nível: a TUI substitui a tela de primeiro nível atual em vez de empilhar cópias indefinidamente.

### Teclas globais

| Teclas | Ação | Método principal |
|---|---|---|
| `F1` / `?` | Ajuda | `action_show_help` |
| `F2` / `i` | Início | `action_go_home` |
| `F3` / `p` | Conectar/trocar porta | `action_connection_setup` |
| `F4` / `c` | Comandos | `action_open_commands` |
| `F5` / `r` | Rede CAN | `action_show_network` |
| `F6` / `m` | Mensagens | `action_show_messages` |
| `F7` / `w` | Sensores sem fio | `action_show_wireless` |
| `F10` / `q` | Sair com confirmação | `action_exit_confirm` |
| `Esc` | Voltar | `BaseScreen.action_back` |
| `Ctrl+C` | interromper stream no sensor USB direto | tratamento em `on_key` |

`Ctrl+A` continua aceito como alias compatível para abrir Sensores sem fio, mas não faz parte do fluxo que o operador precisa memorizar. O caminho oficial é `F7`/`w`, exibido na barra de teclas e na Ajuda.

Quando um `Input` possui foco, letras são entrada de texto e não atalhos globais.

## Barra de teclas

`KeyBar` fica presa ao rodapé. Cada tela monta somente as ações pertinentes. Os itens quebram como segmentos inteiros: `F7/w Sensores sem fio`, por exemplo, não é separado entre linhas. Os segmentos podem ser clicados em terminais com suporte a mouse.

## Tela Início — `HomeScreen`

Finalidade: informar condição geral e oferecer acesso direto aos equipamentos.

Seções:

| Seção | Conteúdo |
|---|---|
| Faixa de estado | condição global, conexão, CAN, contagem de equipamentos e segurança |
| Precisa de atenção | somente equipamentos com motivo relevante; do mais grave para o menos grave |
| Equipamentos | Nodes CAN e seus sensores lógicos |
| Rede CAN | estado, taxa e contadores essenciais |
| Conexão | estado, tipo, porta, quantidade de candidatos/associados wireless e última ação |
| Mensagens recentes | opcional; oculto por padrão |

O item de equipamento é navegável com `Enter`. Um sensor associado aparece abaixo do Node responsável somente depois da confirmação de associação publicada pelo Node.

O resumo de sensores sem fio distingue **disponíveis** e **associados**. Um UUID disponível é contado uma vez mesmo que vários Nodes o observem.

## Tela Módulo — `NodeScreen`

Apresenta um Node CAN físico.

Seções:

- **Situação**: condição, comunicação, papel líder/seguidor, estado CAN, capacidades, firmware, protocolo, uptime e contadores;
- **Sensor local**: somente quando o módulo possui função local; para o caminho atual mostra perfil, `0xAA`, publicação, rodada e idade;
- **Sensores sem fio associados**: filhos lógicos do Node; `Enter` abre o sensor;
- **Descoberta sem fio**: estado de scan BLE e candidatos observados pelo Node, com UUID, perfil, RSSI e classificação do sinal;
- **Intercorrências**: diagnósticos e eventos relevantes do módulo.

A tecla `w` abre a visão centralizada de Sensores sem fio. `c` abre os comandos do módulo e `s` solicita atualização do estado.

## Tela Sensor — `SensorScreen`

A tela é orientada pelo tipo de sensor e, principalmente, pelo canal realmente disponível.

### Sensor com plano de dados

No acesso USB direto ao Pico W, ou pelo módulo CAN com sessão autenticada, a tela pode apresentar situação, modo, aquisição, telemetria, bateria, métricas de vibração, tendências, diagnósticos, estatísticas de sequência e configuração aplicada. Para perfil `VIBRATION`, FFT e métricas especializadas ficam disponíveis.

### Sensor wireless associado via Node CAN

A tela acompanha o estado do plano de dados informado pelo Node responsável (`WIRELESS_LINK`).

Sem sessão autenticada, mostra o vínculo e o motivo:

```text
Vínculo wireless     ONLINE via Módulo 04
RSSI do vínculo      -52 dBm
Plano de dados       Módulo anunciando a oferta ao sensor
Telemetria           Aguardando sessão autenticada com o módulo
Perfil               Vibração
Identificador        0xE6616408432B6F39
```

Os estados possíveis são: sem informação do módulo, sem canal de dados, módulo anunciando a oferta ao sensor, autenticação em andamento, sessão autenticada ativa e módulo sem chave de enlace. Este último também aparece na condição do sensor, porque exige ação de manutenção.

Nesse estado:

- `c`, `f` e `s` não são oferecidos como operações normais para o filho wireless;
- telemetria, FFT e configuração não aparecem na tela de comandos para esse alvo;
- a tela oferece `d Desassociar` e `w Sensores sem fio`;
- se uma chamada avançada tentar executar uma ação de dados, o app bloqueia e registra que o plano de dados está indisponível.

Com sessão autenticada, a tela é a mesma do acesso direto (situação, métricas, tendências, configuração) e a seção de comunicação passa a mostrar o enlace:

```text
Plano de dados              Sessão autenticada via Módulo 04
Sinal Wi-Fi                 -47 dBm
Sessão ativa há             312 s
Leituras no CAN             310
Perdidas no CAN             0 (0,00 %)
Perdidas no Wi-Fi           2
Rejeitadas (autenticação)   0
Rejeitadas (repetição)      0
```

`c`, `f`, `s` e `w` ficam disponíveis. Ações que só existem no console USB do sensor (reiniciar aquisição, ligar/desligar Wi-Fi, reler a lista de DTC) não são oferecidas pelo módulo. A queda da sessão e o aumento dos contadores de rejeição geram intercorrências.

O estado `ONLINE/STALE/LOST` do vínculo vem do Node responsável. A TUI não usa a idade local da última mensagem de associação para sobrescrever esse estado.

Essa decisão impede uma interface que aceite uma ação e não possua caminho físico/protocolar para entregá-la.

## Sensores sem fio — `WirelessScreen`

Esta é a interface oficial para descoberta e vínculo. Abre com `F7` ou `w` de qualquer tela de primeiro nível.

### Disponíveis

Candidatos são agrupados por UUID. Um UUID visto por quatro Nodes aparece uma única vez. Para cada candidato a TUI mostra:

- UUID;
- perfil;
- melhor Node por RSSI recente;
- RSSI dessa observação e qualidade textual (`bom`, `regular`, `fraco`);
- quantidade de Nodes observadores.

Somente observações recebidas nos últimos 15 s são oferecidas para nova associação. Isso evita associar a partir de uma observação antiga que permanece no modelo de sessão.

Ao realçar o candidato, a seção **Detalhes** mostra todas as observações recentes, ordenadas por RSSI. A primeira é a recomendação atual, mas o texto deixa explícito que a decisão não é automática.

`Enter` ou `a` inicia a associação.

### Escolha do Node

`WirelessNodeScreen` abre como diálogo e lista todos os Nodes que observaram o UUID recentemente:

```text
Módulo 04 · -52 dBm · melhor recepção
Módulo 03 · -58 dBm
Módulo 02 · -65 dBm
Módulo 01 · -78 dBm
```

O operador pode escolher qualquer um. Depois há confirmação explícita. Só então a TUI envia:

```text
WIRELESS BIND <node> <uuid>
```

A TUI não cria o filho localmente no clique. O fluxo esperado é:

```text
pedido BIND
   ↓
ASSOCIATING
   ↓
BOUND
   ↓
ONLINE
   ↓
NN.CC aparece no estado
```

O menor `child_id` livre é alocado pelo Node, não pela apresentação.

### Associados

A lista mostra `NN.CC`, perfil, UUID, Node responsável, estado e RSSI do vínculo. `Enter` abre a tela do sensor.

`d` solicita desassociação. Antes do envio, `ConfirmScreen` descreve a consequência. A TUI envia:

```text
WIRELESS UNBIND <node> <uuid>
```

O filho **não** é removido de forma otimista. A interface aguarda a resposta do Node. Somente `UNBOUND` leva o `DomainController` a remover `NN.CC`. Se o Pico continuar anunciando, o UUID volta à lista Disponíveis e pode ser associado novamente.

### Estado de liveness

A associação e o liveness são responsabilidades do Node CAN:

```text
DISCOVERED → ASSOCIATING → BOUND → ONLINE
                                      │
                       15 s sem BLE   ▼
                                    STALE
                                      │
                       45 s sem BLE   ▼
                                     LOST
                                      │
                         BLE retorna  └──→ ONLINE
```

A TUI somente representa o estado recebido pelo CAN.

## Tela Comandos — `CommandScreen`

É o único lugar normal para comandos operacionais genéricos. O alvo pode ser Rede CAN, um módulo ou um sensor com canal suportado.

Ações são definidas em `command_catalog.py`, agrupadas em:

- **Consultar**;
- **Operar**;
- **Manutenção**.

Cada ação possui título em linguagem operacional, descrição, escopo, parâmetro opcional, confirmação opcional, perfis/módulos suportados e indicação de proteção.

A tela mostra:

- alvo atual;
- lista de ações válidas;
- explicação do efeito;
- linha CAN quando aplicável;
- exigência de autorização/confirmação;
- último envio.

`t` troca o alvo. `Enter` executa a ação realçada. `a` abre o campo **Comando manual (avançado)**.

Para um sensor wireless associado no modo Probe/CAN sem sessão autenticada, a lista de ações de dados fica vazia e a explicação informa o estado do plano de dados, indicando `F7/w` para gerenciar o vínculo. Com a sessão ativa, as ações são enviadas à Probe como `CMD TARGET=NN.CC ACTION=...` e o resultado chega em `ACK`.

### Comando manual

O campo manual é deliberadamente secundário e oculto por padrão. Texto sem `:` é enviado ao equipamento conectado. Texto com `:` executa comandos internos, entre eles:

```text
:status
:node 04
:node 04.01
:can 22 20 FF 00
:wireless
:bind [uuid] [módulo]
:unbind [uuid] [módulo]
:tel ...
:fft ...
:dtc ...
:config ...
:connect ...
:disconnect
:reconnect
:snapshot
:security
:lock
:quit
```

Esses comandos existem para diagnóstico e automação manual; a operação normal não depende deles.

## Rede CAN — `NetworkScreen`

Mostra:

- estado do barramento;
- bitrate de arbitragem e, quando disponível, fase de dados;
- bus-off, error-passive e utilização;
- quadros e bytes recebidos/enviados;
- erros de CRC e interpretação;
- transferências em curso;
- dados da Probe 00;
- até 40 quadros recentes, com direção, ID, formato e bytes.

Não existe rolagem horizontal; linhas longas são quebradas.

## Mensagens — `MessagesScreen`

Mantém o registro operacional da sessão separado do estado contínuo. A tela recebe até 2000 entradas em memória; o JSONL da sessão continua sendo o registro técnico completo.

Controles:

- `d`: mostrar/ocultar depuração;
- `h`: mostrar/ocultar Mensagens recentes no Início e persistir a preferência;
- `l`: limpar somente a visualização.

Mensagens repetitivas de manutenção, dados locais `0xAA` e linhas não interpretadas já classificadas como ruído não são promovidas ao registro operacional, embora continuem disponíveis no arquivo técnico quando pertinente.

## Espectro — `FftScreen`

Disponível quando o alvo realmente possui canal de dados e perfil compatível. O gráfico textual calcula sua largura a partir da área disponível. A tela apresenta espectro, picos e parâmetros. `n` pede novo espectro.

Para filhos wireless sem sessão autenticada, a ação FFT não é oferecida. Pelo módulo, o espectro chega reduzido ao número de faixas pedido e com as magnitudes reescaladas a partir do pico.

## Ajuda — `HelpScreen`

Documenta navegação, símbolos, telas e comandos internos avançados. `F7/w` aparece explicitamente como gerenciamento wireless.

## Diálogos

Todos são `ModalScreen` e deixam a decisão curta sobre o contexto atual.

### `ConnectScreen`

Mostra portas detectadas, caminho manual, tipo de equipamento e modo demonstração. Rótulos públicos usam Probe 00 e Pico W. O modo interno `gateway` continua aceito por compatibilidade; a CLI também aceita `--mode probe`.

A TUI é offline-first: falha de porta não encerra a aplicação.

### `ConfirmScreen`

Usado para saída e ações com consequência operacional. A ação destrutiva/arriscada usa botão de erro, e o foco inicial fica em Cancelar.

### `ParameterScreen`

Coleta um parâmetro validado para uma ação declarativa, como intervalo de liveness ou resolução FFT.

### `ConfigScreen`

Coleta modo, taxa, janela, tamanho, STA/LTA e ganho quando existe canal de configuração do sensor.

### `TargetScreen`

Escolhe o alvo da tela Comandos.

### `WirelessNodeScreen`

Escolhe o Node responsável por um UUID disponível. Mostra RSSI de todas as observações recentes e marca textualmente a melhor recepção.

## Layout responsivo

A TUI evita `DataTable` e outras estruturas que dependem de largura fixa para informações longas. Os widgets textuais derivam de uma base que calcula suas próprias linhas para a largura disponível.

Pontos de quebra:

| Classe | Condição | Comportamento |
|---|---|---|
| `bp-narrow` | largura < 60 | uma coluna, bordas laterais removidas, diálogos ocupam a largura |
| `bp-medium` | 60 ≤ largura < 100 | uma coluna com seções completas |
| `bp-wide` | largura ≥ 100 | duas colunas quando a tela possui duas áreas |
| `bp-short` | altura < 30 | menos espaçamento e diálogos compactos |

Componentes principais:

| Componente | Uso |
|---|---|
| `WrappedLines` | base de texto com largura conhecida |
| `FlowLine` | segmentos que quebram como unidades |
| `KeyBar` | barra contextual de teclas |
| `Breadcrumb` | caminho de navegação |
| `StatusBand` | condição global |
| `Fields` | pares rótulo/valor responsivos |
| `Paragraphs` | texto com continuação indentada |
| `StableOptionList` | lista atualizável sem perder seleção |
| `EquipmentList` | hierarquia visual módulo → sensor |
| `MessageLog` | registro com wrap vertical |

Não deve existir rolagem horizontal como solução de layout.

## Identidade visual — Aço industrial

A interface utiliza superfícies neutras e reserva cor para significado.

Tokens principais:

| Token | Valor | Papel |
|---|---|---|
| `BG_ROOT` | `#1F2429` | fundo |
| `BG_PANEL` | `#272D33` | seções |
| `BG_RAISED` | `#313840` | faixa neutra/diálogos |
| `BG_SELECTED` | `#2E4A66` | seleção |
| `TEXT_PRIMARY` | `#E3E7EB` | valores |
| `TEXT_SECONDARY` | `#B7C0C8` | apoio |
| `TEXT_MUTED` | `#8E99A3` | rótulos |
| `ACCENT_FOCUS` | `#5B9BE0` | foco/interação |
| `STATE_OK` | `#79B38A` | normal |
| `STATE_WARNING` | `#E8A317` | atenção |
| `STATE_CRITICAL` | `#F4706A` | crítico |

Condição usa símbolo e palavra:

| Nível | Unicode | ASCII (`--ascii`) |
|---|---|---|
| Sem dados | `○` | `-` |
| Normal | `●` | `o` |
| Atenção | `▲` | `!` |
| Crítico | `■` | `X` |

## Modelo de condição

`presentation.py` calcula a condição apresentada sem embutir regras nos widgets.

Para equipamentos com telemetria/estado genérico, `StateStore.refresh_freshness()` utiliza os limites do domínio correspondente. Para sensor wireless associado, existe uma exceção deliberada: `association_state` em `ASSOCIATING`, `BOUND`, `ONLINE`, `STALE` ou `LOST` impede o aging genérico; o Node associado é a fonte autoritativa.

DTC crítico, bus-off e comunicação perdida elevam para Crítico. DTC de aviso, stale, error-passive, saturação ou qualidade degradada elevam para Atenção. A condição geral usa a pior condição relevante sem contar várias causas do mesmo equipamento como equipamentos distintos.

## Intercorrências

`Incident` registra fatos relevantes por equipamento, como DTC, comunicação, sequência, saturação, STA/LTA e respostas de comando. A fila por equipamento é limitada e repetições próximas com a mesma chave são agrupadas. A interface mostra DTCs ativos e histórico recente sem repetir a mesma anomalia a cada amostra.

## Segurança na interface

`SecurityManager` é consultado antes do envio. Prefixos mutáveis incluem:

```text
22 00    eleição
22 10    administração de Node
22 30    liveness CAN
WIRELESS BIND / UNBIND
CMD ...
RESET / SET / APPLY / DTC CLEAR / WIFI ...
```

`22 20` permanece leitura. Se a autorização falhar, o comando é bloqueado; esconder a ação não é considerado mecanismo de segurança.

Em conexão USB direta, o firmware do Pico W ainda aplica sua própria sessão `AUTH UNLOCK` para comandos mutáveis.

## Preferências

Arquivo padrão:

```text
~/.config/iot-over-can/tui.json
```

Conteúdo não sensível:

```json
{
  "show_messages_on_home": false,
  "show_debug_messages": false,
  "ascii_symbols": false
}
```

Falha de leitura/gravação não impede a abertura; a TUI volta ao padrão e registra o aviso.

Opções de execução relevantes:

```text
--messages / --no-messages
--ascii
--preferences <arquivo>
--mode auto|probe|gateway|sensor
--security-mode off|presence|otp
--security-config <arquivo>
```

## Testes

A suíte possui testes de protocolo, estado, segurança, catálogo de comandos, preferências, intercorrências, associação wireless e interface. `test_layout_no_truncation.py` percorre telas em diferentes tamanhos para verificar ausência de conteúdo horizontalmente oculto. `test_tui_redesign_v016_contract.py` mantém contratos específicos desta organização: tela wireless visível, associação/desassociação, bloqueio de ações sem plano de dados e consistência da versão.

Na tela **Sensores sem fio**, os itens das listas não dependem da largura transitória informada pela `OptionList` durante o primeiro ciclo de layout. Cada candidato é apresentado em linhas semânticas curtas — UUID, perfil, melhor módulo/RSSI e número de módulos observadores — e cada vínculo associado usa linhas separadas para identificação, perfil/estado, UUID e módulo/RSSI. O estado vazio da lista de disponíveis também é composto em duas linhas curtas ("Nenhum sensor sem fio disponível." / "Observações dos últimos 15 s."), porque a frase anterior tinha 64 células e era, de fato, a origem das três falhas de layout em 48, 60 e 100 colunas. Dessa forma, tanto itens reais quanto estados vazios obedecem ao limite horizontal sem remover informação.

Execução:

```bash
./Codigo/scripts/test_tui.sh
```

A validação de layout deve incluir pelo menos 48×18, 60×24, 80×24, 100×30 e 160×48.

## Capturas de referência

As capturas fornecidas com o redesenho foram consolidadas em:

```text
Documentacao/interface/img/
├── antes_140x45.png
├── antes_60x20.png
├── inicio_120x36.png
├── inicio_48x24.png
├── sensor_120x36.png
├── comandos_120x36.png
└── conexao_80x24.png
```

As imagens `antes_*` servem apenas para registrar o problema de densidade visual que motivou as regras de layout. A interface documentada neste arquivo é a interface atual.

## Limitações atuais

- o vínculo wireless não transporta telemetria ou comandos do Pico W;
- associação ainda não é persistida no Node após reinicialização;
- só o perfil `VIBRATION` possui visualização especializada de telemetria;
- preferências e intercorrências do modelo visual não substituem o JSONL técnico da sessão;
- correlação completa de solicitação/resposta por transação ainda pode ser expandida.
