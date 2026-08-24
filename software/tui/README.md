# Pico CAN FD TUI

Interface supervisória em terminal para o sistema composto por sensores wireless Pico 2 W, gateways ESP32 e rede CAN FD. Esta revisão usa o padrão visual do `pico_tui` como base e incorpora a arquitetura, os parsers e os serviços desenvolvidos para a TUI distribuída.

A aplicação opera em três modos:

- `GATEWAY_CAN`: conexão USB serial com o gateway ESP32. É o caminho principal.
- `SENSOR_DIRECT`: conexão USB CDC diretamente com o Pico 2 W para bancada e depuração.
- `DEMO`: rede simulada, sem hardware.

## Estado desta revisão

Já estão implementados:

- interface Textual com paleta Outono na Tundra;
- árvore `gateway → nós CAN → sensores wireless`;
- identificação lógica no formato `20.01`;
- telemetria dinâmica, tela detalhada F6 e tendências de RMS/PPV;
- modais de conexão, telemetria detalhada, configuração, FFT, DTC, rede e detalhe do sensor;
- protocolo ASCII real do console do Pico;
- protocolo textual de referência para o gateway;
- compatibilidade com mensagens legadas da fase 1;
- decodificação de ID CAN FD estendido de 29 bits;
- sequência de 16 bits, perda, duplicação, fora de ordem e rollover;
- CRC16 e CRC32;
- remontagem de fragmentos e timeout independente da chegada de novos frames;
- múltiplos DTCs por sensor;
- logs JSONL, exportação CSV e snapshots JSON;
- modo demonstração com quatro nós físicos e três sensores;
- fallback de porta serial para pseudo-terminal POSIX, usado nos testes;
- compatibilidade testada com Textual 1.0 e Textual 8.2;
- baseline de aquisição oficial em `POLLING`;
- `DTC CLEAR` funcional por modal, CLI interna e comando direto;
- fluxo de configuração `STAGED → QUEUED → CONFIG_APPLIED`;
- FFT serial sob demanda por `FFT ONCE` + `TELEMETRY ONCE`;
- gráfico espectral com eixo de frequência, magnitude, resolução e picos principais.

O enquadramento binário definitivo entre gateway e computador ainda depende da especificação final do firmware do ESP32. Até essa definição, o projeto utiliza um protocolo textual explícito e testável, documentado em `GATEWAY_PROTOCOL_REFERENCE.md`.

## Requisitos

- Python 3.11 ou superior;
- terminal com pelo menos 80 × 24 caracteres;
- terminal True Color recomendado;
- Linux, Windows ou outro sistema com suporte do pySerial.

Dependências principais:

```text
textual>=1.0,<9.0
rich>=13,<16
pyserial>=3.5,<4
```

