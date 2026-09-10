# Correção: spam de DTC_EVENT 0x1002

## Sintoma

A versão `edge_node_tui_ready_robust` podia imprimir continuamente:

```text
DTC_EVENT CODE=0x1002 SYMPTOM=0x11 SEVERITY=2 ...
```

a cada ~2 segundos.

## Causa

`0x1002` é `DTC_I2C_BUS_STUCK`. O DTC estava sendo armazenado como ativo, mas também era reenfileirado como evento assíncrono sempre que o mesmo erro reaparecia depois do cooldown. Isso tornava a serial pouco usável.

Além disso, a leitura auxiliar de temperatura do MPU para freeze frame/STATUS usava a mesma rotina I2C que reportava DTC crítico. Uma falha nessa leitura auxiliar podia gerar evento crítico mesmo sem falha real da aquisição.

## Correção

- `diag_report_dtc_event()` agora trabalha no modelo **event-on-change**.
- Se o mesmo DTC já está ativo com mesmo sintoma e mesma severidade, a tabela interna é atualizada, mas a serial não recebe novo `DTC_EVENT`.
- DTC novo, DTC com nova severidade ou DTC após `DTC CLEAR` continua gerando evento.
- A leitura de temperatura do MPU passou a ser silenciosa. Se ela falhar, não gera `DTC_I2C_BUS_STUCK`.
- O cooldown interno do erro I2C foi aumentado de 2 s para 10 s.

## Comportamento esperado

O DTC ativo continua visível em:

```text
STATUS
TEL
DTC
```

mas não deve mais ficar inundando a serial com `DTC_EVENT` repetido.
