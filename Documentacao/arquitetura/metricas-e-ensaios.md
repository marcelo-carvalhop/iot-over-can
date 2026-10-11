# Métricas da rede, reassociação automática e ensaios de falha

Este documento descreve o que a versão 0.18 acrescenta ao projeto: a medição do barramento pela Probe 00, a análise de tempo de resposta, a reassociação automática de sensores sem fio, a vigilância do líder pelos próprios seguidores, os ensaios de injeção de falhas e a bancada virtual usada para verificar tudo isso sem hardware.

**Estado de validação.** O firmware do Node CAN e o da Probe 00 compilam para o ESP32 (arduino-esp32 2.0.17, ACAN2515, NimBLE-Arduino 2.5.1). Toda a lógica foi exercitada na bancada virtual, que executa o código real do firmware em vários Nodes simulados. Na primeira bancada física (08/10/2026) funcionaram o plano de dados e uma reassociação automática; o restante desta versão ainda não foi exercitado em hardware (ver [`../validacao-em-bancada.md`](../validacao-em-bancada.md)). Os tempos citados neste documento são os da bancada virtual; os de bancada física dependem do rádio e precisam ser medidos (seção 9).

## 1. Visão geral

| Recurso | Onde está | Quem usa |
|---|---|---|
| Ocupação do barramento, período por identificador, contadores de erro | `Codigo/node-can/src/net_metrics.cpp` (somente na Probe 00) | tela **Métricas da rede** (`F8`/`b`) |
| Tempo de ida e volta Node ↔ sensor e tempo de resposta de comandos | `Codigo/node-can/src/wireless_link.cpp` | telas Métricas e Sensor |
| Análise de tempo de resposta e capacidade restante | `Front/pico_tui/metrics/rta.py` | tela Métricas, exportação |
| Reassociação automática de sensores | `Codigo/common/ioc_link/ioc_failover.c`, `wireless_discovery.cpp` | líder da rede; política escolhida pelo operador |
| Vigilância do líder pelos seguidores | `Codigo/node-can/src/main.ino` | todos os Nodes funcionais |
| Ensaios de falha | `Codigo/node-can/src/fault_injection.cpp` | tela **Ensaios** (`F9`/`e`) |
| Bancada virtual e gravações | `Codigo/node-can/test/host/sim_network.cpp`, `Front/replays/` | testes automáticos, `--replay` |

## 2. Medição do barramento pela Probe 00

A Probe 00 mede o que observa, sem hardware adicional. Para cada quadro recebido (e para cada quadro que ela própria transmite, pois o controlador não devolve o que envia) o firmware calcula a duração exata no barramento: o MCP2515 entrega identificador, DLC e dados, e o restante do quadro é reconstruído.

```text
SOF(1) ID(11) RTR(1) IDE(1) r0(1) DLC(4) dados(8n) CRC(15)    sujeito a preenchimento de bits
delimitador CRC(1) ACK(2) EOF(7) intervalo entre quadros(3)   13 bits fixos
```

O CRC-15 (polinômio `0x4599`) e os bits de preenchimento são calculados por `Codigo/common/ioc_link/ioc_canbits.c`. A ocupação é a soma dos bits de todos os quadros da janela dividida pela capacidade do barramento na mesma janela (500 000 bits por segundo). O cálculo é conferido nos testes contra uma implementação independente em Python.

Duas linhas são emitidas:

```text
[GW] BUS_STATS t_ms=17300 win_ms=1000 frames=42 bits=4718 load=0.94 peak=2.63 rx_err=0 tx_err=0
     eflg=0x00 rx_peak=7 ids=11 xfer_ok=20 xfer_err=0 own=2 untracked=0
[GW] ID_STATS id=0x100 win_ms=5000 n=5 dlc=5 bits=469 dt_min_us=999500 dt_avg_us=1000020 dt_max_us=1000700
```

| Campo | Significado |
|---|---|
| `load` | ocupação média da janela de 1 s, em % |
| `peak` | maior ocupação entre as fatias de 100 ms da janela, em % |
| `rx_err`, `tx_err` | contadores de erro do controlador CAN da Probe (REC e TEC) |
| `eflg` | registrador de erros do MCP2515 (`0x20` = bus-off, `0x40`/`0x80` = transbordo de recepção) |
| `rx_peak` | maior ocupação da fila de recepção do driver desde a inicialização (capacidade: 128) |
| `ids` | identificadores com tráfego recente |
| `xfer_ok`, `xfer_err` | transferências segmentadas remontadas e descartadas, acumuladas |
| `own` | quadros transmitidos pela própria Probe na janela (comandos, pedidos de estado, ensaios e carga artificial) |
| `untracked` | quadros de identificadores que não couberam na tabela (48 posições) |
| `dt_min_us`, `dt_avg_us`, `dt_max_us` | intervalo entre quadros consecutivos do identificador na janela de 5 s |

