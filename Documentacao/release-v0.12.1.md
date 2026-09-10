# Release v0.12.1 — reorganização acadêmica do repositório

Esta release não altera o protocolo operacional da baseline v0.12.0. O objetivo
é adequar a estrutura física do repositório ao padrão da disciplina de IoT II.

Alterações principais:

- TUI migrada para `Front/`;
- firmware ESP32/CAN migrado para `Codigo/node-can/`;
- firmware Raspberry Pi Pico 2 W migrado para `Codigo/node-wifi/`;
- implementação ESP32 histórica preservada em `Codigo/node-can/legacy/`;
- documentação centralizada em `Documentacao/`;
- scripts de build, execução e testes atualizados;
- workflows de CI atualizados;
- testes que calculavam a raiz do repositório ajustados à nova profundidade;
- árvore e manifesto atualizados.

A Probe 00 continua sendo exclusivamente uma interface de instrumentação. A
reorganização não modifica a arquitetura distribuída definida para os módulos CAN
e sensores wireless.
