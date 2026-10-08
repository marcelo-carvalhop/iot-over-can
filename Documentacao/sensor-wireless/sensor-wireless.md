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

Wi-Fi permanece desabilitado até o sensor receber, por BLE, uma oferta autêntica do Node que assumiu o vínculo. O SSID e a senha do ponto de acesso desse Node são derivados da chave de rede gravada no build; não ficam no repositório nem trafegam pelo ar. DHCP é utilizado quando a interface Wi-Fi é ativada.

Máquina de estados do enlace (`NET` no console USB):

```text
DISABLED ── oferta autêntica ──► DISCOVERY ── CHALLENGE válido ──► BOUND
    ▲                                │  ▲                            │
    │   60 s sem autenticar          │  └── 10 s sem tráfego do Node ┘
    └────────────────────────────────┘
    ▲
    └──────────── CMD_RELEASE (desassociação) ─────────────── BOUND
```

Em `DISCOVERY` o sensor envia um `HELLO` autenticado a cada 1,5–3,5 s. Em `BOUND`, cada janela processada gera um datagrama de telemetria com o espectro completo; o Node decide o que segue para o CAN. Comandos do Node (`SET_CONFIG`, `CLEAR_DTC`, `PING`, consulta do menu de configuração) só são aceitos com código de autenticação válido e contador crescente.

`NET` informa também `LINK_KEYS` (`DEVICE`, `MASTER`, `NONE` ou `UUID_MISMATCH`), o Node pai, o filho lógico e os contadores de rejeição.

## Segurança

Comandos mutáveis pelo console USB exigem uma sessão `AUTH UNLOCK`. O token de administração e as chaves do enlace são fornecidos no build a partir de `.env.local` ou, de preferência, de `.env.sensor-<UUID>.local`, que contém apenas as chaves derivadas para aquele sensor. O procedimento de geração e renovação está em `../modelo_de_seguranca.md`.

## Relação com a associação

O sensor continua anunciando seu UUID64 e perfil por BLE, inclusive com sessão ativa, o que mantém o liveness do vínculo no Node. A associação é decidida no lado dos Nodes CAN; o sensor toma conhecimento dela pela oferta autenticada e então estabelece o plano de dados. O firmware do protocolo `0x06` é necessário: sensores com o firmware anterior continuam sendo descobertos e associados, mas não abrem sessão.

## Reassociação a outro Node

O firmware do sensor não foi alterado na versão 0.18; a reassociação automática usa o comportamento que ele já tinha. Com sessão ativa o sensor ignora ofertas. Quando o Node responsável deixa de responder, ele desiste da sessão depois de 10 s sem tráfego (`NET_SESSION_TIMEOUT_MS`) e volta a procurar ofertas; a de outro Node só é aceita depois de 15 s nesse estado (`NET_OFFER_SWITCH_HOLD_MS`), para que uma oferta antiga repetida por um terceiro não faça o rádio alternar de ponto de acesso. Uma desassociação explícita (`CMD_RELEASE`) dispensa essa espera.

Essas duas constantes respondem pela maior parte do tempo de uma reassociação, cerca de 25 s na bancada virtual, enquanto a rede decide em cerca de 5 s. Reduzi-las é possível e deve ser feito com medição em bancada. Ver [`../arquitetura/metricas-e-ensaios.md`](../arquitetura/metricas-e-ensaios.md).

## Operação na TUI

A operação normal do vínculo não depende de comandos internos. `F7` ou `w` abre a tela **Sensores sem fio**, na qual cada UUID é apresentado uma única vez mesmo quando vários Nodes o observam. A tela mostra o perfil anunciado, o melhor Node observado por RSSI e as demais observações recentes. O operador pode aceitar o Node sugerido ou selecionar explicitamente outro observador antes de confirmar a associação.

Depois da confirmação `BOUND`/`ONLINE` publicada pelo Node responsável, o sensor passa a aparecer pelo identificador lógico `NN.CC`. A ação **Desassociar** envia `WIRELESS UNBIND` e mantém o filho visível até a confirmação `UNBOUND`; somente então a TUI remove `NN.CC`. Se o Pico W continuar anunciando BLE, o mesmo UUID volta imediatamente à lista de dispositivos disponíveis.

A tela do sensor associado distingue deliberadamente **vínculo de controle** de **plano de dados**. Enquanto o Node não informa sessão autenticada, a TUI mostra o estado do plano de dados e não oferece telemetria, FFT ou configuração. Com a sessão ativa, a tela passa a mostrar métricas, configuração e as estatísticas do enlace (sinal Wi-Fi, perdas, datagramas rejeitados).