`METRICS ON|OFF|RESET` controla a emissão. Ela começa ligada.

**Limite da medição de tempo.** O MCP2515 não registra o instante de chegada. A Probe marca o tempo quando o laço principal retira o quadro da fila do driver; por isso `dt_min_us` e `dt_max_us` incluem a latência desse laço (da ordem de 1 ms, maior enquanto a serial escreve linhas longas). O período médio não é afetado. Uma medição de variação abaixo de 1 ms exige um controlador com carimbo de tempo no hardware, o que o MCP2518FD oferece.

**Em que fatia um quadro é contado.** Pelo mesmo motivo, cada quadro entra na janela de 1 s e na fatia de 100 ms em que foi retirado da fila, não na que ocupou no barramento. A ocupação média não muda. O pico de 100 ms pode ficar acima do real quando o laço atrasa e retira de uma vez quadros que chegaram em fatias diferentes.

**O que a Probe não vê.** Quadros que ela não recebeu por transbordo, quadros com erro (descartados pelo controlador antes de chegar ao firmware) e retransmissões. Os contadores `rx_err`, `tx_err` e `eflg` indicam quando isso está acontecendo.

## 3. Tempos de resposta medidos

| Medida | Como é obtida | Onde aparece |
|---|---|---|
| Ida e volta Node ↔ sensor | o Node envia `CMD_PING` a cada 3 s com um contador; o sensor o devolve no `CMD_PONG` | `rtt_ms` e `rtt_max_ms` em `WIRELESS_LINK` |
| Comando → confirmação | a Probe guarda o instante de envio do quadro `0x304` e o compara com a chegada da confirmação | `RTT_MS` no fim da linha `ACK` |
| Intervalo da telemetria | a TUI mede o intervalo entre linhas `TEL` de cada sensor | tela Métricas |
| Amostras perdidas | saltos no número de sequência das linhas `TEL` | telas Métricas e Ensaios |
| Intervalo sem sinal de presença | a TUI mede o intervalo entre linhas `HEARTBEAT` | telas Métricas e Ensaios |

A ida e volta inclui o processamento no sensor e o tempo até o Node ler o datagrama no laço principal. O protocolo do sensor não foi alterado: ele já devolvia o contador do `PING`.

**O que conta como amostra perdida.** O Node encaminha ao barramento uma amostra a cada período configurado (1 s por padrão), mesmo que o sensor produza mais. O número de sequência da linha `TEL` é atribuído pelo Node a cada amostra encaminhada, e não o do sensor. Um salto, portanto, é uma amostra que o Node enviou e a Probe não remontou: perda no barramento ou na remontagem. As amostras que o Node deixa de encaminhar por causa do período não contam, e as que se perdem no Wi-Fi aparecem em `lost` na linha `WIRELESS_LINK`. Na versão 0.17 a linha trazia a sequência do sensor, e toda amostra não encaminhada aparecia como perda.

O formato do relatório de enlace no CAN passou de 15 para 19 bytes. A Probe aceita os dois tamanhos; um Node com firmware anterior aparece sem tempo de ida e volta.

## 4. Análise de tempo de resposta

A tela Métricas calcula, a partir das linhas `ID_STATS`, o pior tempo de resposta de cada mensagem. O método é o de Davis, Burns, Bril e Lukkien, *Controller Area Network (CAN) schedulability analysis: Refuted, revisited and revised* (Real-Time Systems, 2007), que corrige a análise de Tindell, Burns e Wellings (1995).

Cada identificador é um fluxo de prioridade fixa (o menor identificador vence a arbitragem):

| Símbolo | Significado | Origem |
|---|---|---|
| `C` | duração de um quadro com o pior preenchimento de bits: `47 + 8n + ⌊(34 + 8n − 1) / 4⌋` bits | DLC observado |
| `P` | período: o menor intervalo medido entre chegadas ou, para rajadas, o intervalo entre rajadas | `dt_min`, `dt_avg`, `dt_max` |
| `b` | quadros por rajada (estimado) | idem |
| `J` | variação da liberação | zero: `P` já é o menor intervalo observado |
| `D` | prazo | o próprio período |

