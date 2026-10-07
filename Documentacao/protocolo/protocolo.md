# Protocolo

## CAN de controle

Comandos de quatro bytes usam o formato:

```text
opcode subcomando alvo ação
```

Principais comandos:

```text
22 00 FF 01   iniciar eleição
22 10 ID 00   desativar Node
22 10 ID 11   reativar Node
22 10 ID 33   confirmar falha
22 10 ID 44   limpar falha
22 20 FF 00   consultar status global
22 20 ID 00   consultar status de um Node
22 30 FF 01..05  configurar período do liveness lease
```

Respostas de status usam `23 21 ID código`. Atualizações internas conhecidas, como `23 50`, são tratadas como manutenção de estado e não como erro de parser.

## Probe 00

A Probe responde aos comandos textuais:

```text
PROBE_VERSION
PROBE_STATUS
```

Os aliases `VERSION` e `STATUS` podem ser usados durante autodetecção da porta. A resposta contém versão de firmware, protocolo, estado CAN e uptime.

## Função local do Node CAN

A telemetria local de demonstração produz um byte `0xAA`. A TUI associa esse valor ao Node físico que o originou. O valor atual, a rodada e a idade da amostra são estado contínuo e não devem produzir eventos repetitivos na tela principal.

## Descoberta BLE

Manufacturer Data do Pico W:

```text
company-id(2) | "IC"(2) | adv-version(1) | profile(1) | protocol(1) | UUID64(8)
```

O perfil `0x01` representa `VIBRATION`. O protocolo atual é `6`.

Cada Node CAN envia a observação em dois frames clássicos consecutivos. Para o Node `N`:

```text
0x280 + 2*N      parte A: seq + primeiros 7 bytes do UUID
0x280 + 2*N + 1  parte B: seq + último byte UUID + profile + RSSI + protocol + adv-version
```

A Probe recompõe as duas partes quando a sequência coincide e emite:

```text
WIRELESS_CANDIDATE reporter=N uuid=0x... profile=VIBRATION rssi=-55 protocol=6
```


## Associação wireless

A associação é iniciada pela TUI e transmitida pela Probe 00 por um comando textual:

```text
WIRELESS BIND <node> <uuid>
WIRELESS UNBIND <node> <uuid>
```

O UUID é transportado pelo CAN clássico em dois frames de comando:

```text
0x300  parte A: seq + target + action + primeiros 5 bytes do UUID
0x301  parte B: seq + target + action + últimos 3 bytes do UUID
```

Cada Node publica o estado do vínculo em dois IDs exclusivos. Para o Node `N`:

```text
0x320 + 2*N      estado A: seq + child + state + profile + primeiros 4 bytes UUID
0x320 + 2*N + 1  estado B: seq + child + RSSI + protocol + últimos 4 bytes UUID
```

Estados de associação:

```text
DISCOVERED → ASSOCIATING → BOUND → ONLINE → STALE → LOST
                                      ↑         │       │
                                      └─────────┴───────┘  novo advertisement
```

`UNBOUND` indica remoção explícita e `REJECTED` indica que o Node não pôde aceitar o vínculo, por exemplo porque o UUID não foi observado recentemente ou porque não há `child_id` livre.

A Probe recompõe o estado e emite uma linha textual para a TUI:

```text
[GW] WIRELESS_ASSOC node=1 child=1 uuid=0xE6616408432B6F39 profile=VIBRATION state=ONLINE rssi=-48 protocol=6
```

Na operação normal da TUI, `F7`/`w` abre a tela Sensores sem fio. O operador seleciona o UUID, escolhe explicitamente o Node entre as observações recentes e confirma o `BIND`. Para remoção, a TUI envia `UNBIND` e mantém o filho visível até receber `UNBOUND`. Os comandos internos `:bind` e `:unbind` são recursos avançados, não o fluxo principal.

A associação atual é um vínculo lógico de identidade e responsabilidade. A continuidade dos advertisements BLE funciona como lease de presença. O Node CAN associado é a fonte autoritativa dos estados `ONLINE`, `STALE` e `LOST`; a TUI não deriva esses estados pela idade local da última mensagem. O lease considera o vínculo `STALE` após 15 s sem advertisement e `LOST` após 45 s. O scanner BLE desabilita explicitamente o filtro de duplicatas e possui watchdog para reinício caso a varredura seja interrompida. O Node republica o estado de cada vínculo a cada 10 s, além das transições, para que uma TUI aberta depois da associação reconstrua a topologia.

## Plano de dados wireless

