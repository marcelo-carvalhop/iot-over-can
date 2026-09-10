# Baseline serial do sensor — versão 0.6

## Aquisição

`POLLING` é o modo nominal e saudável. A TUI oferece `ACQ POLLING` para reiniciar a aquisição sem reiniciar a placa.

Resposta:

```text
OK ACQ=POLLING_RESTARTED
```

`ACQ DRDY` não é oferecido. A resposta abaixo é classificada como rejeição de comando, não como falha do sensor:

```text
ERR DRDY disabled in polling baseline. Use ACQ POLLING.
```

Estados reconhecidos: `UNKNOWN`, `POLLING`, `SIM`, `IDLE` e `DRDY` legado.

## STATUS

```text
STATUS NET=DISCOVERY MODE=STRUCTURAL ACQ=POLLING WINDOW=HANN WINDOW_SIZE=512 RATE_HZ=1000.00 STALTA=4.000 GAIN=1.000 DTC=0x0000 DTC_COUNT=0 MPU=YES SIM=NO TELEMETRY=OFF PERIOD_MS=1000 BATT_PCT=255 BATT_MV=65535 DRDY_IRQ=0 DRDY_MISSED=0
```

Em `POLLING`, os campos DRDY são apenas informativos e não geram alerta.

## TEL

```text
TEL MODE=STRUCTURAL ACQ=POLLING WIN=512 AXIS=VECTOR FFT_VALID=YES RMS=0.07136 KURT=2.19474 CREST=3.9580 PEAK_HZ=17.578 PEAK_AMP=0.011725 ENT=0.9221 PPV_MM_S=1.6673 STA_LTA=NO CLIP=NO BATT_PCT=255 BATT_MV=65535 DTC=0x0000 DTC_COUNT=0
```

Quando `FFT_VALID=NO`, `PEAK_HZ`, `PEAK_AMP` e `ENT` são ocultados e nenhum gráfico é apresentado.

## FFT sob demanda

Sequência enviada pela TUI:

```text
FFT ONCE
TELEMETRY ONCE
```

Resposta de vetor:

```text
FFT VALID=YES BINS=64 VALUES=...
```

No modo `SEISMIC`, a TUI não solicita FFT.

## Configuração

```text
SET MODE STRUCTURAL
OK STAGED MODE=STRUCTURAL
APPLY
OK APPLY_QUEUED
CONFIG_APPLIED MODE=STRUCTURAL RATE_REQ=1000.00 RATE_EFF=1000.00 WINDOW_REQ=512 WINDOW_EFF=512 ACQ=POLLING
```

Somente `CONFIG_APPLIED` altera a configuração principal mostrada na interface.

## Bateria

```text
BATT_PCT=255
BATT_MV=65535
```

Esses valores significam bateria não instrumentada e são exibidos como `N/A`.

## DTC

Comandos:

```text
DTC
DTC CLEAR
```

Catálogo mínimo:

- `0x0000`: sem DTC ativo;
- `0x1002`: falha persistente do barramento I²C;
- `0x2002`: clipping ou saturação do acelerômetro;
- `0x4002`: falha de transmissão ou rede UDP.

## Apresentação na TUI 0.7

- `F6` abre a telemetria detalhada e não altera o streaming implicitamente.
- `F7` envia a solicitação e abre uma janela que aguarda `FFT VALID=YES BINS=... VALUES=...`.
- A frequência do bin é calculada por `bin_index × sample_rate_hz / fft_size`.
- Os valores são chamados de magnitude, não de energia, até que o firmware defina normalização e unidade.



## Segurança e Wi-Fi 0.9

Comandos novos de rede direta:

```text
NET WIFI STATUS
NET WIFI ON
NET WIFI OFF
```

Estado inicial esperado do sensor:

```text
NET=DISABLED
```

A TUI só envia `NET WIFI ON` por decisão explícita do operador. Esse comando é protegido pela camada de segurança local da TUI.

Comandos internos da TUI:

```text
:wifi on
:wifi off
:wifi status
:security
:unlock <otp>
:lock
```

## Catálogo de DTCs

A TUI deve usar `Documentacao/protocol/dtc-catalog.md` como referência. Códigos emitidos pelo firmware não devem aparecer como “falha desconhecida”. Para códigos novos, usar fallback por categoria e exibir o hexadecimal bruto.
