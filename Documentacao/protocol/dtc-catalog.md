# Catálogo de DTCs

A TUI não deve exibir mensagens genéricas como “falha desconhecida” para códigos emitidos pelo firmware. Todo DTC definido no protocolo deve ter uma descrição objetiva, orientada ao problema observado.

## Códigos atuais

| Código | Nome lógico | Severidade | Descrição operacional |
|---:|---|---|---|
| `0x0000` | `DTC_NONE` | Info | Sem DTC ativo. |
| `0x1001` | `DTC_I2C_MPU_COMM` | Crítico | MPU6050 sem comunicação no I2C0 ou `WHO_AM_I` inválido. |
| `0x1002` | `DTC_I2C_BUS_STUCK` | Crítico | Barramento I2C0 travado ou timeout persistente de leitura. |
| `0x2001` | `DTC_SENS_GRAVITY` | Crítico | Vetor de gravidade fora da faixa esperada; possível soltura, impacto ou montagem instável. |
| `0x2002` | `DTC_SENS_CLIPPING` | Warning | Saturação do acelerômetro ou clipping mecânico no MPU6050. |
| `0x3001` | `DTC_DSP_OVERRUN` | Warning | Sobrecarga no processamento DSP ou janela não processada no tempo esperado. |
| `0x4001` | `DTC_SYS_LOW_VOLTAGE` | Crítico | Tensão de alimentação baixa ou crítica no nó sensor. |
| `0x4002` | `DTC_SYS_NET_TX_FAIL` | Warning | Falha repetida de transmissão UDP após vínculo com gateway. |
| `0x4003` | `DTC_SYS_AUTH_REJECT` | Warning | Vínculo rejeitado por credencial ou chave de autenticação. |
| `0x4004` | `DTC_SYS_PROTOCOL_MISMATCH` | Warning | Versão de protocolo incompatível durante o vínculo. |
| `0x4005` | `DTC_SYS_REPLAY_REJECT` | Warning | Comando rejeitado por contador repetido ou antigo. |
| `0x4006` | `DTC_SYS_CONFIG_REJECT` | Warning | Configuração remota inválida ou fora de faixa. |

## Política de fallback

Se um código novo aparecer antes de a TUI ser atualizada, ela deve usar a categoria do nibble alto e exibir o código bruto. O objetivo é evitar mensagens vagas.

Exemplos:

```text
0x1007 → Falha de barramento ou aquisição ainda não detalhada — código bruto 0x1007
0x2009 → Falha física ou mecânica do sensor ainda não detalhada — código bruto 0x2009
0x3002 → Falha de processamento DSP ainda não detalhada — código bruto 0x3002
0x4008 → Falha de sistema, energia ou rede ainda não detalhada — código bruto 0x4008
```

A TUI deve sempre mostrar o código hexadecimal junto da descrição.
