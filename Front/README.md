# TUI do iot-over-can

`Front/` contém a interface textual de operação e instrumentação do projeto. A aplicação usa Python 3.11+, Textual 8.2.8, Rich 15.0.0 e pySerial 3.5, com versões fixadas em `pyproject.toml` para que desenvolvimento local e CI executem o mesmo ambiente.

A interface não é um console serial decorado. Ela recebe eventos decodificados, mantém um modelo de estado da rede e apresenta telas orientadas ao trabalho do operador.

## Organização visual

Cada assunto ocupa uma tela cheia:

```text
Início
├── Módulo NN
│   └── Sensor NN.CC
│       └── Espectro FFT
├── Comandos
├── Rede CAN
├── Mensagens
├── Sensores sem fio
├── Métricas da rede
├── Ensaios
└── Ajuda
```

`HomeScreen` resume condição, itens que precisam de atenção, equipamentos, rede e conexão. `NodeScreen` descreve um módulo CAN. `SensorScreen` descreve um sensor lógico. `CommandScreen` é o único fluxo normal de envio de comandos. `NetworkScreen` mostra barramento e quadros. `MessagesScreen` contém o registro operacional. `WirelessScreen` gerencia descoberta, associação, política de reassociação e desassociação. `MetricsScreen` reúne as medições do barramento, dos enlaces e a análise de tempo de resposta. `ExperimentsScreen` conduz ensaios de falha e mede o resultado. `FftScreen` apresenta o espectro quando há dados disponíveis.

A interface usa widgets próprios de quebra de texto (`WrappedLines`, `FlowLine`, `Fields`, `Paragraphs`, `StableOptionList`) para não depender de rolagem horizontal. Os pontos de quebra são: largura abaixo de 60 colunas, 60–99, 100 ou mais, e altura abaixo de 30 linhas.

## Teclas globais

| Teclas | Ação |
|---|---|
| `F1` / `?` | Ajuda |
| `F2` / `i` | Início |
| `F3` / `p` | Conexão |
| `F4` / `c` | Comandos |
| `F5` / `r` | Rede CAN |
| `F6` / `m` | Mensagens |
| `F7` / `w` | Sensores sem fio |
| `F8` / `b` | Métricas da rede |
| `F9` / `e` | Ensaios |
| `F10` / `q` | Sair |
| `Esc` | Voltar |

A barra de teclas é contextual e clicável. Letras não são tratadas como atalhos quando um campo de texto está recebendo entrada. `Ctrl+A` permanece apenas como alias compatível para abrir o gerenciamento wireless; não é necessário conhecê-lo para operar a interface.

## Gerenciamento wireless

`F7` ou `w` abre a tela **Sensores sem fio**. Candidatos são agrupados por UUID e observações com mais de 15 s deixam de ser oferecidas para uma nova associação. A lista mostra o melhor RSSI recente e quantos módulos observam o dispositivo. Ao associar, uma janela apresenta todos os Nodes observadores, ordenados por RSSI, e o operador confirma o responsável.

A TUI envia:

```text
WIRELESS BIND <node> <uuid> [AUTO]
WIRELESS UNBIND <node> <uuid>
WIRELESS POLICY <node> <uuid> AUTO|MANUAL
```

Na associação o operador escolhe a política de reassociação. Com a automática, o líder da rede entrega o sensor a outro módulo que o alcance quando o responsável sai do ar ou deixa de ouvi-lo; `f` na lista de associados altera a política depois.

O sensor lógico só aparece após o estado publicado pelo Node. Na desassociação, a TUI não remove localmente `NN.CC` ao pressionar o comando; ela espera `UNBOUND` do Node e então o domínio remove o filho.

O plano de dados de um sensor associado depende de uma sessão autenticada entre o Pico W e o Node responsável, informada pelo Node em `WIRELESS_LINK`. Sem ela, a tela do sensor mostra vínculo, Node responsável, RSSI e o estado do plano de dados, e não oferece telemetria, FFT ou configuração. Com a sessão ativa, essas ações são enviadas à Probe como `CMD TARGET=NN.CC ACTION=...` e as respostas chegam em `TEL`, `FRAG`, `ACK` e `WIRELESS_CONFIG`.