## Instalação

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -e .
```

No Windows PowerShell:

```powershell
py -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -e .
```

A instalação cria os comandos equivalentes:

```bash
pico-tui
can-tui
```

## Execução

### Demonstração sem hardware

```bash
pico-tui --demo
```

### Detecção automática

```bash
pico-tui --port /dev/ttyACM0 --mode auto
```

### Sensor direto

```bash
pico-tui --port /dev/ttyACM0 --mode sensor
```

### Gateway CAN FD

```bash
pico-tui --port /dev/ttyUSB0 --mode gateway
```

### Windows

```powershell
pico-tui --port COM5 --mode sensor
```

Sem `--port`, a aplicação abre a janela de conexão e enumera as interfaces disponíveis.

Argumentos:

```text
--port, -p       porta serial
--baud, -b       baudrate nominal, padrão 115200
--mode           auto, gateway ou sensor
--demo           inicia a rede simulada
--no-file-log    desativa o JSONL automático
```

## Layout

A tela principal possui três colunas:

```text
┌────────────────────────────────────────────────────────────────────────────┐
│ Cabeçalho                                                                  │
│ conexão │ modo │ porta │ alvo │ CAN │ RX │ CRC                            │
├──────────────────┬────────────────────────────────┬────────────────────────┤
│ Rede e sensores  │ Telemetria                     │ Configuração rápida    │
│                  │ Tendência de RMS               │ Estado rápido          │
│ Gateway          │                                │                        │
│ ├─ Node 20       │ Eventos e comandos             │                        │
│ │  ├─ 20.01      │                                │                        │
│ │  └─ 20.02      │                                │                        │
│ └─ Node 21       │                                │                        │
├──────────────────┴────────────────────────────────┴────────────────────────┤
│ Linha de comando                                                           │
│ Rodapé de atalhos                                                          │
└────────────────────────────────────────────────────────────────────────────┘
```

Abaixo de aproximadamente 112 colunas, a aplicação ativa automaticamente o modo compacto. `F11` alterna manualmente esse estado.

## Atalhos

### Funções

| Tecla | Ação |
|---|---|
| `F1` | ajuda |
| `F2` | menu principal |
| `F3` | abrir navegador hierárquico de nós e sensores |
| `F4` | configurar sensor selecionado |
| `F5` | solicitar status |
| `F6` | abrir telemetria detalhada em tempo real |
| `F7` | solicitar FFT |
| `F8` | DTCs |
| `F9` | estado da rede CAN FD |
| `F10` | saída com confirmação |
| `F11` | modo compacto |
| `F12` | snapshot JSON |

### Operação

| Tecla | Ação |
|---|---|
| `Ctrl+T` | telemetria ON/OFF |
| `Ctrl+F` | FFT |
| `Ctrl+D` | diagnósticos |
| `Ctrl+R` | reconectar |
| `Ctrl+P` | pausar somente a atualização visual |
| `Ctrl+G` | resumo do gateway |
| `Ctrl+N` | árvore de nós |
| `Ctrl+E` | eventos |
| `Ctrl+B` | detalhe do sensor |
| `Ctrl+K` | limpeza de DTC |
| `Ctrl+M` | simulação ON/OFF |
| `Ctrl+O` | reiniciar aquisição em `POLLING` |
| `Ctrl+C` | parar stream enviando byte `0x03` |

`Ctrl+C` envia o byte `0x03` ao firmware direto para interromper a telemetria contínua. A saída da aplicação permanece em `F10`, `Q` ou `:quit`.


## Tela detalhada de telemetria — F6

`F6` abre uma janela dedicada ao sensor selecionado. A janela é atualizada enquanto novas linhas `TEL` são recebidas e apresenta:

- estado do sensor, streaming e aquisição;
- todas as métricas resumidas do firmware;
- tendências de RMS e PPV;
- qualidade, sequência, perdas, bateria e DTCs;
- controles explícitos para `TELEMETRY ON`, `OFF`, `ONCE`, `FAST`, `SLOW` e `PERIOD`.

Abrir ou fechar a janela não liga nem desliga a telemetria. `Espaço` pausa apenas a atualização visual; recepção e logs continuam ativos.


## Guia das grandezas exibidas

### Advertência sobre valores de referência

As referências desta seção servem para interpretação, comissionamento e comparação de tendências. Elas **não constituem limites universais de falha ou segurança**. O resultado depende do tipo de máquina ou estrutura, posição e fixação do sensor, faixa do acelerômetro, taxa de amostragem, janela DSP, duração do buffer e ruído local.

A prática recomendada para este projeto é registrar uma **baseline saudável** para cada sensor, modo e ponto de montagem. Alertas absolutos só devem ser habilitados depois de validação experimental. A série ISO 20816 fornece princípios para avaliação de vibração de máquinas, mas os limites são específicos para classes de equipamento e frequentemente utilizam velocidade RMS, não o RMS de aceleração calculado diretamente por este firmware.

### Grandezas de vibração e DSP

| Grandeza | O que é | O que representa neste projeto | Referências de interpretação |
|---|---|---|---|
| **RMS** | Raiz quadrada da média dos quadrados da aceleração dinâmica após remoção da componente DC. | Intensidade global da vibração no buffer. Aumenta quando a energia vibratória total cresce, mesmo que não exista um único pico dominante. | `0` representa ausência ideal de movimento dinâmico. Não há limite universal em `m/s²`; comparar com a baseline do mesmo sensor. Uma elevação persistente é mais importante que um pico isolado. Não aplicar diretamente as zonas da ISO 20816 sem converter e validar a grandeza usada. |
| **Curtose** | O firmware reporta **curtose em excesso**, isto é, curtose menos 3. | Mede a presença de caudas pesadas e impulsos. Pode indicar impactos, folgas, trincas, choques ou defeitos incipientes em rolamentos. | Ruído gaussiano tende a `0`. Valores negativos indicam distribuição mais achatada; valores positivos indicam maior impulsividade. Como heurística: `0–1` discreta, `1–3` relevante e `>3` forte impulsividade. Esses intervalos não são normativos e dependem do tamanho do buffer. |
| **Fator de crista — CREST** | Razão entre o maior valor absoluto do buffer e o RMS. | Indica quanto os picos instantâneos se destacam do nível médio. É útil para detectar impactos curtos que ainda não elevaram muito o RMS. | Uma senoide ideal possui fator de crista `√2 ≈ 1,414`. Ruído e sinais reais podem ficar próximos de `3`. Valores crescentes acima da baseline, especialmente `>3–5`, sugerem impulsos; não usar um único limite para todos os equipamentos. |
| **Frequência de pico — PEAK_HZ** | Frequência do bin de maior magnitude, ignorando o bin DC. | Indica a componente periódica dominante no buffer. Pode revelar rotação, harmônicos, ressonância ou excitações externas. | Comparar com `RPM/60`, harmônicos `2×`, `3×` etc. e frequências estruturais conhecidas. Não existe valor universal bom ou ruim; a relevância depende da origem física esperada. |
| **Amplitude de pico — PEAK_AMP** | Magnitude do bin dominante da FFT unilateral. | Quantifica a intensidade da componente indicada por `PEAK_HZ`. | Na baseline atual a unidade deve ser tratada conforme o protocolo, frequentemente como `raw`. Comparações só são válidas mantendo taxa, tamanho, janela, ganho e normalização iguais. Não usar como valor absoluto de engenharia enquanto não houver calibração formal. |
| **Entropia espectral — ENT** | Entropia de Shannon normalizada do espectro, entre `0` e `1`. | Indica se a energia está concentrada em poucas frequências ou espalhada pelo espectro. Pode ajudar a diferenciar vibração tonal de ruído amplo ou comportamento estrutural complexo. | Próximo de `0`: espectro concentrado. Próximo de `1`: espectro distribuído. Heurística visual: `<0,3` concentrado, `0,3–0,7` misto, `>0,7` amplo. Não é um critério normativo. No firmware atual é relevante principalmente em `STRUCTURAL`. |
| **PPV — Peak Particle Velocity** | Maior módulo da velocidade obtida pela integração da aceleração durante o buffer, expressa em `mm/s`. | Aproxima a velocidade máxima de movimento do ponto monitorado. É útil em vibração estrutural, eventos sísmicos e comparação com critérios que utilizam velocidade de partícula. | No firmware atual é uma **estimativa**, calculada por integração simples após remoção da média, sem cadeia metrológica certificada e sujeita a drift. Portanto, não aplicar automaticamente limites legais. Como contexto externo, critérios estruturais publicados variam aproximadamente de `3–5 mm/s` para construções sensíveis, `5–20 mm/s` para residenciais e `20–50 mm/s` para industriais, sempre dependentes da frequência, construção e norma. Para este projeto, usar tendência e baseline até haver calibração. |
| **STA/LTA** | Razão entre uma média de curto prazo e uma média de longo prazo da energia do sinal. | Detector de eventos transitórios no modo `SEISMIC`. Quando a razão supera o limiar configurado, `STA_LTA=YES`. | Em ruído estacionário, a razão tende a ficar próxima de `1`. O firmware utiliza limiar configurável, com valor inicial típico de projeto `4,0`. Limiar menor aumenta sensibilidade e falsos disparos; limiar maior reduz falsos alarmes e pode perder eventos. Deve ser calibrado com dados reais do local. |
| **CLIP** | Indicador de saturação do acelerômetro ou da cadeia de aquisição. | Informa que uma ou mais amostras atingiram ou se aproximaram da faixa máxima e que as métricas do buffer podem estar distorcidas. | O estado normal é `NO`. Qualquer `YES` deve marcar o buffer como saturado e inadequado para análise quantitativa. O MPU-6050 admite faixas programáveis de `±2`, `±4`, `±8` e `±16 g`; o limite efetivo depende da configuração do firmware. |
| **FFT_VALID** | Flag que informa se os indicadores espectrais daquele buffer são válidos. | Controla a apresentação de `PEAK_HZ`, `PEAK_AMP`, `ENT` e do gráfico FFT. | `YES` é esperado em `ROTATING` e `STRUCTURAL`. `NO` é esperado em `SEISMIC` na baseline atual e não representa falha. |

### Grandezas de aquisição e configuração

| Campo | Significado e relevância | Referência prática |
|---|---|---|
| **RATE_HZ / taxa efetiva** | Número de amostras por segundo efetivamente usado no processamento. Define a maior frequência observável e participa do cálculo do eixo da FFT. | Frequência de Nyquist `Fs/2`. Com `1000 Hz`, o espectro pode representar no máximo `500 Hz`. A taxa deve ser maior que duas vezes a frequência de interesse; na prática, utilizar margem adicional. |
| **WINDOW_SIZE / WIN** | Quantidade de amostras processadas em cada buffer. | A resolução espectral é `Δf = Fs/N`. Em `1000 Hz`: `N=128 → 7,8125 Hz`; `256 → 3,90625 Hz`; `512 → 1,953125 Hz`. Buffers maiores melhoram resolução, mas aumentam tempo e latência. |
| **WINDOW** | Função aplicada ao buffer antes da FFT para reduzir vazamento espectral. | `RECT`: melhor resolução para sinais coerentes, maior vazamento. `HANN`: opção geral. `HAMMING`: boa redução de lóbulos próximos. `FLATTOP`: favorece precisão de amplitude, com menor resolução. `BLACKMAN`: forte supressão de vazamento, com lóbulo principal mais largo. |
| **AXIS** | Origem espacial do sinal processado. | `VECTOR` indica que o firmware reporta uma grandeza combinada dos eixos; a fórmula exata deve permanecer documentada no firmware. Comparar resultados apenas entre sensores que usam o mesmo modo de eixo. |
| **ACQ** | Modo de aquisição. | `POLLING` é saudável e oficial na baseline. `SIM` indica dados simulados. `IDLE` indica aquisição inativa. `DRDY` permanece legado/experimental. |
| **GAIN** | Multiplicador de calibração aplicado antes do DSP. | `1,0` significa sem correção. Alterar o ganho modifica RMS, curtose, FFT, PPV e STA/LTA; deve ser definido por calibração e não para “melhorar” visualmente os valores. |

### Integridade, diagnóstico e alimentação

| Campo | Significado | Referência prática |
|---|---|---|
| **DTC / DTC_COUNT** | Código do diagnóstico ativo e quantidade de diagnósticos. | `0x0000` e contagem `0` representam ausência de DTC. Qualquer código deve ser interpretado pelo catálogo; `0x2002` indica clipping/saturação. |
| **BATT_PCT / BATT_MV** | Percentual e tensão da bateria. | `255` e `65535` são sentinelas e devem aparecer como `N/A`, não como medições. Alertas só são válidos quando a bateria estiver instrumentada. |
| **SEQ** | Contador de sequência do fluxo. | Incremento contínuo é esperado. Saltos indicam perda; repetição indica duplicação; retorno controlado de `65535` para `0` é rollover normal. |
| **LOSS %** | Estimativa de mensagens ausentes a partir da sequência. | `0%` é o objetivo. Qualquer perda persistente deve ser investigada, mas o limite aceitável depende da taxa, do tipo de mensagem e da criticidade da aplicação. |
| **Data Quality** | Flags que distinguem dado real, simulado, placeholder, degradado, inválido ou obsoleto. | Dados `SIM`, `PLACEHOLDER`, `INVALID` ou `STALE` não devem ser utilizados como medição operacional sem tratamento explícito. |

### Como usar os valores de referência

1. Fixar o sensor rigidamente e registrar posição, orientação e configuração.
2. Coletar uma baseline em condição conhecida como saudável.
3. Manter taxa, janela, tamanho de buffer, ganho e eixo constantes durante comparações.
4. Observar tendências combinadas, por exemplo RMS crescente com curtose e fator de crista elevados.
5. Validar qualquer limiar absoluto com equipamento de referência e ensaio controlado.
6. Não utilizar o PPV calculado atualmente para laudo estrutural, conformidade normativa ou decisão de segurança sem calibração e validação metrológica.

### Referências técnicas

- ISO 20816-1:2016, *Mechanical vibration — Measurement and evaluation of machine vibration — Part 1: General guidelines*. Consultar também a parte específica aplicável à classe da máquina.
- TDK InvenSense, *MPU-6000/MPU-6050 Product Specification*, Rev. 3.4.
- Allen, R. V. (1978), “Automatic earthquake recognition and timing from single traces”, *Bulletin of the Seismological Society of America*, 68(5), 1521–1532, DOI `10.1785/BSSA0680051521`.
- Siskind, D. E. et al. (1980), *Structure Response and Damage Produced by Ground Vibration from Surface Mine Blasting*, U.S. Bureau of Mines, Report of Investigations 8507.

## Visualização da FFT — F7

`F7` abre a solicitação de FFT e, após a confirmação, uma janela de espectro que aguarda a resposta separada do firmware. A TUI mostra:

- magnitude por bin;
- eixo horizontal em hertz quando `RATE_HZ` e `WINDOW_SIZE` estão disponíveis;
- resolução `Δf = Fs / N`;
- faixa realmente coberta pelos bins recebidos;
- frequência de Nyquist;
- cinco maiores picos com frequência, magnitude e índice do bin.

O eixo vertical permanece identificado como `Magnitude [raw]` enquanto o firmware não definir uma unidade calibrada, potência ou PSD.

## Prompt interno

Comandos iniciados por `:` são tratados pela TUI:

```text
:status
:node 20.01
:tel on 20.01
:tel off 20.01
:tel rate normal
:tel once
:tel fast
:tel slow
:tel period 1000
:fft once 20.01 bins=64
:dtc list 20.01
:dtc clear 20.01 all
:acq polling
:config 20.01 rate=250 window=hann size=256
:export csv
:disconnect
:reconnect
:quit
```

Linhas sem `:` são encaminhadas ao dispositivo conectado. No modo direto, isso permite usar todo o console ASCII do Pico.

## Modo `SENSOR_DIRECT`

O parser reconhece:

```text
VERSION
STATUS
STAGED
CONFIG_APPLIED
TEL
FFT
DTC_EVENT
NET_EVENT
WARN
ERR
OK
NOTE
PONG
DTC
NET
EDGE>
```

Na conexão, a TUI envia:

```text
VERSION
STATUS
GET
```

A porta direta é representada internamente como sensor `01.01`. O filtro serial remove eco e prompt sem eliminar respostas assíncronas.

Comandos mantidos na baseline atual:

```text
STATUS
GET
SET MODE IDLE|ROTATING|STRUCTURAL|SEISMIC
SET RATE <4..1000>
SET WINDOW RECT|HANN|HAMMING|FLATTOP|BLACKMAN
SET WINDOW_SIZE 128|256|512
SET STALTA <valor>
SET GAIN <valor>
APPLY
TELEMETRY ON|OFF|ONCE|FAST|SLOW
TELEMETRY PERIOD <ms>
FFT ONCE
SIMULATE ON|OFF
ACQ POLLING
DTC
DTC CLEAR
NET
VERSION
PING
RESET
!
```

`ACQ DRDY` e os comandos `SLEEP LIGHT`, `SLEEP SENSOR`, `SLEEP DEEP` e `WAKE REASON` não são oferecidos pela TUI. `DRDY` permanece apenas no parser para compatibilidade com logs antigos.

A FFT direta é obtida sob demanda. A TUI envia `FFT ONCE`, envia `TELEMETRY ONCE` e aguarda `FFT VALID=YES BINS=... VALUES=...`. No modo `SEISMIC`, `FFT_VALID=NO` é esperado e não constitui falha.

## Modo `GATEWAY_CAN`

A TUI espera que o gateway exponha eventos de alto nível ou frames brutos pela USB serial. O protocolo textual de referência aceita, entre outras, as mensagens:

```text
GW_VERSION
GW_STATUS
NODE
SENSOR
TEL
DTC
DTC_CLEAR
ACK
CAN_RX
CAN_TX
FRAG
CRC_ERROR
```

Exemplo de telemetria escalonada:

```text
TEL NODE=20 CHILD=1 SEQ=7 MODE=ROTATING ACQ=POLLING \
RMS_MG=125 KURT_X100=25 CREST_X100=310 PEAK_HZ_X10=500 \
ENTROPY_X1000=700 PPV_UM_S=450 QUALITY=REAL
```

Exemplo de comando enviado ao gateway:

```text
CMD TARGET=20.01 ACTION=FFT TX=A1B2C3 MODE=VIEW_ONLY BINS=64
```

A gramática e os exemplos completos estão em `GATEWAY_PROTOCOL_REFERENCE.md`.

## CAN FD

O modelo adotado considera:

```text
CAN FD
MCP2518FD
ID estendido de 29 bits
arbitragem inicial de 500 kbit/s
fase de dados inicial de 2 Mbit/s
payload de até 64 bytes
```

Estrutura do ID:

```text
Bits 28..26  PRIORITY
Bits 25..22  DOMAIN
Bits 21..14  PARENT_NODE_ID
Bits 13..8   CHILD_ID
Bits 7..0    MSG_TYPE
```

O monitor de rede preserva frames desconhecidos e mostra ID, direção, FD, DLC e bytes brutos.

## FFT e fragmentação

A FFT é solicitada sob demanda com 32, 64, 128 ou 256 bins. O padrão é 64.

O remontador suporta:

- fragmentos fora de ordem;
- duplicados idênticos;
- detecção de duplicado divergente;
- CRC32 do payload final;
- timeout de transferência;
- transferências simultâneas de origens distintas.

Formatos atuais de magnitude:

```text
U16_LE
F32_LE
```

No modo demonstração, a FFT é gerada localmente para validar o modal e o gráfico ASCII.

## Diagnósticos

Cada sensor mantém uma coleção de DTCs ativos. A janela de diagnóstico exibe:

- código;
- severidade;
- sintoma;
- ocorrências;
- estado.

A TUI permite solicitar a lista, limpar um código no protocolo do gateway e limpar a tabela direta com o comando literal `DTC CLEAR`. Tanto o modal de DTC quanto `:dtc clear ...` utilizam o mesmo fluxo validado.

## Qualidade dos dados

A interface distingue:

```text
[REAL]
[SIM]
[N/A]
[DEG]
[CRIT]
[STALE]
[LOST]
```

Valores não instrumentados ou placeholders não são apresentados como medições válidas. `BATT_PCT=255` e `BATT_MV=65535` são convertidos para bateria `N/A`. `POLLING` é o modo nominal saudável; `DRDY_IRQ=0` e `DRDY_MISSED=0` são ignorados para diagnóstico quando `ACQ=POLLING`.

## Logs e exportação

Por padrão, cada execução cria:

```text
logs/session_<id>.jsonl
```

O arquivo registra eventos e telemetria estruturada. Pode ser desativado com `--no-file-log`.

Exportação CSV:

```text
:export csv
```

Saída:

```text
exports/telemetry_<session>.csv
```

Snapshot completo do estado:

```text
F12
```

Saída:

```text
exports/snapshot_YYYYMMDD_HHMMSS.json
```

## Arquitetura interna

```text
pico_tui/
├── app.py                    aplicação e navegação
├── app.tcss                  layout e estilo
├── palette.py               paleta semântica
├── screens.py               modais
├── widgets.py               painéis da tela principal
├── serial_client.py         transporte serial e fallback POSIX
├── commands.py              construção de comandos
├── core/
│   ├── models.py            modelo canônico
│   ├── state_store.py       estado central thread-safe
│   ├── events.py            eventos de domínio
│   └── event_bus.py         publicação e assinatura
├── protocol/
│   ├── router.py            autodetecção de protocolo
│   ├── sensor_direct.py     console do Pico
│   ├── gateway_text.py      protocolo do gateway
│   ├── legacy_gateway.py    compatibilidade da fase 1
│   ├── can_id.py            ID de 29 bits
│   ├── sequence.py          sequência de 16 bits
│   ├── fragments.py         remontagem
│   └── crc.py               CRC16 e CRC32
└── services/
    ├── controller.py        eventos → estado
    ├── demo.py              rede simulada
    └── log_manager.py       JSONL e CSV
