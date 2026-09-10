# TUI como console de rede CAN

A TUI não deve ser centrada no sensor de vibração. A tela principal passa a ser um console genérico da rede CAN/CAN FD.

## Hierarquia operacional

```text
Rede CAN/CAN FD
├── Gateway
├── Node 01
├── Node 02
│   └── Sensor lógico/wireless 02.01
└── Node 03
```

## Regras de interface

- A tela principal mostra estado da rede, módulos CAN, tráfego, erros, gateway e sensores detectados.
- Selecionar um módulo CAN físico deve habilitar comandos/configurações do módulo.
- Selecionar um sensor lógico deve abrir a visão especializada daquele sensor.
- A tela de vibração é contextual, não é a tela principal.
- A TUI deve permitir envio de comandos CAN por alvo, sem obrigar o operador a digitar tudo manualmente.

## Comandos internos

```text
:node 04
:node 04.01
:can
:can 22 20 04 00
```

`:can` sem argumentos abre o modal do módulo CAN selecionado. `:can` com argumentos envia o comando informado ao gateway serial.

## Ações rápidas do módulo CAN

A tela de configuração de módulo oferece inicialmente:

```text
22 20 ID 00    solicitar status do nó
22 10 ID 00    desativar função do nó
22 10 ID 11    reativar função do nó
22 10 ID 44    limpar falha do nó
22 00 FF 01    iniciar eleição global
22 20 FF 00    solicitar status global
22 30 FF 01..05 ajustar liveness/heartbeat global
```

Essa interface ainda depende do gateway/nó serial aceitar o protocolo textual legado `22 ...`.

## Comandos globais independentes de seleção

Eleição, consulta global de status e ajustes globais de liveness pertencem à rede e devem permanecer acessíveis mesmo quando nenhum módulo CAN estiver selecionado.

A TUI oferece `Ctrl+L`, `:can`, `:election` e `:canstatus`.
