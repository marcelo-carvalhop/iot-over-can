# Edge Node TUI Ready Firmware

Firmware para Raspberry Pi Pico W com MPU6050, console serial ASCII, Wi-Fi/UDP local e protocolo preparado para integração com gateway ESP32/CAN e TUI.

Esta versão parte da variante `DRDY sem DMA` e adiciona campos estruturados para a TUI. O sensor é amostrado por `INT/DRDY` no GP2; a ISR apenas sinaliza amostra pendente, e a leitura I2C ocorre fora da interrupção.

## Principais melhorias desta versão

- Bateria implementada via MAX17048/MAX17043 em I2C1.
- `Payload_Beacon.battery_pct` deixou de ser fixo em 100.
- `DTC_SYS_LOW_VOLTAGE` agora é gerado quando a tensão de bateria fica baixa ou crítica.
- DSP passou a processar os três eixos por magnitude vetorial, em vez de usar somente Z.
- `window_size` passou a aceitar 128, 256 ou 512 amostras.
- O buffer do driver do MPU acompanha o `window_size` efetivo.
- Serial informa `FFT_VALID`, `WIN`, `AXIS`, `ACQ`, bateria, DTC ativo e contagem de DTCs. O vetor FFT pode ser solicitado com `FFT ONCE`.
- UDP de telemetria agora carrega as métricas que a serial já exibe: RMS, curtose, crest, pico, entropia, PPV, STA/LTA, clipping, bateria, modo de aquisição, janela efetiva, taxa solicitada e taxa efetiva.
- FFT só é enviada quando foi realmente calculada. Em modo sísmico, `fft_valid=0` e `fft_bins_count=0`.
- DTCs de warning também geram `DTC_EVENT` assíncrono na serial.
- Existe comando explícito de limpeza de DTC: `DTC CLEAR` na serial e `CMD_CLEAR_DTC` via UDP.
- O firmware mantém uma tabela de DTCs ativos, não apenas um código único sobrescrito.
- O DTC ativo exibido é um resumo priorizado da tabela de DTCs.
- Freeze frame usa temperatura do MPU, temperatura interna do RP e tensão real quando o fuel gauge está presente.
- ACK de configuração agora tem dois estágios: `status=0` para queued e `status=1` para applied.
- `STATUS` expõe `ACQ`, `DRDY_IRQ`, `DRDY_MISSED`, `BATT_PCT`, `BATT_MV`, `DTC_COUNT` e `WINDOW_SIZE`.
- IDLE pausa a aquisição do MPU.
- Comentários de saturação foram alinhados à faixa real do acelerômetro configurada: ±2 g.
- DTC de falha de TX UDP não é gerado enquanto o nó está apenas em DISCOVERY, para não poluir testes sem gateway.

## Pinagem

```text
MPU6050        Pico W
------------------------------
VCC        -> 3V3
GND        -> GND
SDA        -> GP0 / I2C0 SDA
SCL        -> GP1 / I2C0 SCL
INT        -> GP2 / DRDY
AD0        -> GND, endereço 0x68
```

Fuel gauge MAX17048/MAX17043:

```text
MAX17048       Pico W
------------------------------
VCC        -> 3V3
GND        -> GND
SDA        -> GP6 / I2C1 SDA
SCL        -> GP7 / I2C1 SCL
ALRT       -> GP8, opcional
```

Sem fuel gauge, o firmware continua funcionando. Nesse caso, a bateria aparece como desconhecida:

```text
BATT_PCT=255
BATT_MV=65535
```

## Console serial

Abrir terminal:

```bash
picocom -b 115200 /dev/ttyACM0
```

Comandos principais:

```text
HELP
STATUS
GET
SET MODE <IDLE|ROTATING|STRUCTURAL|SEISMIC|0-3>
SET RATE <4..1000>
SET WINDOW <RECT|HANN|HAMMING|FLATTOP|BLACKMAN|0-4>
SET WINDOW_SIZE <128|256|512>
SET STALTA <valor > 1.0>
SET GAIN <valor > 0.0>
APPLY
TELEMETRY ON|OFF|ONCE
TELEMETRY FAST|SLOW|PERIOD <ms>
DTC
DTC CLEAR
FFT ONCE
SIMULATE ON|OFF
PING
NET
VERSION
RESET
```

