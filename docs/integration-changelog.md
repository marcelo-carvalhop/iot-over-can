# Integração segura — revisão 1

## Bases integradas

Esta revisão organiza o projeto em três blocos:

```text
sensor_pico_polling_secure/   firmware Pico 2 W + MPU6050
tui_secure/                   TUI Textual com proteção operacional
esp32_can_legacy_base/        base CAN distribuída anterior
```

## Decisão principal

O firmware do Pico permanece em `ACQ=POLLING` como baseline oficial. O DRDY continua fora do ciclo atual.

## Mudanças funcionais no firmware do Pico

- Wi-Fi não conecta automaticamente no boot.
- Estado inicial da rede: `NET=DISABLED`.
- Comandos novos:

```text
NET WIFI STATUS
NET WIFI ON
NET WIFI OFF
```

- `NET WIFI ON` inicia a pilha CYW43, conecta ao Soft-AP esperado e entra em `DISCOVERY`.
- `NET WIFI OFF` remove o PCB UDP, limpa sessão e volta para `DISABLED`.
- `FFT ONCE` agora entra na condição de processamento de buffer, mesmo sem telemetria contínua.

## Mudanças funcionais na TUI

- Adicionada camada `SecurityManager`.
- `--security-mode presence` exige YubiKey USB presente para comandos mutáveis.
- `--security-mode otp` permite desbloqueio temporário com `:unlock <otp>`.
- `--security-mode off` desativa a proteção operacional.
- Comandos internos novos:

```text
:wifi on
:wifi off
:wifi status
:security
:unlock <otp>
:lock
```

- Atalhos novos:

```text
Ctrl+W  Wi-Fi ON
Ctrl+Y  estado da segurança
```

## Comandos protegidos

A camada de segurança bloqueia comandos mutáveis, incluindo:

```text
SET ...
APPLY
TELEMETRY ...
FFT ...
SIMULATE ...
ACQ ...
DTC CLEAR
RESET
NET WIFI ON
NET WIFI OFF
CMD ...
```

Comandos de leitura, como `STATUS`, `GET`, `VERSION`, `PING`, `NET`, `NET WIFI STATUS` e `DTC`, permanecem permitidos.

## Limite assumido

A base `esp32_can_legacy_base` ainda é a rede CAN clássica/legada. Ela foi preservada para integração posterior ao gateway real com MCP2518FD/CAN FD. Esta revisão fecha principalmente a integração operacional entre TUI e Pico.
