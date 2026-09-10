# Capacidades dos módulos CAN e UI orientada a perfis

A função distribuída de um módulo (`LEADER`, `FOLLOWER`) é separada das capacidades físicas do módulo.

Um módulo CAN pode declarar capacidades como:

```text
CAN
LOCAL_SENSOR
LOCAL_SENSOR_DEMO
WIFI_AP          (futuro)
BLE_SCAN         (futuro)
WIRELESS_BRIDGE  (futuro)
```

O valor `0xAA` da baseline é tratado explicitamente como telemetria do sensor local de demonstração:

```text
LOCAL_PROFILE=DEMO_BYTE
LOCAL_SENSOR_DEMO_VALUE=0xAA
```

Ele pertence ao módulo CAN físico e não cria um `child_id` wireless.

## Hierarquia da TUI

```text
Rede CAN
├── Node 01
│   ├── sensor local (capacidade do próprio módulo)
│   └── sensores wireless associados
├── Node 02
└── Node 03

Instrumentação
└── Probe 00
```

O Probe 00 não pertence à topologia funcional.

## UI por perfil

Sensores wireless possuem `PROFILE_ID`. Telas especializadas só aparecem quando o perfil correspondente está selecionado.

A baseline possui:

```text
PROFILE=VIBRATION
  -> Telemetria de vibração
  -> FFT
  -> DTC
  -> configuração de aquisição
```

Sem um sensor `PROFILE=VIBRATION` selecionado, os painéis de vibração permanecem ocultos.

Perfis futuros poderão registrar interfaces próprias sem alterar a tela principal da rede.