```text
B_m    = maior C entre as mensagens de menor prioridade
t_m    = B_m + Σ_{k ∈ hp(m) ∪ {m}} b_k · ⌈(t_m + J_k) / P_k⌉ · C_k
Q_m    = b_m · ⌈(t_m + J_m) / P_m⌉
w_m(q) = B_m + q · C_m + Σ_{k ∈ hp(m)} b_k · ⌈(w_m(q) + J_k + τ) / P_k⌉ · C_k
R_m(q) = J_m + w_m(q) − ⌊q / b_m⌋ · P_m + C_m
R_m    = máximo de R_m(q)
```

Com `b = 1` as expressões são as do artigo. A extensão para rajadas trata os `b` quadros de uma transferência segmentada como liberados juntos a cada período. Uma amostra de telemetria ocupa 9 quadros; o relatório de enlace, 4.

**Como o tráfego medido vira modelo.** A linha `ID_STATS` traz a contagem e três intervalos por identificador. A função `flow_from_observation` distingue três formas de tráfego:

| Forma | Como é reconhecida | Modelo |
|---|---|---|
| Um quadro por vez | intervalos mínimo e médio próximos | `b = 1`, `P = dt_min`. É o modelo esporádico, com intervalo mínimo entre chegadas. Uma mensagem que falha um ciclo aumenta `dt_max` e não altera o modelo. |
| Rajadas periódicas (transferência segmentada) | intervalo mínimo menor que um quarto do médio e máximo maior que 1,5 vez o médio | com `b` quadros espaçados de `s`, vale `dt_avg = P / b` e `dt_max = P − (b − 1) · s`, de onde `b = (dt_max − s) / (dt_avg − s)` e `P = b · dt_avg`. O espaçamento `s` é o menor intervalo medido, limitado por baixo pela duração de um quadro. |
| Episódio isolado (respostas a um comando do operador) | todos os quadros em menos da metade da janela | uma rajada de `n` quadros por janela de 5 s. Tratar o intervalo entre esses quadros como período apontaria uma ocupação que o barramento não tem. |

Em um fluxo denso, cujo intervalo mínimo é da ordem da duração de um quadro (a carga artificial dos ensaios), o mínimo medido é efeito da fila da Probe e o período usado é o intervalo médio.

O tamanho da rajada é inferido de três números, não contado. A tela o apresenta como "cerca de N quadros (estimado)" e a exportação traz o campo `burst_estimated`.

**Verificação.** O teste `test_response_time_analysis_reproduces_the_published_counterexample` reproduz o exemplo do artigo: três mensagens de 1 ms com períodos de 2,5, 3,5 e 3,5 ms. A análise de 1995 responde 3 ms para a de menor prioridade; o valor correto, alcançado pela segunda instância do período ocupado, é 3,5 ms.

**Capacidade restante.** A tela informa quantos sensores sem fio a mais o barramento comporta, cada um com uma amostra de telemetria por segundo, em duas leituras: o limite em que todos os prazos ainda são cumpridos e o limite com a ocupação do modelo abaixo de 40 %. Com o tráfego da bancada virtual (três Nodes e um sensor) os valores são da ordem de 400 e 160 sensores.

**Ressalvas.** Qualquer conclusão tirada desta análise precisa levá-las em conta.

1. Os parâmetros são os observados na janela de medição, não o pior caso de projeto. Uma mensagem que não ocorreu não entra na conta.
2. O instante crítico supõe todas as mensagens liberadas juntas. Para o tráfego sincronizado pelo sinal de presença isso é pessimista.
3. O MCP2515 é usado com uma fila de transmissão em ordem de chegada e um único buffer de hardware. Dentro de um mesmo Node, um quadro de menor prioridade já enfileirado atrasa outro de maior prioridade. A análise não inclui esse efeito.
4. Erros de transmissão e retransmissões não são considerados.
5. O tamanho das rajadas é uma estimativa. Um resultado "não cumpre o prazo" em um identificador de tráfego em rajadas pede conferência com os intervalos medidos antes de qualquer conclusão.
6. A análise trata do barramento. Os limites práticos do projeto estão em outro lugar: quatro sensores por ponto de acesso e quatro vínculos por Node.

## 5. Reassociação automática de sensores

### 5.1 Política

A reassociação é uma autorização dada pelo operador, por sensor. O padrão continua manual.

