# Firmware

## `pico-edge-sensor`

Firmware do sensor de vibração baseado em Raspberry Pi Pico 2 W + MPU6050.

Baseline atual:

```text
ACQ=POLLING
DRDY desativado
Serial ASCII
Wi-Fi desabilitado no boot
```

## `esp32-can-legacy`

Código CAN/ESP32 legado preservado como base de estudo e evolução. Ainda não representa o gateway CAN FD final com MCP2518FD.


## Política enxuta

A baseline atual remove modos de simulação de falha e geração sintética de eventos. O firmware deve produzir somente eventos reais ou diagnósticos derivados do hardware, da aquisição e da comunicação. O antigo heartbeat acadêmico deve ser entendido como liveness/status, não como sincronização rígida.

## `esp32-can-node-platformio`

Versão PlatformIO do firmware dos nós CAN ESP32 + MCP2515. Esta passa a ser a pasta recomendada para evolução do firmware CAN legado.