## Métricas e ensaios

`F8` ou `b` abre **Métricas da rede**: ocupação do barramento medida pela Probe 00, período e variação por identificador, estado de erro do controlador, tempo de ida e volta de cada enlace sem fio, tempo de resposta dos comandos e a análise de tempo de resposta no pior caso, com a estimativa de quantos sensores a mais o barramento comporta. `x` exporta tudo para `exports/` em JSON e CSV.

`F9` ou `e` abre **Ensaios**: queda do líder, queda de um módulo, módulo congelado, queda do ponto de acesso, sessões descartadas, carga no barramento e ensaio manual. A TUI pede o alvo, a duração e a confirmação, envia o comando `FAULT ...` e mede a reação da rede a partir das mensagens recebidas.

As duas telas funcionam sobre uma gravação, sem hardware:

```bash
python -m pico_tui --replay replays/owner_failover.log --replay-speed 4
```

`replays/` guarda a saída da Probe 00 em cada cenário da bancada virtual, que executa o firmware real do Node CAN. O desenho, as fórmulas e os limites estão em `Documentacao/arquitetura/metricas-e-ensaios.md`.

## Arquitetura do pacote

```text
pico_tui/
├── app.py                ciclo de vida, navegação e execução de ações
├── screens.py            telas em tela cheia
├── metrics_screens.py    telas Métricas da rede e Ensaios
├── dialogs.py            conexão, confirmação, parâmetros e escolhas
├── widgets.py            componentes responsivos e sem truncamento
├── app.tcss              layout e pontos de quebra
├── palette.py            identidade Aço industrial
├── presentation.py       condição, rótulos e formatação pt-BR
├── command_catalog.py    catálogo declarativo de ações
├── spectrum.py           representação textual de FFT
├── preferences.py        preferências não sensíveis do operador
├── security.py           autorização operacional
├── serial_client.py      transporte serial
├── core/                 eventos e modelo de estado
├── metrics/              medições, análise de tempo de resposta, ensaios e exportação
├── protocol/             decodificadores e contratos de protocolo
└── services/             controlador de domínio, demonstração e log
```

A apresentação recebe snapshots do `StateStore`; widgets não alteram diretamente o domínio. A tela visível é atualizada a cada 0,5 s, e os componentes evitam redesenho quando seu conteúdo não mudou.

## Condição e intercorrências

A apresentação usa quatro níveis: `Sem dados`, `Normal`, `Atenção` e `Crítico`. A cor reforça o nível, mas símbolo e palavra sempre acompanham a cor. DTCs, transições de comunicação, perdas de sequência, saturação e respostas de comando podem gerar intercorrências associadas ao equipamento. Repetições próximas são agrupadas para não transformar a interface em um log de ruído.

Para sensores wireless associados, o estado `ONLINE/STALE/LOST` publicado pelo Node CAN é autoritativo; a TUI não aplica o aging genérico do sensor por cima desse liveness.

## Preferências

Preferências não sensíveis ficam em:

```text
~/.config/iot-over-can/tui.json
```

Elas controlam o painel de mensagens recentes, mensagens de depuração e modo de símbolos ASCII. O arquivo é independente de `security.json`.

Opções úteis:

```bash
iot-over-can-tui --demo
iot-over-can-tui --messages
iot-over-can-tui --no-messages
iot-over-can-tui --ascii
iot-over-can-tui --port /dev/ttyUSB0 --mode probe
iot-over-can-tui --replay replays/leader_failure.log --replay-speed 2
```

`--mode gateway` continua aceito como alias de compatibilidade interna; para documentação e operação usa-se **Probe 00**.

## Instalação e testes

A partir da raiz:

```bash
./Codigo/scripts/setup_tui.sh
./Codigo/scripts/test_tui.sh
./Codigo/scripts/run_tui.sh
```

A suíte cobre protocolo, estado, segurança, catálogo de comandos, preferências, intercorrências, associação wireless, métricas, análise de tempo de resposta, ensaios, contratos de interface e layout responsivo. Com compilador C/C++ disponível, ela também executa a bancada virtual do firmware e confere as gravações de `replays/`. A documentação oficial da interface está em `Documentacao/interface/tui.md`.
