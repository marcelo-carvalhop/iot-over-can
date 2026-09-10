# Protocolo Textual de Referência da Probe 00 e dos módulos CAN

## Finalidade

Este protocolo permite desenvolver e testar a TUI antes da definição do enquadramento binário definitivo entre ESP32 e computador. Cada mensagem ocupa uma linha ASCII terminada por `LF` ou `CRLF`.

O parser aceita nomes em maiúsculas ou minúsculas, mas a emissão recomendada utiliza maiúsculas.

## Regras gerais

```text
PREFIX KEY=VALUE KEY=VALUE ...
```

- espaços separam campos;
- números podem ser decimais ou hexadecimais com `0x`;
- booleanos aceitos incluem `YES/NO`, `ON/OFF`, `TRUE/FALSE` e `1/0`;
- `DATA` pode conter bytes hexadecimais separados por espaço;
- campos desconhecidos são preservados quando possível;
- campos ausentes não devem ser substituídos por medições inventadas.

## Identificação da Probe 00

```text
GW_VERSION FIRMWARE=0.5.0 PROTOCOL=1.0 ROLE=PROBE
```

```text
GW_STATUS NODE=0 UPTIME_MS=123456 CAN=ONLINE WIFI=ONLINE \
ARB=500000 DATA=2000000 BUS_OFF=NO ERROR_PASSIVE=NO \
ERROR_WARNING=NO UTILIZATION=18.4
```

## Nó físico

```text
NODE NODE=20 TYPE=CAN_NODE ROLE=FOLLOWER STATE=ONLINE FIRMWARE=0.3.0 \
PROTOCOL=1.0 CAN=ONLINE WIFI=OFF CAPS=CAN,LOCAL_SENSOR,BLE_SCAN,WIFI_AP UPTIME_MS=55120
```

## Sensor wireless

```text
SENSOR NODE=20 CHILD=1 UUID=0xA1B2C3D4E5F60718 PROFILE=VIBRATION \
STATE=ONLINE MODE=ROTATING ACQ=POLLING QUALITY=REAL NET=BOUND
```

## Telemetria

Versão escalonada recomendada:

```text
TEL NODE=20 CHILD=1 SEQ=7 SENSOR_TS_MS=10120 GW_TS_MS=10126 \
MODE=ROTATING ACQ=POLLING RATE_REQ_HZ=333 RATE_EFF_HZ=333.33 \
WINDOW=HANN WINDOW_SIZE=512 RMS_MG=125 KURT_X100=25 \
CREST_X100=310 PEAK_HZ_X10=500 PEAK_AMP=0.019531 \
ENTROPY_X1000=700 PPV_UM_S=450 STA_LTA=NO CLIP=NO \
DTC=0x0000 QUALITY=REAL BATT_PRESENT=NO
```

Conversões efetuadas pela TUI:

```text
RMS_MG / 1000       → g
KURT_X100 / 100     → curtose
CREST_X100 / 100    → fator de crista
PEAK_HZ_X10 / 10    → Hz
ENTROPY_X1000 / 1000
PPV_UM_S / 1000     → mm/s
BATT_MV / 1000      → V
```

## DTC

```text
DTC NODE=20 CHILD=1 CODE=0x2001 SEVERITY=WARNING SYMPTOM=0x17 \
TS_MS=10120 MODE=ROTATING ACQ=POLLING SEQ=7
```

Confirmação de limpeza:

```text
DTC_CLEAR NODE=20 CHILD=1 CODE=0x2001 STATE=VERIFIED TX=A1B2C3
```

Para todos:

```text
DTC_CLEAR NODE=20 CHILD=1 SCOPE=ALL STATE=VERIFIED TX=A1B2C3
```

## ACK de comando direcionado ao módulo CAN

Ações administrativas direcionadas ao sensor local do módulo produzem confirmação observável pela Probe 00:

```text
[GW] CMD_ACK node=4 subcmd=0x10 action=0x11 result=APPLIED
```

`REJECTED` indica que o nó recebeu a ação, mas não a aplicou. O ACK confirma execução; não autentica a origem do frame CAN.

## ACK

```text
ACK TARGET=20.01 ACTION=CONFIG TX=A1B2C3 STATE=QUEUED
ACK TARGET=20.01 ACTION=CONFIG TX=A1B2C3 STATE=APPLIED
ACK TARGET=20.01 ACTION=CONFIG TX=A1B2C3 STATE=VERIFIED
```

