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
└── Ajuda
```

`HomeScreen` resume condição, itens que precisam de atenção, equipamentos, rede e conexão. `NodeScreen` descreve um módulo CAN. `SensorScreen` descreve um sensor lógico. `CommandScreen` é o único fluxo normal de envio de comandos. `NetworkScreen` mostra barramento e quadros. `MessagesScreen` contém o registro operacional. `WirelessScreen` gerencia descoberta, associação e desassociação. `FftScreen` apresenta o espectro quando há dados disponíveis.

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
| `F10` / `q` | Sair |
| `Esc` | Voltar |

A barra de teclas é contextual e clicável. Letras não são tratadas como atalhos quando um campo de texto está recebendo entrada. `Ctrl+A` permanece apenas como alias compatível para abrir o gerenciamento wireless; não é necessário conhecê-lo para operar a interface.

## Gerenciamento wireless

`F7` ou `w` abre a tela **Sensores sem fio**. Candidatos são agrupados por UUID e observações com mais de 15 s deixam de ser oferecidas para uma nova associação. A lista mostra o melhor RSSI recente e quantos módulos observam o dispositivo. Ao associar, uma janela apresenta todos os Nodes observadores, ordenados por RSSI, e o operador confirma o responsável.

A TUI envia:

```text
WIRELESS BIND <node> <uuid>
WIRELESS UNBIND <node> <uuid>
```

O sensor lógico só aparece após o estado publicado pelo Node. Na desassociação, a TUI não remove localmente `NN.CC` ao pressionar o comando; ela espera `UNBOUND` do Node e então o domínio remove o filho.

Um sensor wireless associado ainda não possui plano de dados Pico W ↔ Node CAN. Por isso, a tela do sensor mostra explicitamente vínculo, Node responsável e RSSI, enquanto telemetria, FFT, configuração e comandos via Node não são oferecidos. Isso evita ações que não teriam efeito real.

## Arquitetura do pacote

```text
pico_tui/
├── app.py                ciclo de vida, navegação e execução de ações
├── screens.py            telas em tela cheia
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
```

`--mode gateway` continua aceito como alias de compatibilidade interna; para documentação e operação usa-se **Probe 00**.

## Instalação e testes

A partir da raiz:

```bash
./Codigo/scripts/setup_tui.sh
./Codigo/scripts/test_tui.sh
./Codigo/scripts/run_tui.sh
```

A suíte cobre protocolo, estado, segurança, catálogo de comandos, preferências, intercorrências, associação wireless, contratos de interface e layout responsivo. A documentação oficial da interface está em `Documentacao/interface/tui.md`.
