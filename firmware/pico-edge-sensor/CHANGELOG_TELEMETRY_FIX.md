# Correção de telemetria serial sem buffer DMA

Esta versão corrige o caso em que `STATUS` mostra `TELEMETRY=ON`, mas nenhuma linha `TEL ...` aparece.

## Causa

`TELEMETRY ON` apenas habilita a impressão. A telemetria só é emitida quando o `main.c` processa um buffer de 512 amostras.

No caminho normal, esse buffer vem de `mpu6050_get_ping_pong_buffer()`, que só retorna `true` quando `g_buffer_ready` foi marcado pelo fluxo INT/DRDY + DMA. Se o MPU6050 responde ao `WHO_AM_I`, mas o pino INT/DRDY não está ligado ao GP2 ou o fluxo DMA ainda não fechou o buffer, nenhum pacote de telemetria é produzido.

## Mudanças

- Adicionado `serial_console_wants_telemetry()`.
- Adicionado `mpu6050_get_polling_buffer()` como fallback de bancada.
- O `main.c` usa fallback por I2C polling quando:
  - o MPU está presente;
  - existe telemetria serial pendente ou gateway vinculado;
  - nenhum buffer DMA fica pronto por pelo menos 1,5 s.
- O firmware imprime:
  `WARN DMA_BUFFER_TIMEOUT USING_I2C_POLLING_FALLBACK`
  quando entra nesse caminho.

## Observação

O fallback por polling é para teste. Ele valida serial, DSP e telemetria mesmo que o INT/DMA não esteja pronto. Para a versão final, o ideal ainda é resolver o fluxo INT/DRDY + DMA.
