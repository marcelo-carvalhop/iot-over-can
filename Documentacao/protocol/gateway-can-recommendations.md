# Recomendações de integração ESP32 Gateway / CAN / TUI

## Papel do gateway

O ESP32 deve funcionar como gateway entre três domínios:

1. Pico 2 W via Wi-Fi/UDP local.
2. Rede CAN do projeto de Computação Distribuída.
3. TUI no computador, via serial, UDP ou ponte CAN-serial.

O gateway não deve tentar transformar todo pacote UDP em frame CAN bruto. Ele deve interpretar o protocolo do sensor, manter estado local e publicar no CAN apenas mensagens compactas e úteis.

## Estados mínimos por sensor

A tabela interna do gateway deve guardar:

```text
node_uuid
profile_id
session_token
net_state
last_seen_ms
protocol_ver
fsm_mode
acquisition_mode
sample_rate_req_hz
sample_rate_eff_hz
window_size
battery_pct
battery_mv
dtc_active_code
dtc_count
last_rms
last_kurtosis
last_crest
last_peak_hz
last_ppv
fft_valid
```

## Integração com CAN distribuído existente

Use a base conceitual do projeto anterior:

- Gateway CAN continua sendo nó 0.
- Sensor wireless não participa diretamente da eleição CAN.
- O ESP32 representa o sensor wireless na rede CAN como nó periférico virtual.
- Liveness/status CAN do gateway deve indicar se o sensor wireless está online, degradado ou ausente.
- A TUI deve receber atualização de estado apenas após confirmação da rede/gateway, não quando o comando é apenas digitado.

## Mensagens CAN sugeridas

```text
0x180 + node_id  STATUS
Byte 0: fsm_mode
Byte 1: acquisition_mode
Byte 2: flags: bit0=mpu_ok, bit1=batt_present, bit2=fft_valid, bit3=dtc_active
Byte 3: battery_pct
Byte 4-5: battery_mv uint16
Byte 6: dtc_count
Byte 7: protocol_ver
```

```text
0x280 + node_id  TELEMETRY_A
Byte 0-1: rms_ac escalado
Byte 2-3: crest_factor escalado
Byte 4-5: peak_freq_hz escalado
Byte 6-7: ppv_mm_s escalado
```

```text
0x300 + node_id  DTC
Byte 0-1: dtc_active_code
Byte 2: severity
Byte 3: symptom
Byte 4-7: timestamp reduzido ou contador
```

```text
0x400 + node_id  CONFIG_COMMAND
Byte 0: target_mode
Byte 1: window_type
Byte 2: window_size_code: 0=128, 1=256, 2=512
Byte 3-4: sample_rate_hz uint16
Byte 5: command_flags, bit0=apply, bit1=clear_dtc
Byte 6-7: sequence_id
```

## Tratamento de configuração

Fluxo recomendado:

```text
TUI -> CAN CONFIG_COMMAND -> ESP32
ESP32 -> UDP CMD_SET_CONFIG -> Pico
Pico -> UDP CMD_ACK_CONFIG status=0 queued
Pico -> UDP CMD_ACK_CONFIG status=1 applied
ESP32 -> CAN CONFIG_STATUS -> TUI
```

A TUI só deve marcar configuração como aplicada depois de receber o ACK `status=1`.

## FFT

Não transportar FFT completa em CAN clássico. Opções:

1. TUI recebe FFT diretamente do gateway por UDP/serial.
2. Gateway armazena FFT e envia ao CAN apenas pico de frequência, amplitude e entropia.
3. Usar CAN FD se a disciplina ou hardware permitir.

## Robustez

- Usar contador de sequência em telemetria UDP e em comandos CAN.
- Implementar timeout por sensor wireless.
- Implementar replay protection simples para comandos críticos.
- Separar erros de rede Wi-Fi de erros reais do sensor.
- Permitir `DTC CLEAR` remoto, mas registrar o evento de limpeza.
- Não apagar DTC automaticamente quando o valor volta ao normal, exceto se houver política explícita.