```text
WIRELESS BIND <node> <uuid> AUTO            associa e autoriza a reassociação
WIRELESS POLICY <node> <uuid> AUTO|MANUAL   altera a política de um vínculo existente
```

Na TUI, a associação pergunta a política, e `f` na lista de associados da tela Sensores sem fio a altera.

### 5.2 Como a rede decide

Todos os Nodes funcionais mantêm a mesma tabela, alimentada pelo que já trafega no barramento: o estado de vínculo que cada Node responsável republica a cada 10 s e as observações BLE (RSSI) que cada Node relata. Só o líder em exercício consulta a tabela e age. Como a tabela existe em todos, um líder recém-eleito continua de onde o anterior parou.

Dois bits do byte de estado do vínculo, antes sem uso, levam a informação necessária:

| Bit | Significado |
|---|---|
| 7 | política AUTO |
| 6 | o Node tem sessão de dados autenticada com o sensor |

| Motivo | Condição | Ação do líder |
|---|---|---|
| `OWNER_LOST` | o Node responsável foi declarado ausente pelo líder, ou deixou de republicar o vínculo por 25 s | pedido de associação ao Node com melhor RSSI recente |
| `SENSOR_LOST` | o responsável está ativo, mas não tem sessão com o sensor nem ouve os anúncios dele (estado `STALE` ou `LOST`), e outro Node o observa | pedido de associação ao outro Node; o responsável anterior cede quando ouve o novo publicar o vínculo (seção 5.3) |

O destino precisa ter observado o sensor nos últimos 12 s com RSSI de pelo menos −90 dBm. Sem outro Node ao alcance, nada acontece.

O líder não desfaz o vínculo anterior. Se o pedido se perde ou o Node escolhido não responde, o sensor continua com o responsável que tinha, e o líder decide de novo depois do intervalo mínimo.

Regras que impedem uma decisão de deixar o sensor sem responsável:

| Regra | Motivo |
|---|---|
| Entre duas decisões para o mesmo sensor passam no mínimo 30 s, mesmo que o responsável mude entre elas | dá tempo ao sensor de trocar de ponto de acesso e evita alternância entre dois Nodes |
| O Node que aceita o pedido e não ouve o sensor em 15 s publica o vínculo como `STALE` | o vínculo não fica parado em `ASSOCIATING`; o líder pode escolher outro Node |
| As observações de um Node declarado ausente são descartadas | um Node que saiu do ar não é escolhido com base no que relatou antes de sair |
| Um líder que retoma de uma parada do próprio laço reinicia a contagem de silêncio dos vínculos e descarta as observações | ele não age com uma tabela que envelheceu enquanto estava parado |

O pedido usa o formato dos quadros de associação, em identificadores próprios do líder (`0x302`/`0x303`), com ações próprias (`0x11` e `0x12`), o Node de origem e o RSSI nos dois últimos bytes. Os identificadores `0x300`/`0x301` ficam só com a Probe 00: dois transmissores no mesmo identificador poderiam colidir sem arbitragem. Um Node só aceita as ações de reassociação em `0x302`/`0x303`, e só aceita ali essas ações. A Probe 00 registra a decisão:

```text
[GW] WIRELESS_FAILOVER uuid=0xE6616408432B6F39 from=1 to=2 reason=OWNER_LOST rssi=-68
```

### 5.3 Dois Nodes com o mesmo sensor

Pode acontecer por um instante, por exemplo quando o responsável volta de um travamento. A regra é local e não depende do líder: um Node que ouve outro publicar o vínculo de um sensor que ele também mantém cede se o outro tem sessão autenticada e ele não, ou se ele próprio já não ouve o sensor. Sem sessão em nenhum dos lados, permanece o de maior identificador. Quem tem sessão nunca cede.

Uma sessão de dados ativa conta como presença do sensor. O rádio BLE do Node divide o tempo com o ponto de acesso e pode deixar de ouvir anúncios; sem essa regra, um sensor entregando dados poderia ser dado como perdido.

### 5.4 O que limita o tempo de reassociação

Na bancada virtual, com o Node responsável desligado:

| Etapa | Instante |
|---|---|
| Líder declara o Node ausente (5 sinais de presença) | 5,0 s |
| Decisão e pedido ao novo Node | 5,2 s |
| Sensor desiste da sessão anterior (`NET_SESSION_TIMEOUT_MS`, 10 s desde a última atividade) | 7,8 s |
| Sensor aceita a oferta de outro Node (`NET_OFFER_SWITCH_HOLD_MS`, 15 s) | 22,8 s |
| Telemetria pelo novo Node | 24,5 s |