Estados esperados:

```text
SENT
QUEUED
APPLIED
VERIFIED
REJECTED
FAILED
TIMEOUT
APPROXIMATED
```

## Frame CAN FD bruto

```text
CAN_RX TS_MS=10126 ID=0x0C050142 FD=YES BRS=YES ESI=NO \
DATA=01 02 A0 FF
```

```text
CAN_TX TS_MS=10130 ID=0x0C050142 FD=YES BRS=YES ESI=NO \
DATA=10 20 30 40
```

## CRC

```text
CRC_ERROR NODE=20 CHILD=1 TYPE=TELEMETRY EXPECTED=0xA1B2 ACTUAL=0xC3D4
```

## Fragmentação

```text
FRAG NODE=20 CHILD=1 TYPE=FFT TRANSFER=0x42A1 INDEX=0 COUNT=3 \
CRC32=0x89ABCDEF RATE_HZ=1000 FFT_SIZE=128 WINDOW=HANN FORMAT=U16_LE \
DATA=01 00 02 00 03 00
```

Os fragmentos seguintes podem omitir metadados repetidos:

```text
FRAG NODE=20 CHILD=1 TYPE=FFT TRANSFER=0x42A1 INDEX=1 COUNT=3 \
CRC32=0x89ABCDEF DATA=04 00 05 00 06 00
```

Campos obrigatórios:

```text
NODE
CHILD
TYPE
TRANSFER
INDEX
COUNT
DATA
```

Campos recomendados:

```text
CRC32
RATE_HZ
FFT_SIZE
WINDOW
FORMAT
```

## Comandos da TUI para o gateway

Formato geral:

```text
CMD TARGET=<PP.CC> ACTION=<AÇÃO> TX=<ID> KEY=VALUE ...
```

Exemplos:

```text
CMD TARGET=20.01 ACTION=STATUS TX=A1B2C3
CMD TARGET=20.01 ACTION=TELEMETRY TX=A1B2C4 STATE=ON
CMD TARGET=20.01 ACTION=FFT TX=A1B2C5 MODE=VIEW_ONLY BINS=64
CMD TARGET=20.01 ACTION=DTC_LIST TX=A1B2C6
CMD TARGET=20.01 ACTION=DTC_CLEAR TX=A1B2C7 SCOPE=ALL
CMD TARGET=20.01 ACTION=CONFIG TX=A1B2C8 MODE=ROTATING RATE_HZ=250 \
WINDOW=HANN WINDOW_SIZE=512 STALTA=5.0 GAIN=1.0 VERIFY=YES
CMD TARGET=20.01 ACTION=SIMULATE TX=A1B2C9 STATE=ON
```

## Compatibilidade legada

A TUI também reconhece mensagens da fase 1, por exemplo:

```text
[GW] HEARTBEAT (legacy liveness) lider=4 rodada=18 sensores_ativos=3 modo=2 periodo=1000 ms
[GW] SENSOR sensor=3 rodada=18 valor=0xAA ativo=1
[STATUS] NODE 3 estado=ATIVO
[NODE 3] [SENSOR TX] sensor=3 rodada=18 valor=0xAA
[120 ms] RX ID=0x080 DLC=4 DATA=23 21 03 01
```

Essa compatibilidade é destinada à bancada e não substitui o protocolo CAN FD definitivo.


## Compatibilidade com a baseline serial 0.6

O gateway deverá preservar os campos `ACQ`, `FFT_VALID`, `AXIS`, `DTC_COUNT`, `BATT_PCT` e `BATT_MV`. `POLLING` é estado saudável. Valores de bateria 255 e 65535 representam ausência de instrumentação.


## Extensão de controle Wi-Fi do sensor

A TUI passa a tratar conexão Wi-Fi do sensor como ação explícita do operador.

Formato textual recomendado para gateway:

```text
CMD TARGET=20.01 ACTION=WIFI STATE=ON TX=ABC123
CMD TARGET=20.01 ACTION=WIFI STATE=OFF TX=ABC124
CMD TARGET=20.01 ACTION=WIFI_STATUS TX=ABC125
```

O gateway deve mapear `ACTION=WIFI STATE=ON` para o comando local do sensor ou para a rotina equivalente de vínculo wireless. Em modo direto, a TUI envia:

```text
NET WIFI ON
NET WIFI OFF
NET WIFI STATUS
```
