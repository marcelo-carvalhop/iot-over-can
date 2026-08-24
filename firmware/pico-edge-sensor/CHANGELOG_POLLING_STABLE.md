# Correção: polling estável e antispam de DTC

Esta versão corrige o comportamento observado em bancada:

```text
WARN DMA_BUFFER_TIMEOUT USING_I2C_POLLING_FALLBACK
DTC_EVENT CODE=0x1002 ...
DTC_EVENT CODE=0x1002 ...
...
```

## Mudanças

- `DTC_I2C_BUS_STUCK` agora é limitado por cooldown para não inundar a serial.
- `diag_report_dtc_event()` suprime DTC crítico idêntico por 2 segundos.
- Quando o fallback entra, o driver passa para `mpu6050_enter_polling_mode()`.
- O modo polling desabilita IRQ/DRDY, aborta DMA, limpa ping-pong parcial e reinicializa o I2C0 em 400 kHz.
- O modo polling tenta liberar o barramento com 9 pulsos de SCL e uma condição STOP manual.
- Se o polling falhar, a serial imprime um erro legível e limitado:

```text
ERR I2C_POLLING_READ_FAILED CHECK_SDA_SCL_PULLUPS_AND_MPU_POWER
```

## Interpretação

Se o erro persistir, o problema não é o menu serial. É o barramento I2C ou a ligação elétrica do MPU6050.