Quando o sensor sai do alcance do responsável (`SENSOR_LOST`), a decisão depende de o responsável declarar o vínculo `STALE`: 10 s para a sessão expirar e mais 15 s sem ouvir anúncios. Na bancada virtual a telemetria volta pelo novo Node em 26,9 s. Se o Node escolhido não assumir, a segunda tentativa só ocorre depois do intervalo mínimo de 30 s, e a telemetria volta em 56,9 s (cenários `failover_retry` e `failover_dead_target`).

A rede decide em cerca de 5 s quando o responsável cai. O restante é espera do sensor, definida por duas constantes do firmware do Pico W que esta versão não alterou (a única mudança no sensor são as mensagens de diagnóstico no console). Reduzi-las é o caminho para uma reassociação mais rápida, e deve ser feito com medição em bancada: a espera de 15 s existe para que uma oferta antiga repetida por um terceiro não faça o rádio alternar de ponto de acesso.

### 5.5 Limites

- Uma segunda tentativa de reassociação só ocorre 30 s depois da primeira. Nesse intervalo o sensor fica sem entregar dados se o Node escolhido não assumir.
- Os vínculos ficam em memória. Um Node que reinicia os perde; com política AUTO o líder devolve o sensor a ele quando volta a observá-lo e nenhum outro Node o alcança melhor.
- Um Node isolado do barramento, mas com o ponto de acesso ligado, mantém a sessão com o sensor. Os dados do sensor ficam retidos nele. Detectar esse isolamento no próprio Node (contador de erros de transmissão) é trabalho futuro.
- A decisão usa RSSI de anúncios BLE. É uma aproximação do alcance Wi-Fi.
- Os quadros de associação continuam sem autenticação no barramento (ver `modelo_de_seguranca.md`). Quem escreve no CAN pode pedir uma reassociação; não consegue ler nem forjar dados do sensor, que seguem protegidos pela sessão.

## 6. Vigilância do líder pelos seguidores

Até a versão 0.17 só a Probe 00 detectava a falta do líder e pedia a eleição. Sem a Probe ligada, uma rede que perdesse o líder ficava sem coordenação. Isso contrariava o papel da Probe, que é de instrumentação.

Agora cada seguidor vigia o sinal de presença. Sem recebê-lo por **6 períodos** (mais 20 ms por posição abaixo do identificador 31, para que o seguidor de maior identificador reaja primeiro), o seguidor pede a eleição. A Probe continua reagindo em 4 períodos: com ela presente, o comportamento de bancada é o mesmo de antes; sem ela, a rede se recupera sozinha.

| Situação | Tempo até a rede ter líder de novo (bancada virtual, período de 1 s) |
|---|---|
| Probe 00 presente | 11,5 s |
| Probe 00 sem transmitir | 14,1 s |

A eleição em si custa 5 s de descoberta, 1,5 s de recuperação e 1 s até o primeiro sinal de presença.

`-D IOT_FOLLOWER_LEADER_WATCHDOG=0` desativa a vigilância.

Três correções acompanham a mudança. Todas foram encontradas na bancada virtual ao aplicar o ensaio de líder congelado:

1. **Seguidor órfão.** Um seguidor ignorava o sinal de presença de um líder diferente do que ele seguia. Quando um líder travado retomava e o líder provisório cedia a ele, os seguidores do provisório deixavam de transmitir e eram declarados ausentes. O seguidor agora aplica a regra que os líderes já usavam entre si: adota o líder de maior identificador.
2. **Líder que volta de uma parada.** Um líder que ficava sem executar o laço por alguns segundos, ao retomar, encontrava todos os seguidores "calados" pelo mesmo tempo e os declarava ausentes de uma vez. Um Node que percebe que o próprio laço ficou parado por mais de um período rearma as vigilâncias.
3. **Eleição redundante.** Durante uma eleição pedida por um seguidor, a Probe continuava contando a falta de sinal de presença e pedia outra eleição 4 s depois, reiniciando a primeira. A Probe agora reconhece a eleição em curso.

## 7. Ensaios de falha

### 7.1 Comandos da Probe 00

```text
FAULT POWER_CYCLE  <node> <segundos>     Node fora do ar e reinício
FAULT SILENCE      <node> <segundos>     Node congelado; retoma com o estado que tinha
FAULT AP_OUTAGE    <node> <segundos>     ponto de acesso Wi-Fi desligado
FAULT DROP_SESSION <node>                sessões com os sensores descartadas
FAULT LOAD <pct> <segundos> [HIGH|LOW]   carga artificial gerada pela Probe
FAULT CANCEL [node]                      encerra o ensaio em curso
```

