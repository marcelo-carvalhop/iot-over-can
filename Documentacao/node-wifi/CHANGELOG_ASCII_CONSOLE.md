# Correções da versão ASCII Console

Esta versão troca a interface serial JSON-line por um console ASCII simples.

## Correções principais

- `serial_console.c` agora aceita comandos terminados por `\n`, `\r` ou `\r\n`.
- O firmware faz eco local dos caracteres recebidos, facilitando uso com `picocom`.
- As respostas agora são texto simples: `OK`, `ERR`, `STATUS`, `TEL`, `DTC_EVENT`, `NET_EVENT`.
- O prompt `EDGE> ` é impresso após o boot e após cada comando.
- `CMakeLists.txt` define `PICO_BOARD=pico2_w` apenas quando o usuário não passa outro alvo no CMake.
- O README foi atualizado para documentar o console ASCII.

## Exemplos

```text
HELP
STATUS
SET MODE STRUCTURAL
SET RATE 250
SET WINDOW HANN
SET STALTA 4.0
SET GAIN 1.0
APPLY
TELEMETRY ONCE
SIMULATE ON
DTC
NET
VERSION
RESET
```

## Uso com picocom

```bash
picocom -b 115200 /dev/ttyACM0
```

Não é obrigatório usar `--echo`, porque o firmware já ecoa os caracteres.
