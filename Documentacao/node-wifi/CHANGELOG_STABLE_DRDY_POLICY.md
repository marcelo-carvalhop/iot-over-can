# Política DRDY estável

## Problema observado em bancada

A versão anterior conseguia usar `ACQ=DRDY`, mas ainda caía para `ACQ=POLLING` de forma agressiva quando uma solicitação `TELEMETRY ONCE` acontecia antes de fechar um buffer completo, ou quando ocorria uma falha transitória de I2C durante aquisição a 1000 Hz.

Também foi observado `DTC_EVENT CODE=0x1002` enquanto a aquisição DRDY continuava funcionando, indicando que uma falha isolada estava sendo tratada como falha crítica persistente.

## Alterações

- O fallback para polling foi tornado conservador.
- O timeout DRDY agora considera uma janela maior: `expected_buffer_ms * 8 + 3000`, limitado entre 5 s e 30 s.
- O firmware monitora atividade real do contador `DRDY_IRQ`.
- Enquanto houver atividade recente de DRDY, o firmware evita trocar automaticamente para polling.
- Falhas I2C isoladas não geram mais `DTC_I2C_BUS_STUCK`.
- `DTC_I2C_BUS_STUCK` passa a exigir falhas I2C consecutivas antes de ser reportado.

## Resultado esperado

Em bancada, o modo principal deve permanecer:

```text
ACQ=DRDY
DRDY_IRQ subindo
DRDY_MISSED=0
DTC=0x0000
```

Polling continua existindo como fallback, mas não deve ser usado em resposta a atrasos pequenos ou falhas transitórias.
