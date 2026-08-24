# Wi-Fi sob autorização da TUI

## Decisão

O Pico 2 W não conecta mais automaticamente ao gateway Wi-Fi no boot.

## Novo comportamento

Estado inicial:

```text
NET=DISABLED
```

A conexão só é iniciada por comando serial explícito:

```text
NET WIFI ON
```

Para desligar:

```text
NET WIFI OFF
```

Para consultar:

```text
NET WIFI STATUS
```

## Motivação

Esse comportamento reforça o desenho de sistema desconectado: o sensor só passa a operar como nó wireless quando o operador escolhe essa ação na TUI.

## FFT sob demanda

`FFT ONCE` passa a entrar na condição de processamento de buffer, evitando depender apenas de `TELEMETRY ONCE`.
