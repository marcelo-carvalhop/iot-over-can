# ESP32 CAN Node — PlatformIO

Esta pasta contém a versão PlatformIO do firmware legado dos nós CAN baseados em ESP32 + MCP2515.

O objetivo é profissionalizar o fluxo de build sem apagar a base anterior em `Codigo/node-can/legacy/`. A pasta legada continua preservada como referência histórica; esta pasta passa a ser o ponto recomendado para evolução do firmware dos nós CAN.

## Build

```bash
cd Codigo/node-can
pio run
```

## Upload

```bash
pio run -t upload
```

## Monitor serial

```bash
pio device monitor -b 115200
```

## Observações

- O arquivo principal foi colocado como `src/main.ino` para manter a compatibilidade com o pré-processamento Arduino usado pelo PlatformIO.
- Os módulos auxiliares ficam em `src/`.
- Os headers ficam em `include/`.
- A dependência CAN MCP2515 está declarada em `platformio.ini` a partir do repositório oficial da biblioteca ACAN2515.
- Esta versão ainda representa a rede CAN legada, não o gateway CAN FD final com MCP2518FD.

## Biblioteca CAN

O firmware legado usa `ACAN2515.h`; por isso o `platformio.ini` aponta para a biblioteca ACAN2515. Não substituir por bibliotecas MCP2515 incompatíveis sem adaptar a API de envio/recepção.

## Medição e falhas

O modo de falha programada foi removido. A função `getSensorValueForFaultAnalysis()` apenas repassa o valor medido recebido. Ela permanece como ponto de extensão para integração futura com uma medição física real no nó CAN.

## Gravação com NODE_ID sem editar código

O identificador do nó CAN agora é definido por variável de build, não por edição manual do código.

Build para o nó 1:

```bash
IOT_NODE_ID=1 pio run
```

Upload para o nó 2:

```bash
IOT_NODE_ID=2 pio run -t upload --upload-port /dev/ttyUSB0
```

Pela raiz do repositório:

```bash
./scripts/build_esp32_can_node.sh 1
./scripts/upload_esp32_can_node.sh 2 /dev/ttyUSB0
```

`NODE_ID=0` continua reservado para gateway. Nós funcionais devem usar IDs únicos maiores que zero.