| Ensaio | O que o Node faz | O que não faz |
|---|---|---|
| `POWER_CYCLE` | desliga ponto de acesso e anúncio, põe o controlador CAN em modo somente escuta (não transmite nem confirma quadros), descarta o que recebe e reinicia ao final | não corta a alimentação: o reinício é por software |
| `SILENCE` | modo somente escuta e laço principal parado; ao final retoma com as variáveis como estavam | os rádios continuam ligados |
| `AP_OUTAGE` | encerra as sessões e desliga o ponto de acesso; religa ao final e, se o ponto de acesso não subir, tenta de novo a cada 5 s | não afeta o barramento |
| `DROP_SESSION` | esquece as chaves de sessão | o ponto de acesso continua ligado |
| `LOAD` | (Probe) transmite quadros de 8 bytes no ritmo pedido, em `0x010` (vence toda arbitragem) ou `0x7EF` (perde toda arbitragem) | os Nodes ignoram esses quadros |

A troca de modo do MCP2515 é confirmada pelo controlador. Se a entrada em modo somente escuta falha depois de três tentativas, o ensaio é recusado. Se a volta ao modo normal falha ao fim de um `SILENCE`, o Node repete o pedido a cada 100 ms até conseguir, para não ficar mudo no barramento.

Duração de 1 a 120 s; carga de 1 a 90 %. Um Node aceita um ensaio por vez e, em silêncio, só ouve o cancelamento. A Probe informa a carga efetivamente gerada, que pode ficar abaixo da pedida se o laço dela não acompanhar.

```text
[GW] FAULT_TX kind=POWER_CYCLE node=3 duration_ms=20000 seq=4
[GW] FAULT_EVENT node=3 kind=POWER_CYCLE state=STARTED duration_ms=20000 seq=4
[GW] FAULT_EVENT node=0 kind=BUS_LOAD state=ENDED duration_ms=10000 seq=5 load_pct=40 priority=HIGH frames=17316 achieved_pct=39.99
```

### 7.2 Segurança

Os comandos de ensaio derrubam Nodes e ocupam o barramento. A TUI os trata como mutáveis: passam pelo `SecurityManager` e pedem confirmação. No barramento, o quadro `0x305` não é autenticado, como os demais quadros de controle do perfil CAN clássico. Para uma instalação que não seja de bancada, compile os Nodes com `-D IOT_FAULT_INJECTION=0`, o que remove o tratamento do comando:

```bash
PLATFORMIO_BUILD_FLAGS="-D IOT_FAULT_INJECTION=0" ./Codigo/scripts/build_esp32_can_node.sh 1
```

### 7.3 Tela Ensaios

`F9` ou `e`. Cada ensaio pede o módulo, a duração e uma confirmação que mostra o comando. O resultado é medido a partir das mensagens da rede, sem anotação manual:

| Ensaio | Medidas principais |
|---|---|
| Queda do líder | falta percebida, rede com líder de novo, maior intervalo sem sinal de presença, eleições, líder ao final, retorno do módulo |
| Queda de um módulo | módulo declarado ausente, reassociações, sessão retomada, maior intervalo sem telemetria |
| Módulo congelado | as de coordenação, líderes observados, módulos marcados em falha |
| Queda do ponto de acesso, sessões descartadas | sessão encerrada, sessão retomada, telemetria perdida |
| Carga de alta ou baixa prioridade | ocupação máxima, pico, variação do sinal de presença, contadores de erro, telemetria perdida |
| Ensaio manual | as mesmas, para uma falha provocada à mão (desligar um módulo, desconectar o barramento) |

Um ensaio iniciado fora da tela (comando digitado, reprodução de gravação) é reconhecido pelas linhas `FAULT_TX` e `FAULT_EVENT`. `x` exporta medições e ensaios para `exports/` em JSON e CSV.

**Depois de um ensaio de queda.** O líder marca como falho o módulo que ficou ausente, e essa marcação é mantida até o comando *Limpar falha* (tela do módulo, Comandos). É o comportamento que o protocolo já tinha: uma falha confirmada não é apagada por eleição nem por reentrada na rede.

## 8. Bancada virtual