Atalhos durante telemetria contínua:

```text
!
Ctrl+C
```

Ambos desligam a telemetria sem precisar completar `TELEMETRY OFF`.

## Exemplo de teste

```text
STATUS
SET MODE STRUCTURAL
SET RATE 250
SET WINDOW_SIZE 256
APPLY
TELEMETRY ONCE
STATUS
```

Saída esperada de configuração:

```text
OK APPLY_QUEUED
CONFIG_APPLIED MODE=STRUCTURAL RATE_REQ=250.00 RATE_EFF=250.00 WINDOW_REQ=256 WINDOW_EFF=256 ACQ=DRDY
```

Saída esperada de telemetria:

```text
TEL MODE=STRUCTURAL ACQ=DRDY WIN=256 AXIS=VECTOR FFT_VALID=YES RMS=... KURT=... CREST=... PEAK_HZ=... PEAK_AMP=... ENT=... PPV_MM_S=... STA_LTA=NO CLIP=NO BATT_PCT=... BATT_MV=... DTC=0x0000 DTC_COUNT=0
```

## Protocolo UDP

`NET_PROTOCOL_VERSION` foi atualizado para `0x04` porque os payloads mudaram.

Mensagens relevantes para o gateway ESP32:

```text
CMD_BEACON_BROADCAST  0x01
CMD_CLAIM_NODE        0x10
CMD_ACK_CAPABILITIES  0x11
CMD_SET_CONFIG        0x12
CMD_ACK_CONFIG        0x13
CMD_PING              0x14
CMD_PONG              0x15
CMD_GET_CONFIG_MENU   0x16
CMD_CONFIG_MENU       0x17
CMD_CLEAR_DTC         0x18
CMD_DTC_SNAPSHOT      0x19
CMD_TELEMETRY_STREAM  0x20
CMD_URGENT_DTC_ALARM  0xFF
```

`CMD_ACK_CONFIG` usa `Payload_ConfigAck`:

```text
status=0 -> configuração recebida e enfileirada
status=1 -> configuração aplicada e verificada pelo loop principal
status=2 -> rejeitada
```

A TUI deve confiar no ACK aplicado, não apenas no ACK enfileirado.

## Recomendações para o gateway ESP32/CAN

O ESP32 deve atuar como ponte determinística entre UDP local do Pico W e a rede CAN do projeto distribuído. Recomendação de funções mínimas:

1. Criar Soft-AP local com SSID compatível com o firmware do Pico.
2. Receber `CMD_BEACON_BROADCAST` e manter uma tabela de sensores descobertos.
3. Enviar `CMD_CLAIM_NODE` com `protocol_ver=0x04`, `session_token` e `auth_key`.
4. Enviar `CMD_PING` a cada 3 s enquanto o sensor estiver vinculado.
5. Expor para a TUI os campos estruturados de `STATUS`, bateria, DTCs, modo de aquisição, taxa efetiva e janela efetiva.
6. Traduzir telemetria do sensor para mensagens CAN resumidas, sem tentar jogar FFT inteira em CAN clássico.
7. Transportar FFT por serial/UDP/TUI ou por CAN FD somente se disponível. Para CAN clássico, enviar apenas métricas resumidas e eventos.
8. Mapear `CMD_CLEAR_DTC` para um comando CAN de limpeza de DTC do nó sensor.
9. Encaminhar `DTC_EVENT` e `CMD_URGENT_DTC_ALARM` como eventos prioritários no CAN.
10. Separar frames CAN de controle, status, telemetria resumida e DTC.

Sugestão de divisão CAN:

```text
0x180 + node_id  Sensor status resumido
0x280 + node_id  Telemetria lenta: RMS, crest, pico Hz
0x300 + node_id  Diagnóstico/DTC
0x380 + node_id  Bateria e saúde do nó
0x400 + node_id  Controle/configuração vindo da TUI/gateway
```