```

A thread serial nunca altera widgets diretamente. Linhas são encaminhadas ao loop da aplicação, decodificadas em eventos e aplicadas ao estado canônico. A interface consome snapshots desse estado.

## Testes

Instalação de desenvolvimento:

```bash
python -m pip install -e '.[dev]'
```

Execução:

```bash
pytest
ruff check pico_tui tests
python -m compileall -q pico_tui tests
```

A suíte utiliza:

- modo headless do Textual;
- pseudo-terminal POSIX;
- firmware serial simulado;
- mensagens de gateway simuladas;
- testes de sequência, CRC, fragmentação e estado.

## Limitações conhecidas

1. O protocolo serial binário definitivo do gateway ainda não foi definido.
2. Os valores oficiais de `DOMAIN` e `MSG_TYPE` ainda precisam ser fixados no firmware.
3. `DRDY` é aceito apenas como estado legado/experimental e não integra a baseline final.
4. A seleção de quantidade de bins no modo direto depende do firmware; o comando atual é apenas `FFT ONCE`.
3. A FFT direta do Pico depende de uma extensão do protocolo serial.
4. A ocupação do barramento é exibida quando reportada pelo gateway; o cálculo bit a bit ainda não é realizado localmente.
5. SocketCAN e replay completo permanecem planejados para uma versão posterior.
6. A limpeza de DTC pelo modo direto depende de suporte futuro no firmware.

## Documentos adicionais

- `INTEGRATION_NOTES.md`: comparação e decisões da integração.
- `GATEWAY_PROTOCOL_REFERENCE.md`: protocolo textual de referência.
- `REQUIREMENTS_COVERAGE.md`: cobertura dos requisitos atuais.

## Navegação F3 e configuração rápida

- `F3` abre uma janela hierárquica. Use `↑`/`↓` e `Enter` para selecionar um sensor.
- `Ctrl+N` apenas devolve o foco à árvore do dashboard.
- Selecionar modo ou janela na coluna **Configuração Rápida** envia o comando `SET` e deixa o valor estagiado.
- O botão **Aplicar** envia `APPLY`. A TUI mantém a transação como `QUEUED` até receber `CONFIG_APPLIED`.



## Segurança operacional e YubiKey

A TUI agora possui uma camada local de proteção para comandos mutáveis. Por padrão, `--security-mode presence` exige uma YubiKey conectada por USB para ações como `SET`, `APPLY`, `TELEMETRY ON`, `DTC CLEAR`, `RESET`, `ACQ POLLING` e `NET WIFI ON/OFF`.

Execução recomendada:

```bash
pico-tui --port /dev/ttyACM0 --mode sensor --security-mode presence
```

Modos disponíveis:

```text
--security-mode presence   exige presença USB de YubiKey
--security-mode otp        exige :unlock <OTP-da-YubiKey>
--security-mode off        desativa proteção operacional
```

Comandos internos:

```text
:security
:unlock <otp>
:lock
```

Esta proteção é operacional/local. Ela cria uma barreira física para operação na TUI e reforça o caráter de sistema desconectado. Para autenticação criptográfica forte, a evolução recomendada é FIDO2/HMAC challenge-response.

## Wi-Fi sob escolha explícita do operador

O sensor Pico inicia com Wi-Fi desligado. A TUI deve ligar a rede apenas por ação humana:

```text
:wifi on
:wifi off
:wifi status
```

Em modo sensor direto, esses comandos enviam ao firmware:

```text
NET WIFI ON
NET WIFI OFF
NET WIFI STATUS
```

Assim, a conexão do nó wireless ao gateway deixa de acontecer automaticamente no boot.