`Codigo/node-can/test/host/sim_network.cpp` executa a Probe 00 e três Nodes em um único processo. Cada Node é o firmware real (`src/main.ino` e os demais arquivos), compilado dentro de um namespace próprio, com suas próprias variáveis globais. A bancada fornece o que no laboratório é físico:

- o barramento CAN, com arbitragem por identificador, duração real de cada quadro a 500 kbit/s e confirmação (um quadro sem outro Node para confirmá-lo é retransmitido e incrementa o contador de erros);
- o relógio, em passos de 0,5 ms;
- o rádio BLE e o ponto de acesso Wi-Fi de cada Node;
- os sensores, que seguem as regras do firmware do Pico W (`edge_network_driver.c`) e usam a biblioteca `ioc_link`. Cada sensor produz duas amostras por segundo; o Node encaminha uma por segundo.

```bash
./Codigo/scripts/build_network_sim.sh /tmp/sim_network
/tmp/sim_network --list
/tmp/sim_network owner_failover --verbose
```

| Cenário | O que verifica |
|---|---|
| `formation` | eleição, sinal de presença, ciclo TDMA, estatísticas do barramento |
| `sensor` | associação, sessão, telemetria, tempos de resposta, mudança de política |
| `owner_failover` | Node responsável sai do ar; o líder reassocia; o Node volta sem disputar o sensor |
| `sensor_moves` | sensor muda de alcance; reassociação por `SENSOR_LOST` |
| `manual_policy` | política manual: ninguém assume o sensor |
| `leader_failure` | líder sai do ar e volta como seguidor |
| `leader_failure_no_probe` | o mesmo, com a Probe impedida de transmitir |
| `silence_leader` | líder congela e retoma; termina com um único líder e sem falhas indevidas |
| `ap_outage`, `drop_session` | reautenticação do sensor |
| `bus_load` | carga de 40 % de alta prioridade e carga de baixa prioridade |
| `two_sensors` | dois sensores redistribuídos entre dois Nodes, um deles o próprio líder |
| `failover_retry` | o Node escolhido perde o sensor antes de assumir; o vínculo passa a `STALE` e o líder escolhe outro depois do intervalo mínimo |
| `failover_dead_target` | o Node escolhido sai do ar antes de receber o pedido; o responsável anterior mantém o vínculo até a nova decisão |
| `silence_leader_with_sensor` | o líder fica 43 s parado e retoma; não reassocia um sensor que continuou funcionando |

A saída padrão são as linhas que a Probe 00 enviaria à TUI, com o instante simulado. `Codigo/scripts/record_network_scenarios.sh` as grava em `Front/replays/`, e os testes da TUI as entregam ao decodificador, ao domínio, às métricas e ao registrador de ensaios. Um teste refaz a gravação e a compara com a versionada: se o firmware mudar o que a Probe escreve, o teste acusa. As gravações são versionadas por uma exceção em `.gitignore`, que ignora os demais arquivos `.log`.

A TUI reproduz uma gravação sem hardware:

```bash
cd Front && python -m pico_tui --replay replays/owner_failover.log --replay-speed 4
```

**O que a bancada virtual não reproduz:** temporização de rádio, coexistência BLE/Wi-Fi, perda de quadros por ruído, a temporização real do MCP2515 por SPI, a latência da serial. Ela verifica a lógica do firmware e a integração com a TUI. Tempos de rádio e de reinício são parâmetros da simulação.

### 8.1 Ferramentas de simulação (futuro, prioridade muito baixa)

Registro de recomendação, sem compromisso de implementação. Nada aqui é necessário para o TCC: a bancada virtual atual já executa o firmware real dos Nodes e cobre os cenários da seção 8. Os itens abaixo só fazem sentido se sobrar tempo depois das pendências de bancada física, alimentação e comissionamento.

**Princípio.** A bancada em C++ continua sendo a simulação do sistema inteiro, porque roda o mesmo código que vai para o ESP32. As ferramentas abaixo a complementam; nenhuma a substitui.

