# ADR-001 — Firmware enxuto e liveness lease

## Decisão

A baseline do projeto remove os modos de falha programada e o modo sintético de bancada do firmware principal. O projeto mantém apenas falhas reais derivadas de aquisição, saturação, comunicação, energia e diagnóstico operacional.

Também foi decidido que o antigo conceito de `heartbeat`, herdado do trabalho de Sistemas Distribuídos, não deve ser tratado como sincronização lógica pesada, eleição acadêmica ou mecanismo obrigatório de TDMA. Na baseline do **iot-over-can**, o conceito passa a ser chamado de **liveness lease**.

## Motivo

O objetivo atual é fechar uma base realista para IoT embarcado sobre CAN/CAN FD. Eventos programados de erro são úteis para demonstração, mas aumentam comandos, estados e código, além de confundirem a TUI ao misturar eventos reais com eventos artificiais.

O liveness lease permanece porque resolve um problema real de rede: distinguir um nó silencioso, desconectado, travado ou deliberadamente ocioso de um nó saudável. CAN/CAN FD não fornece, por si só, uma semântica de presença de aplicação por nó. A camada de aplicação precisa declarar presença quando a telemetria está desligada, pausada ou sob demanda.

## Política

- Remover `SIMULATE ON/OFF` da baseline.
- Remover comandos de falha programada do firmware legado.
- Manter DTCs reais.
- Manter `PING/PONG` como mecanismo leve de liveness.
- Não usar liveness como sincronização determinística rígida.
- Telemetria periódica pode servir como heartbeat implícito.
- Quando a telemetria estiver desligada, `PING/PONG` ou frame de status periódico deve manter a sessão viva.
- Gateway deve marcar nó como `STALE` ou `LOST` apenas após múltiplas perdas consecutivas.

## Recomendação inicial

```text
Liveness interval: 3 s
Timeout de sessão: 10 s
Política de perda: 3 ausências consecutivas antes de LOST
```

## Consequência

O sistema mantém utilidade real de supervisão e segurança sem carregar a lógica acadêmica completa de heartbeat, eleição e sincronização temporal.
