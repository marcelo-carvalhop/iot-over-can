# Correção: watchdog durante fallback I2C polling

Sintoma observado:

```text
WARN DMA_BUFFER_TIMEOUT ENTERING_STABLE_I2C_POLLING_MODE
FATAL: read zero bytes from port
```

Interpretação: em taxas baixas, a coleta polling de 512 amostras pode durar mais
que o timeout do watchdog. Exemplo: 100 Hz -> aproximadamente 5,12 s. Como o
loop principal não volta durante essa coleta, o watchdog reinicia o RP e o USB
CDC desaparece, fazendo o `picocom` encerrar com `read zero bytes from port`.

Mudanças:

- `WATCHDOG_TIMEOUT_MS` passou de 3000 ms para 8000 ms.
- `mpu6050_get_polling_buffer()` agora chama `watchdog_update()` dentro do loop
  de aquisição.
- `mpu_reset_i2c0_peripheral()` também alimenta o watchdog antes e depois da
  tentativa de liberação do barramento.

Isso não prova que o sensor está bom nem ruim. A correção remove uma causa de
reset falsa durante teste de bancada.