| Ordem | Ferramenta (código aberto) | Papel | Observação |
|---|---|---|---|
| 1 | SocketCAN `vcan`, `can-utils`, `python-can`, Wireshark | ligar a bancada a um barramento CAN virtual do Linux, para observar os quadros com `candump` e Wireshark e injetar quadros com `cansend` ou scripts | com um adaptador USB-CAN compatível com SocketCAN (CANable ou candleLight, firmware aberto), os mesmos scripts de teste rodam contra o barramento físico |
| 2 | Robot Framework ou pytest | escrever os cenários como roteiros legíveis (passos, esperas, verificações) e gerar relatório | pytest já é usado na TUI; Robot Framework só se os roteiros tiverem de ser lidos por quem não programa |
| 3 | GNU Octave | analisar as exportações de Métricas e Ensaios (CSV/JSON), fazer varreduras de parâmetros chamando a bancada (`system()` ou `mkoctfile`) e modelos matemáticos: tempo de resposta no pior caso, ocupação do barramento, probabilidade de colisão de canais Wi-Fi | não reescrever a bancada em Octave: perderia o firmware real e laços com passos de 0,5 ms seriam lentos |
| 4 | OMNeT++ com INET e FiCo4OMNeT | estudos de escala (dezenas de Nodes e sensores, carga, falhas aleatórias), em que modelos substituem o firmware | FiCo4OMNeT modela CAN e FlexRay; licença acadêmica do OMNeT++. ns-3 (GPL) é alternativa, mas não tem CAN nativo |
| 5 | Renode | emular as placas no nível de instruções, com vários nós, CAN e BLE, e roteiros em Robot Framework | só para uma geração futura de hardware com microcontroladores suportados (por exemplo nRF52840) |

**Por que não emular as placas atuais.** O firmware dos rádios do ESP32 e do CYW43 do Pico W é fechado, e nenhum emulador aberto reproduz esses rádios. No Renode, o suporte a Xtensa se limita aos núcleos DSP do projeto SOF, não ao ESP32; o RP2040 tem modelos da comunidade, sem o CYW43. Por isso o comportamento de rádio continua a ser verificado em bancada física (seção 9 e `../validacao-em-bancada.md`).

**Primeiro passo, se um dia for feito.** Uma ponte entre a bancada e `vcan0`: cada quadro simulado é copiado para o barramento virtual do Linux, e quadros escritos em `vcan0` entram na simulação. É o item de menor custo e o que mais aproxima os testes virtuais dos físicos.

Referências: [Xtensa no Renode (SOF)](https://antmicro.com/blog/2022/01/xtensa-isa-in-renode-for-sof-project), [comunicação sem fio multibanda no Renode](https://antmicro.com/blog/2020/10/multi-band-wireless-communication-in-renode), [BLE no nRF52840 com Renode e Zephyr](https://zephyrproject.org/developing-and-testing-bluetooth-low-energy-products-on-nrf52840-in-renode-and-zephyr/), [modelos do RP2040 para Renode](https://xiao-seeed-rp2040-renode.readthedocs.io/en/latest/ROADMAP/), [FiCo4OMNeT](https://github.com/CoRE-RG/FiCo4OMNeT), [artigo do FiCo4OMNeT](https://arxiv.org/pdf/1609.05179).

## 9. Roteiro de bancada

Grave todos os Nodes e a Probe 00 com a mesma versão: o byte de estado do vínculo e o relatório de enlace mudaram de formato.

1. **Métricas.** Com a rede formada, abra `F8`. Confira a ocupação (da ordem de 1 %), o período do identificador `0x100` (1000 ms) e os contadores de erro em zero. Desconecte o barramento de um Node por alguns segundos e observe `tx_err`.
2. **Tempos de resposta.** Com um sensor associado, confira `Ida e volta` na tela do sensor e envie *Consultar estado*; o tempo do comando aparece em Métricas.
3. **Vigilância pelos seguidores.** Desligue a Probe 00 e depois o líder. Os seguidores devem eleger outro em cerca de 14 s (monitor serial: `[VIGILANCIA]`).
4. **Ensaio de queda do líder.** `F9`, *Queda do líder*, 20 s. Compare os tempos com os da seção 6.
5. **Reassociação.** Associe um sensor com reassociação automática a um Node que não seja o líder, com outro Node ao alcance. `F9`, *Queda de um módulo*. Anote a decisão e a volta da telemetria; depois use *Limpar falha* no módulo.
6. **Carga.** *Carga de alta prioridade*, 40 %, 15 s. Verifique se a carga gerada chega perto da pedida e se a rede se mantém.
7. **Exportação.** `x` em Métricas ou Ensaios; guarde os arquivos de `exports/` junto do registro da sessão.

O que observar com atenção, por não ser coberto pela bancada virtual: o modo somente escuta do MCP2515 (`changeModeOnTheFly`), a volta do ponto de acesso depois de `AP_OUTAGE`, a varredura BLE enquanto o ponto de acesso está ligado e o ritmo que a Probe consegue sustentar em `FAULT LOAD`.
