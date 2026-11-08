# Protocolo

Referência das mensagens trocadas no sistema: no barramento CAN, entre a estação e a Probe 00, por BLE e pelo canal de dados Wi-Fi. A [Arquitetura](arquitetura.md) explica o porquê de cada mecanismo; aqui ficam os formatos.

**Convenções.** Identificadores CAN de 11 bits, quadros de até 8 bytes, CAN clássico a 500 kbit/s. Números de vários bytes em little-endian, salvo indicação. `N` é o número do módulo (0 é a Probe 00). Um identificador CAN tem um único transmissor: dois equipamentos transmitindo no mesmo identificador poderiam colidir sem arbitragem, por isso os relatos de cada módulo usam identificadores próprios.

## 1. Mapa de identificadores CAN

Quanto menor o identificador, maior a prioridade no barramento.

| Identificador | Quem transmite | Conteúdo | Seção |
|---|---|---|---|
| `0x010` | Probe 00 | carga artificial de alta prioridade (ensaio) | [12](#12-ensaios-de-falha) |
| `0x050` | candidatos | eleição | [2](#2-coordenação-da-rede) |
| `0x060` | líder eleito | anúncio de líder | [2](#2-coordenação-da-rede) |
| `0x080` | Probe 00 e módulos | comandos de controle e estado dos módulos | [2](#2-coordenação-da-rede) |
| `0x100` | líder | sinal de presença | [2](#2-coordenação-da-rede) |
| `0x120 + N` (`0x120`–`0x13F`) | Probe 00 e módulo N | hora e pedido de hora | [4](#4-relógio-da-rede) |
| `0x1F0` | Probe 00 | carga artificial de prioridade intermediária (ensaio) | [12](#12-ensaios-de-falha) |
| `0x200` | módulos | função local de demonstração | [2](#2-coordenação-da-rede) |
| `0x280 + 2N`, `+1` (`0x282`–`0x2BF`) | módulo N | sensor ouvido por BLE, em duas partes | [5](#5-descoberta-ble) |
| `0x300`, `0x301` | Probe 00 | pedido de associação, em duas partes | [6](#6-associação) |
| `0x302`, `0x303` | líder | pedido de reassociação, em duas partes | [6](#6-associação) |
| `0x304` | Probe 00 | comando a um sensor sem fio | [7](#7-canal-de-dados) |
| `0x305` | Probe 00 | comando de ensaio | [12](#12-ensaios-de-falha) |
| `0x306` | módulos | estado de ensaio | [12](#12-ensaios-de-falha) |
| `0x320 + 2N`, `+1` (`0x320`–`0x35F`) | módulo N | estado de um vínculo, em duas partes | [6](#6-associação) |
| `0x380 + N` (`0x381`–`0x39F`) | módulo N | dados dos sensores do módulo, segmentados | [7](#7-canal-de-dados) |
| `0x7EF` | Probe 00 | carga artificial de baixa prioridade (ensaio) | [12](#12-ensaios-de-falha) |

`0x7EF` é o maior identificador permitido: os sete bits mais significativos não podem ser todos recessivos. As definições ficam em `Codigo/node-can/include/can_ids.h`; o painel e a estação mantêm cópias conferidas por teste.

## 2. Coordenação da rede

### Comandos de controle (`0x080`)

Comandos de quatro bytes, `opcode subcomando alvo ação`. O alvo `FF` é a rede inteira.

| Comando | Efeito |
|---|---|
| `22 00 FF 01` | iniciar eleição |
| `22 10 ID 00` | desativar a função local do módulo |
| `22 10 ID 11` | reativar a função local do módulo |
| `22 10 ID 33` | confirmar falha do módulo |
| `22 10 ID 44` | limpar falha do módulo |
| `22 20 FF 00` | consultar o estado de todos os módulos |
| `22 20 ID 00` | consultar o estado de um módulo |
| `22 30 FF 01`..`05` | período do sinal de presença: 2000, 1500, 1000, 750 ou 500 ms |

Respostas e atualizações usam o opcode `23`: `23 21 ID estado` (resposta de estado), `23 41` (entrada aceita), `23 50`/`23 51` (atualização de estado), `23 52` (confirmação de comando). Estados: `00` líder, `01` seguidor, `02` desativado, `03` em falha, `04` Probe, `05` entrando, `06` conflito de líder.

### Sinal de presença (`0x100`)

O líder o emite a cada período. A Probe 00 o relata como:

```text
[GW] HEARTBEAT lider=3 rodada=17 sensores_ativos=3 modo=3 periodo=1000 ms t_us=...
```

## 3. Probe 00: interface de texto

A estação conversa com a Probe 00 por linhas de texto na serial USB (115200 baud). O que não é reconhecido como comando de texto é lido como comando CAN de quatro bytes em hexadecimal (seção 2).

### Comandos

| Comando | Efeito |
|---|---|
| `PROBE_VERSION` (ou `VERSION`) | versão do firmware, protocolo e lista `FEATURES` |
| `PROBE_STATUS` (ou `STATUS`) | estado do CAN e tempo ligado |
| `22 ...` | comando CAN de quatro bytes (seção 2) |
| `WIRELESS BIND <node> <uuid> [AUTO]`, `UNBIND`, `POLICY` | associação (seção 6) |
| `CMD TARGET=NN.CC ACTION=... TX=<id>` | comando a um sensor sem fio (seção 7) |
| `TIME [<ms UTC>]` | mostra ou ajusta a hora da rede (seção 4) |
| `METRICS ON\|OFF\|RESET` | medição do barramento (seção 11) |
| `FAULT ...` | ensaios de falha (seção 12) |
| `ECHO`, `CONFIG`, `BENCH CRYPTO` | instrumentação de medição (seção 11) |

`FEATURES` diz o que o firmware da Probe entende: `WIRELESS_DATA`, `ACOUSTIC_DATA`, `BUS_METRICS`, `FAULT_INJECTION`, `FAILOVER`, `RESEARCH` e `TIME_SYNC`. A estação só usa um recurso que a Probe anuncia.

### Linhas emitidas

As linhas que começam com `[GW]` são fatos da rede observados pela Probe; as demais (`TEL`, `FRAG`, `DTC`, `ACK`, `PANEL`) são dados remontados do barramento. Cada formato aparece na seção do assunto.

## 4. Relógio da rede

Quadro de 8 bytes em `0x120 + N`. Formato em `Codigo/common/ioc_link/ioc_time.h`.

```text
tipo(1) | fonte(1) | segundos UTC desde 1970 (4) | milissegundos (2)
tipo:  1 hora, 2 pedido de hora
fonte: 0 sem hora, 1 terminal, 2 relógio de tempo real (RTC), 3 operador
```

- **Ajuste.** `TIME <ms UTC desde 1970>` na serial da Probe ajusta a hora com fonte terminal e a divulga em `0x120`. A estação o envia ao conectar, se a Probe anuncia `TIME_SYNC`, e a cada 10 minutos. `TIME` sozinho mostra a hora atual.
- **Pedido.** Leva a fonte e a hora de quem pede (zeros se não tem). Responde quem tem uma hora que o solicitante adotaria, se for a Probe 00 ou o líder.
- **Adoção.** Sempre, se não há hora ou se a fonte recebida é melhor (número menor); da mesma fonte, só com diferença acima de 50 ms nos módulos e de 2 s no painel; nunca, de fonte pior.

```text
[GW] TIME_SYNC node=0 fonte=TERMINAL epoch=1793003613.505 ajuste_ms=12     (ajuste_ms=NA na primeira hora)
[GW] TIME_REQ node=2
```

## 5. Descoberta BLE

Anúncio do sensor (*Manufacturer Specific Data*):

```text
company-id(2) | "IC" | versão do anúncio(1) | perfil(1) | protocolo(1) | UUID(8)
```

Perfis: `0x01` vibração, `0x02` acústico, `0x04` painel. Protocolo atual: `6`. Cada módulo relata o que ouve em dois quadros:

```text
0x280 + 2N       parte A: seq | primeiros 7 bytes do UUID
0x280 + 2N + 1   parte B: seq | último byte do UUID | perfil | RSSI | protocolo | versão do anúncio
```

A Probe junta as partes de mesma sequência:

```text
[GW] WIRELESS_CANDIDATE reporter=2 uuid=0xE6616408432B6F39 profile=VIBRATION rssi=-55 protocol=6
```

## 6. Associação

### Comandos de texto

```text
WIRELESS BIND <node> <uuid> [AUTO]          associa; AUTO autoriza a reassociação automática
WIRELESS UNBIND <node> <uuid>               desassocia
WIRELESS POLICY <node> <uuid> AUTO|MANUAL   muda a política de um vínculo
```

### Quadros de pedido

```text
0x300  Probe 00, parte A: seq | alvo | ação | primeiros 5 bytes do UUID
0x301  Probe 00, parte B: seq | alvo | ação | últimos 3 bytes do UUID | 2 bytes sem uso
0x302  líder, parte A: mesmo formato
0x303  líder, parte B: seq | alvo | ação | últimos 3 bytes do UUID | módulo de origem | RSSI
```

| Ação | Valor | Emissor |
|---|---|---|
| associar | `0x01` | Probe 00 |
| desassociar | `0x02` | Probe 00 |
| associar com política automática | `0x03` | Probe 00 |
| política automática / manual | `0x04` / `0x05` | Probe 00 |
| reassociar: responsável fora do ar | `0x11` | líder |
| reassociar: sensor fora do alcance do responsável | `0x12` | líder |

Todos os módulos remontam os pedidos; só o alvo executa. Uma ação recebida no par de identificadores do outro emissor é descartada.

### Estado do vínculo

```text
0x320 + 2N       parte A: seq | filho | estado | perfil | primeiros 4 bytes do UUID
0x320 + 2N + 1   parte B: seq | filho | RSSI | protocolo | últimos 4 bytes do UUID
```

No byte de estado, o bit 7 indica política automática e o bit 6, sessão de dados autenticada; os seis bits baixos são o estado: `01` DISCOVERED, `02` ASSOCIATING, `03` BOUND, `04` ONLINE, `05` STALE, `06` LOST, `07` UNBOUND. `0x7F` (REJECTED: o sensor não foi ouvido recentemente ou não há filho livre) vai sem os bits de indicação. O módulo publica o estado a cada mudança e a cada 10 s.

```text
[GW] WIRELESS_ASSOC node=1 child=1 uuid=0xE6616408432B6F39 profile=VIBRATION state=ONLINE rssi=-48 protocol=6 failover=AUTO session=SECURE
[GW] WIRELESS_FAILOVER uuid=0xE6616408432B6F39 from=1 to=2 reason=OWNER_LOST rssi=-68
```

## 7. Canal de dados

### Oferta (BLE, módulo → sensor)

27 bytes, não conectável, anunciada a cada 100 ms:

```text
company(2)=0xFFFF | "IO" | versão=1 | node | UUID(8, big-endian) | canal | nonce(4, big-endian) | tag(8)

tag = HMAC-SHA256(K_dev, "ioc-offer-v1" || bytes[2..18])[0..7]
```

### Datagramas UDP (porta 4242)

```text
"IL" | versão=6 | tipo | contador(4) | conteúdo | tag(8)

tag = HMAC-SHA256(chave, cabeçalho || conteúdo)[0..7]
```

| Tipo | Sentido | Chave | Conteúdo |
|---|---|---|---|
| `0x01` HELLO | sensor → módulo | do dispositivo | UUID, perfil, protocolo, nonce do sensor (8) |
| `0x02` CHALLENGE | módulo → sensor | do dispositivo | módulo, filho, nonce do módulo (8), eco do nonce do sensor |
| `0x03` CONFIRM | sensor → módulo | de sessão | eco dos dois nonces |
| `0x10` DATA_UP | sensor → módulo | de sessão | mensagem do sensor |
| `0x11` DATA_DOWN | módulo → sensor | de sessão | comando do módulo |

O contador começa em 1 em cada sentido e só é aceito se for maior que o último. As mensagens transportadas por `DATA_UP` e `DATA_DOWN` estão definidas em `Codigo/node-wifi/edge_protocol_definitions.h`.

### Comando a um sensor (`0x304`)

```text
seq | node | filho | opcode | arg0 | arg1 | arg2 | arg3
```

| Opcode | Ação | Executada por |
|---|---|---|
| `0x01` | telemetria: liga, desliga, uma amostra, período | módulo |
| `0x02` | espectro com N faixas | módulo, com a próxima amostra |
| `0x03` | republicar o estado do canal e a configuração | módulo |
| `0x10`–`0x14` | preparar modo, taxa, janela, limiar ou ganho | módulo (cópia em edição) |
| `0x1F` | aplicar a configuração preparada | sensor |
| `0x20` | limpar alarmes (DTC) | sensor |
| `0x21` | sensor acústico: reiniciar a medição, aprender ou descartar a assinatura | sensor |

Cada comando recebe uma confirmação `ACCEPTED`, `APPLIED`, `REJECTED`, `NO_LINK` (sem sessão) ou `UNKNOWN`. Ações executadas pelo sensor confirmam `ACCEPTED` na hora e o resultado quando o sensor responde, ou `REJECTED` com `REASON=SENSOR_TIMEOUT` depois de 3 s. Enquanto o módulo não conhece a configuração atual do sensor, `0x10`–`0x1F` são recusados com `REASON=CONFIG_UNKNOWN`. O argumento 1 de `0x10` é o perfil a que o modo pertence; um modo de outro perfil é recusado com `REASON=PROFILE_MISMATCH`.

### Dados dos sensores (`0x380 + N`)

Cada transferência é dividida em segmentos:

```text
byte 0     filho (3 bits) | tipo (5 bits)
byte 1     número da transferência (3 bits) | índice do segmento (5 bits)
índice 0:  byte 2 = tamanho total, byte 3 = CRC-8 (polinômio 0x07), bytes 4..7 = dados
índice >0: bytes 2..7 = dados
```

Até 190 bytes (32 quadros) por transferência. Uma lacuna, um CRC errado ou 1,5 s sem completar descartam a transferência.

| Tipo | Conteúdo | Bytes | Quadros |
|---|---|---|---|
| 1 | telemetria do sensor de vibração | 50 | 9 |
| 2 | bloco de espectro: cabeçalho de 17 bytes e até 80 faixas de 16 bits | até 177 | até 30 |
| 3 | confirmação de comando: seq, opcode, resultado, detalhe | 5 | 2 |
| 4 | alarme (DTC): código, sintoma, severidade, instante | 8 | 2 |
| 5 | estado do canal de dados | 19 | 4 |
| 6 | configuração do sensor | 22 | 4 |
| 7 | relatório do sensor acústico | 108 | 19 |
| 8 | estado e marcas do painel | variável | — |
| 9 | custo do canal de dados medido no módulo (instrumentação) | — | — |
| 10 | etapa da criação do canal de dados | 8 ou 16 | 2 ou 3 |

### Comandos e linhas na Probe

```text
CMD TARGET=01.01 ACTION=TELEMETRY TX=A1B2C3 STATE=ON|OFF|ONCE|FAST|SLOW
CMD TARGET=01.01 ACTION=TELEMETRY_PERIOD TX=... PERIOD_MS=500
CMD TARGET=01.01 ACTION=FFT TX=... BINS=64
CMD TARGET=01.01 ACTION=CONFIG TX=... MODE=STRUCTURAL RATE_HZ=500 WINDOW=HAMMING WINDOW_SIZE=256 STALTA=3.5 GAIN=1.25
CMD TARGET=01.01 ACTION=DTC_CLEAR TX=...
CMD TARGET=01.01 ACTION=STATUS TX=...
```

```text
TEL NODE=1 CHILD=1 SEQ=0 GW_TS_MS=... MODE=ROTATING ACQ=POLLING WIN=512 ... RMS=... RMS_UNIT=m/s2 ...
FRAG NODE=1 CHILD=1 TYPE=FFT TRANSFER=1 INDEX=0 COUNT=1 FORMAT=U16_SCALED SCALE=... RATE_HZ=... FFT_SIZE=128 WINDOW=HANN DATA=<hex>
DTC NODE=1 CHILD=1 CODE=0x2002 SYMPTOM=0x16 SEVERITY=1 TS_MS=...
ACK COMMAND=CONFIG STATE=APPLIED TX=A1B2C3 NODE=1 CHILD=1 DETAIL=1 RTT_MS=42
[GW] WIRELESS_LINK node=1 child=1 state=SECURE rssi=-47 age_s=12 rx=30 auth_fail=0 replay=0 lost=0 stream=ON period_ms=1000 rtt_ms=4 rtt_max_ms=9
[GW] WIRELESS_CONFIG node=1 child=1 status=APPLIED mode=STRUCTURAL rate_req_hz=500.00 rate_eff_hz=500.00 window=HAMMING window_size=256 stalta=3.500 gain=1.2500 profile=VIBRATION
[GW] WIRELESS_EVENT node=1 child=1 event=HELLO_REJECT reason=KEY count=4 value=2 stations=1 since_s=11
```

- `SEQ` é numerado pelo módulo a cada amostra encaminhada: um salto é uma perda no barramento ou na remontagem. As perdas no Wi-Fi aparecem em `lost`.
- `RTT_MS` é o tempo entre o envio do comando pela Probe e a confirmação. `rtt_ms` e `rtt_max_ms` são a ida e volta entre módulo e sensor; `-1` indica que ainda não há medida.
- No formato `U16_SCALED`, cada faixa vale `valor × SCALE / 65535`.
- `state` do canal: `DOWN`, `OFFERING` (oferta no ar), `HANDSHAKE` (autenticação em andamento), `SECURE` (sessão ativa) ou `NO_KEY` (módulo sem chave).
- `ACQ`, `WIFI` e `DTC_LIST` existem só no console USB do sensor; pela Probe são recusados com `REASON=UNSUPPORTED_VIA_NODE`.

### Etapas da criação do canal (tipo 10)

```text
evento | motivo | quantidade(2) | valor | estações | segundos desde o vínculo(2) [| UUID(8)]
```

| Evento | Quando | `value` |
|---|---|---|
| `AP_UP` | o ponto de acesso subiu, ou o sensor foi associado com ele já no ar | canal |
| `AP_DOWN` | o ponto de acesso foi desligado (`reason` diz por quê) | — |
| `AP_ERROR` | o ponto de acesso não subiu; nova tentativa em 5 s | — |
| `NO_KEY` | o módulo não tem chave | — |
| `OFFER` | primeira oferta deste sensor desde o vínculo ou a última sessão | canal |
| `OFFER_ERROR` | o anúncio BLE não começou | canal |
| `STATION_JOIN`, `STATION_LEAVE` | uma estação entrou ou saiu do ponto de acesso (filho 0) | — |
| `STATION_IP` | a estação recebeu endereço (filho 0) | último número do IP |
| `HELLO_OK` | HELLO conferido, CHALLENGE enviado | último número do IP |
| `HELLO_REJECT` | HELLO recusado (`reason`) | último número do IP |
| `HANDSHAKE_TIMEOUT` | o CHALLENGE ficou 5 s sem resposta | último número do IP |
| `SESSION_UP` | sessão autenticada | último número do IP |
| `SESSION_DOWN` | sessão encerrada (`reason`) | — |

Motivos: `NONE`, `FORMAT` (HELLO mal formado), `NOT_BOUND` (sensor não associado a este módulo; o relato traz o UUID), `KEY` (chave não confere), `PROTOCOL` (versão diferente), `REPLAY` (repetição do HELLO da sessão em vigor), `BUSY` (autenticação anterior em andamento), `SEND` (falha ao enviar o CHALLENGE), `TIMEOUT`, `UNBOUND`, `SLEEP` (painel dormindo), `TEST` (ensaio), `RADIO_OFF`, `IDLE` (nenhum sensor associado). O filho 0 marca um fato do módulo, antes de o sensor se identificar. Fatos repetidos são agrupados: o primeiro sai na hora e os seguintes no máximo a cada 5 s, com `count` igual ao número agrupado.

## 8. Sensor acústico no canal de dados

O relatório de um segundo chega ao barramento como transferência do tipo 7: 108 bytes, 19 quadros, níveis em centésimos de dB e `-32768` para o que o modo não calcula. A Probe o escreve numa linha:

```text
TEL NODE=N CHILD=C SEQ=... GW_TS_MS=... PROFILE=ACOUSTIC MODE=LEVEL|MACHINE|EVENTS ACQ=I2S WIN=... RATE_EFF_HZ=...
    INTERVAL_MS=... COVERAGE_PCT=... CPU_PCT=... SPECTRUM_VALID=... OVERLOAD=... MIC_FAULT=...
    LAEQ_DB=... LCEQ_DB=... LZEQ_DB=... LZPEAK_DB=... LAF_DB=... LAFMAX_DB=... LAFMIN_DB=... CREST=... KURT=...
    PEAK_HZ=... CENTROID_HZ=... FLATNESS=... OCT_DB=b1,b2,...,b10 <chaves do modo> BATT_PCT=... BATT_MV=... DTC=... DTC_COUNT=...
```

Chave ausente é valor não calculado; `-` em `OCT_DB` é banda acima da metade da taxa de amostragem. As chaves de cada modo estão em [Sensor acústico](sensor-acustico.md#82-do-node-à-tui). Comandos próprios:

```text
CMD TARGET=NN.CC ACTION=CONFIG MODE=LEVEL|MACHINE|EVENTS [RATE_HZ=...] [FFT_SIZE=...] [THRESHOLD=...] [GAIN=...] TX=<id>
CMD TARGET=NN.CC ACTION=MEASURE_RESET TX=<id>
CMD TARGET=NN.CC ACTION=BASELINE_LEARN SECONDS=5..3600 TX=<id>
CMD TARGET=NN.CC ACTION=BASELINE_CLEAR TX=<id>
```

`THRESHOLD` e `FFT_SIZE` são sinônimos de `STALTA` e `WINDOW_SIZE`. As três ações acústicas usam o opcode `0x21` (argumento 0: ação 1, 2 ou 3; argumentos 1 e 2: segundos); o `DETAIL` da confirmação final é o número da ação.

## 9. Painel

O painel usa o canal de dados dos sensores (seção 7). Dentro de `DATA_UP` e `DATA_DOWN` vão mensagens com `magic` (`0xAA55`) e comando. Formatos em `Codigo/common/ioc_panel/ioc_panel_proto.h`.

| Comando | Sentido | Conteúdo |
|---|---|---|
| `0x30` FRAMES | módulo → painel | lote de quadros CAN observados pelo módulo |
| `0x31` HELLO | painel → módulo | pede o retrato e a hora; informa o intervalo de sono |
| `0x32` STATUS | painel → módulo | alimentação, nível da página, relógio, contadores |
| `0x33` MARK | painel → módulo | marca do operador |
| `0x34` MARK_ACK | módulo → painel | marca entregue ao barramento |

**FRAMES:** cabeçalho de 13 bytes (marcas, número do lote, tempo coberto, bits não encaminhados, contagem) e quadros de até 13 bytes (idade, identificador, tamanho, dados). Marcas do lote: `0x01` retrato (último quadro de cada estado), `0x02` caixa postal (eventos acumulados enquanto o painel estava desconectado), `0x04` ao vivo, `0x08` fim do retrato e da caixa postal. A idade usa 16 bits: em milissegundos até 32 767 ms e, acima disso, em segundos.

**HELLO:** versão, sinais e intervalo de sono (0 = sempre acordado). Sinais: `0x01` pede o retrato; `0x02` pede a hora. Com `0x02`, o módulo responde com o próprio quadro de hora num lote à parte, antes do retrato; sem hora, pergunta ao barramento (seção 4).

**MARK:** tipos `BUTTON` (instante marcado num botão), `NOTE` (texto), `STEP` e `RESULT` (passos de roteiro), `FLAG` e `ACK` (ciência de alerta, com o nível reconhecido). STATUS e MARK sobem ao barramento como transferência do tipo 8, e a Probe as escreve como:

```text
PANEL NODE=2 CHILD=1 TYPE=STATUS STATE=AWAKE SUPPLY=USB SLEEP_S=0 LEVEL=2 PAGE=0x8B73DF90 PAGE_SEQ=1 REFRESHES=1 UPTIME_S=43 CLOCK=YES KEYBOARD=NO ALERT=UNACKED
PANEL NODE=2 CHILD=1 TYPE=MARK SEQ=1 KIND=ACK STEP=2 AGE_MS=850 TEXT_HEX=<texto em hexadecimal>
```

`LEVEL`: 0 rede normal, 1 a rede está tratando, 2 atenção, 3 requer você.

## 10. Console USB dos sensores

Comandos de leitura: `VERSION`, `STATUS`, `NET`, `AUTH STATUS`, `FFT ONCE`, `TELEMETRY ONCE`, `DTC`. Comandos que alteram o sensor exigem uma sessão de manutenção (`AUTH UNLOCK <token>`; ver [Segurança](seguranca.md)). `NET` mostra o estado do canal de dados, as chaves presentes (`LINK_KEYS=DEVICE`, `MASTER`, `NONE` ou `UUID_MISMATCH`), o módulo e o filho. O sensor escreve uma linha `[NET]` a cada passo até a sessão:

```text
[NET] oferta do Node 2 conferida: entrando no Wi-Fi IOC-02
[NET] Wi-Fi IOC-02: conectado, ip=192.168.4.2 gateway=192.168.4.1
[NET] HELLO 1 enviado ao Node em 192.168.4.1:4242 (ok)
[NET] sessao autenticada com o Node 2, filho 1
```

O módulo faz o mesmo no monitor serial dele, com linhas `[WLINK]` (oferta, estação conectada, endereço entregue, HELLO e o resultado da conferência). O console do sensor acústico acrescenta `SET MODE LEVEL|MACHINE|EVENTS`, `SET FFT`, `SET THRESHOLD`, `CAL <dB>`, `MEASURE RESET` e `BASELINE LEARN|CLEAR` ([Sensor acústico](sensor-acustico.md#73-console-usb)).

## 11. Medição

```text
METRICS ON|OFF|RESET

[GW] BUS_STATS t_ms=17300 win_ms=1000 frames=42 bits=4718 load=0.94 peak=2.63 rx_err=0 tx_err=0 eflg=0x00 rx_peak=7 ids=11 xfer_ok=20 xfer_err=0 own=2 untracked=0
[GW] ID_STATS id=0x100 win_ms=5000 n=5 dlc=5 bits=469 dt_min_us=999500 dt_avg_us=1000020 dt_max_us=1000700
```

| Campo | Significado |
|---|---|
| `load`, `peak` | ocupação média na janela de 1 s e a maior entre as fatias de 100 ms, em % |
| `rx_err`, `tx_err`, `eflg` | contadores e registrador de erros do controlador da Probe (`eflg 0x20` = fora do barramento) |
| `rx_peak` | maior ocupação da fila de recepção (capacidade: 128 quadros) |
| `xfer_ok`, `xfer_err` | transferências segmentadas remontadas e descartadas |
| `own` | quadros transmitidos pela própria Probe na janela |
| `untracked` | quadros de identificadores que não couberam na tabela (48 posições) |
| `dt_min_us`, `dt_avg_us`, `dt_max_us` | intervalo entre quadros do identificador na janela de 5 s |

A Probe tem também comandos de instrumentação, usados pela ferramenta de [Pesquisa](../Pesquisa/README.md): `ECHO <node>.<filho> <n> <período_ms> [LOCAL|SENSOR] [semente]` mede a ida e volta pelo caminho do canal de dados; `CONFIG` publica os parâmetros da compilação; `BENCH CRYPTO [n]` mede o custo da criptografia na placa. A transferência do tipo 9 vira a linha `[GW] WIRELESS_STATS`.

## 12. Ensaios de falha

```text
FAULT POWER_CYCLE  <node> <segundos>      módulo fora do ar e reinício
FAULT SILENCE      <node> <segundos>      módulo congelado; retoma com o estado que tinha
FAULT AP_OUTAGE    <node> <segundos>      ponto de acesso desligado
FAULT DROP_SESSION <node>                 sessões com os sensores descartadas
FAULT LOAD <pct> <segundos> [HIGH|MID|LOW|MIX] [semente]   carga gerada pela Probe
FAULT CANCEL [node]                       encerra o ensaio em curso
```

Quadros:

```text
0x305   comando: seq | alvo | tipo | arg0 | duração (2 bytes, em unidades de 100 ms) | arg1 | 0xA5
0x306   estado:  seq | node | tipo | estado | duração (2 bytes) | 0 | 0x5A
```

Tipos: `1` POWER_CYCLE, `2` SILENCE, `3` AP_OUTAGE, `4` DROP_SESSION, `5` carga, `0x0F` CANCEL. Estados: `1` iniciado, `2` encerrado, `3` recusado. A carga usa `0x010` (alta prioridade), `0x1F0` (intermediária) ou `0x7EF` (baixa); `MIX` alterna as três. Módulos compilados com `IOT_FAULT_INJECTION=0` ignoram `0x305`.

```text
[GW] FAULT_TX kind=POWER_CYCLE node=3 duration_ms=20000 seq=4
[GW] FAULT_EVENT node=3 kind=POWER_CYCLE state=STARTED duration_ms=20000 seq=4
[GW] FAULT_EVENT node=0 kind=BUS_LOAD state=ENDED duration_ms=10000 seq=5 load_pct=40 priority=HIGH frames=17316 achieved_pct=39.99
```

## 13. Alarmes (DTC)

| Código | Significado |
|---|---|
| `0x0000` | nenhum alarme ativo |
| `0x1001` | MPU6050 sem comunicação no I²C |
| `0x1002` | barramento I²C travado |
| `0x1003` | microfone INMP441 sem dados válidos (dado parado, pino L/R ou alimentação) |
| `0x2001` | vetor de gravidade fora da faixa: sensor solto, impacto ou montagem instável |
| `0x2002` | saturação do acelerômetro |
| `0x2003` | sobrecarga acústica, acima de cerca de 120 dB SPL |
| `0x3001` | processamento atrasado (no sensor acústico: blocos de áudio perdidos) |
| `0x3002` | núcleo de processamento sem progresso (sensor acústico) |
| `0x4001` | tensão de alimentação baixa |
| `0x4002` | falhas repetidas de envio pelo Wi-Fi |
| `0x4003` | mensagem recusada por chave de autenticação |
| `0x4004` | versão de protocolo incompatível |
| `0x4005` | mensagem repetida ou antiga (contador) |
| `0x4006` | configuração recusada por campo inválido |

A estação mostra o código bruto quando recebe um alarme fora do catálogo. Bateria `255 %` e `65535 mV` significam "sem medida".
