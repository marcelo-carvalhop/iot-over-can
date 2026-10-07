# Sensor wireless

O sensor wireless atual usa Raspberry Pi Pico W, MPU6050 e firmware em C/Pico SDK. O dispositivo executa aquisição em `POLLING`, processamento de sinais, diagnóstico, BLE advertising e infraestrutura Wi-Fi controlada por configuração em runtime.

## Hardware

```text
Pico W
I2C0 SDA: GP0
I2C0 SCL: GP1
MPU6050: endereço 0x68
```

O pino de DRDY pode existir fisicamente, mas a aquisição operacional atual não depende dele.

## DSP

A janela padrão possui 512 amostras a 1000 Hz. O firmware calcula métricas como RMS, curtose, fator de crista, FFT, entropia espectral, PPV e STA/LTA. O perfil BLE anunciado é `VIBRATION`.

## Identidade e descoberta

O UUID64 é derivado da identidade única da placa e permanece estável após reinicialização. O advertisement BLE inclui UUID, perfil e versão de protocolo e pode ser observado simultaneamente por vários Nodes CAN.

## Rede

Wi-Fi permanece desabilitado até existir provisionamento válido e uma ação explícita para habilitá-lo. Credenciais não ficam compiladas de forma fixa no repositório. DHCP é utilizado quando a interface Wi-Fi é ativada.

## Segurança

Comandos mutáveis exigem uma sessão `AUTH UNLOCK`. O token de administração é fornecido no build a partir de `.env.local`. O procedimento de geração e renovação está em `../modelo_de_seguranca.md`.

## Relação com a associação

O sensor continua anunciando seu UUID64 e perfil por BLE. A associação atual é feita no lado dos Nodes CAN: o Pico W não precisa receber um comando BLE de bind para que a infraestrutura atribua sua identidade a um Node. Por isso, esta etapa não exige alteração ou nova gravação do firmware do Pico W. O canal de dados posterior ao vínculo será implementado separadamente.

## Operação na TUI

A operação normal do vínculo não depende de comandos internos. `F7` ou `w` abre a tela **Sensores sem fio**, na qual cada UUID é apresentado uma única vez mesmo quando vários Nodes o observam. A tela mostra o perfil anunciado, o melhor Node observado por RSSI e as demais observações recentes. O operador pode aceitar o Node sugerido ou selecionar explicitamente outro observador antes de confirmar a associação.

Depois da confirmação `BOUND`/`ONLINE` publicada pelo Node responsável, o sensor passa a aparecer pelo identificador lógico `NN.CC`. A ação **Desassociar** envia `WIRELESS UNBIND` e mantém o filho visível até a confirmação `UNBOUND`; somente então a TUI remove `NN.CC`. Se o Pico W continuar anunciando BLE, o mesmo UUID volta imediatamente à lista de dispositivos disponíveis.

A tela do sensor associado distingue deliberadamente **vínculo de controle** de **plano de dados**. Enquanto o canal Pico W ↔ Node CAN para telemetria e comandos não estiver implementado, a TUI mostra essa limitação de forma explícita e não oferece controles de telemetria, FFT ou configuração como se fossem funcionais através do Node.