O vínculo aceito dispara o plano de dados entre o sensor e o Node responsável. O resumo abaixo lista os contratos; a justificativa e o modelo de ameaças estão em [`../arquitetura/plano-de-dados-wireless.md`](../arquitetura/plano-de-dados-wireless.md).

Oferta de vínculo anunciada por BLE pelo Node (Manufacturer Data, 27 bytes):

```text
company(2)=0xFFFF | "IO" | versão | node | UUID64(8) | canal | nonce(4) | tag(8)
```

Datagrama UDP autenticado, porta 4242:

```text
"IL" | versão=6 | tipo | contador(4, LE) | payload | tag(8)

0x01 HELLO      sensor -> Node   chave do dispositivo
0x02 CHALLENGE  Node -> sensor   chave do dispositivo
0x03 CONFIRM    sensor -> Node   chave de sessão
0x10 DATA_UP    sensor -> Node   chave de sessão
0x11 DATA_DOWN  Node -> sensor   chave de sessão
```

Quadros CAN:

```text
0x304        comando: seq | node | filho | opcode | arg0..arg3
0x380 + N    dados do Node N, segmentados:
             filho(3)|tipo(5) , transferência(3)|índice(5) , dados
             índice 0: tamanho total, CRC-8, 4 bytes de dados
```

Tipos de transferência: `1` telemetria, `2` espectro, `3` confirmação de comando, `4` DTC, `5` estado do enlace, `6` configuração. Opcodes: `0x01` encaminhamento, `0x02` espectro, `0x03` estado, `0x10`–`0x14` preparação de configuração, `0x1F` aplicar, `0x20` limpar DTC.

Linhas trocadas entre a TUI e a Probe 00:

```text
CMD TARGET=NN.CC ACTION=<ação> TX=<id> [CAMPO=valor ...]

TEL NODE=N CHILD=C SEQ=... MODE=... RMS=... RMS_UNIT=m/s2 ...
FRAG NODE=N CHILD=C TYPE=FFT TRANSFER=... INDEX=... COUNT=... FORMAT=U16_SCALED SCALE=... DATA=<hex>
DTC NODE=N CHILD=C CODE=0x.... SYMPTOM=0x.. SEVERITY=. TS_MS=...
ACK COMMAND=<ação> STATE=ACCEPTED|APPLIED|REJECTED TX=<id> NODE=N CHILD=C DETAIL=... [REASON=...]
[GW] WIRELESS_LINK node=N child=C state=DOWN|OFFERING|HANDSHAKE|SECURE|NO_KEY rssi=... age_s=... rx=... auth_fail=... replay=... lost=... stream=ON|OFF period_ms=...
[GW] WIRELESS_CONFIG node=N child=C status=CURRENT|APPLIED|REJECTED mode=... rate_req_hz=... rate_eff_hz=... window=... window_size=... stalta=... gain=...
```

No formato `U16_SCALED`, cada valor de 16 bits é a fração do pico; a magnitude é `valor × SCALE / 65535`.

## Sensor wireless — console serial

Comandos de leitura principais:

```text
VERSION
STATUS
NET
AUTH STATUS
FFT ONCE
TELEMETRY ONCE
DTC
```

Comandos que alteram estado exigem uma sessão de manutenção autorizada. A aquisição operacional é `POLLING`; `DRDY` não está habilitado no fluxo atual.

## Telemetria de vibração

A telemetria pode incluir RMS, excesso de curtose, fator de crista, frequência e amplitude do pico espectral, entropia espectral, PPV, STA/LTA, clipping, bateria, modo, janela, tamanho do buffer e qualidade da FFT.

Bateria `255%` e `65535 mV` representam informação indisponível quando não existe monitor de bateria instrumentado.

## DTCs

| Código | Significado |
|---|---|
| `0x0000` | sem DTC ativo |
| `0x1001` | comunicação I²C com MPU6050 |
| `0x1002` | barramento I²C bloqueado |
| `0x2001` | condição de gravidade/sensor |
| `0x2002` | clipping do sensor |
| `0x3001` | overrun do DSP |
| `0x4001` | baixa tensão |
| `0x4002` | falha de rede Wi-Fi |
| `0x4003` | datagrama com autenticação inválida |
| `0x4004` | versão de protocolo incompatível |
| `0x4005` | datagrama repetido (contador antigo) |
| `0x4006` | configuração inválida recusada |

A TUI deve exibir o código bruto quando receber um DTC ainda não catalogado.

## Fragmentação e integridade

Transferências maiores que um frame podem ser fragmentadas. A TUI mantém timeout, índice, contagem de fragmentos e CRC para remontagem. Fragmentos duplicados ou transferências incompletas são diagnosticados sem corromper o estado já aplicado.