Se o projeto continuar com CAN clássico, não envie vetor FFT completo no barramento. A FFT é grande demais para frames de 8 bytes e vai degradar a rede. Para a TUI, o gateway pode manter FFT via UDP local e enviar ao CAN apenas KPIs agregados.

## Build

```bash
rm -rf build
mkdir build
cd build
cmake -DPICO_BOARD=pico_w ..
cmake --build . -j$(nproc)
```

Gravação:

```bash
cp edge_node_firmware.uf2 /media/$USER/RPI-RP2/
```

## Observações

Esta versão ainda usa autenticação leve por chave compartilhada constante. Para uso real fora de bancada, substitua por desafio-resposta com HMAC, contador antirreplay e chave por nó.



### Correção de spam de DTC_EVENT 0x1002

Nesta revisão, `DTC_EVENT CODE=0x1002` não é mais reenviado indefinidamente quando o mesmo DTC já está ativo. O firmware usa política `event-on-change`: o evento assíncrono aparece quando o DTC surge, muda de severidade/sintoma ou reaparece após `DTC CLEAR`. Enquanto o DTC permanecer ativo, ele continua visível em `STATUS`, `TEL` e `DTC`, mas não inunda a porta serial.



### Recuperação do modo polling para DRDY

Se o firmware cair para `ACQ=POLLING`, agora é possível forçar o retorno ao caminho principal de aquisição:

```text
ACQ DRDY
```

ou:

```text
ACQ RESET
```

O firmware também tenta recuperar automaticamente o DRDY a cada 15 segundos enquanto estiver em `ACQ=POLLING`.

Critério esperado:

```text
STATUS
```

deve voltar a mostrar:

```text
ACQ=DRDY
DRDY_IRQ>0
DRDY_MISSED=0
```


### Correção de build

Esta revisão corrige a declaração global de `g_drdy_recovery_grace_until_ms` e o protótipo externo de `main_restart_drdy_acquisition()` usado pelo console serial.


### Política DRDY estável

Esta revisão torna o fallback para polling mais conservador. O firmware agora evita sair de `ACQ=DRDY` enquanto houver atividade recente no contador `DRDY_IRQ`. O DTC `0x1002` também passa a exigir falhas I2C consecutivas, evitando que uma falha transitória durante aquisição a 1000 Hz vire DTC crítico persistente.



### Baseline polling-only

Nesta versão, `ACQ=POLLING` é o modo oficial de aquisição. O INT/DRDY foi desativado para reduzir instabilidade de bancada e simplificar a integração com gateway/TUI.

Comportamento esperado:

```text
STATUS ... ACQ=POLLING ... DRDY_IRQ=0 DRDY_MISSED=0
TEL ... ACQ=POLLING ...
```

O comando `ACQ DRDY` é rejeitado. Use:

```text
ACQ POLLING
```

Polling é suficiente para fechar este ciclo do projeto. DRDY fica reservado para uma revisão futura, preferencialmente com PCB.



## Baseline segura — Wi-Fi sob decisão da TUI

Nesta versão, o rádio Wi-Fi/UDP não inicia automaticamente no boot. O sensor permanece em:

```text
NET=DISABLED
```

até que a TUI ou o operador envie explicitamente:

```text
NET WIFI ON
```

Comandos de rede serial:

```text
NET
NET WIFI STATUS
NET WIFI ON
NET WIFI OFF
```

Respostas esperadas:

```text
NET STATE=DISABLED WIFI=OFF
OK NET_WIFI=ON STATE=DISCOVERY
OK NET_WIFI=OFF STATE=DISABLED
```

Essa política preserva o caráter offline do sistema: conectar o nó wireless passa a ser uma ação explícita do operador, preferencialmente feita pela TUI após autorização.

## FFT serial sob demanda

`FFT ONCE` agora também força o processamento de um buffer mesmo quando a telemetria contínua está desligada. A TUI ainda pode enviar a sequência conservadora:

```text
FFT ONCE
TELEMETRY ONCE
```

mas a solicitação de FFT não depende mais exclusivamente de `TELEMETRY ON`.
