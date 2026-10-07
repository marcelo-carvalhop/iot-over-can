# iot-over-can

**iot-over-can** é uma plataforma experimental de IoT embarcada para estudo de redes distribuídas sobre CAN/CAN FD, integração de eventos determinísticos e assíncronos, descoberta e associação de dispositivos wireless, supervisão, diagnóstico e operação segura.

O sistema é composto por três classes de elemento. A **Probe 00** é uma sonda de instrumentação ligada ao CAN e ao computador por USB; observa o barramento e injeta comandos de bancada, mas não participa da eleição nem da lógica funcional distribuída. Os **Nodes CAN** são módulos ESP32 + MCP2515 com papel dinâmico de líder ou seguidor, funções locais próprias e descoberta BLE. O **sensor wireless** é um Raspberry Pi Pico W com MPU6050, aquisição local, DSP, diagnóstico e advertising BLE.

```text
Raspberry Pi Pico W
       │ BLE: identidade, perfil e presença
       │ Wi-Fi/UDP autenticado: telemetria, espectro e configuração
       ▼
Nodes CAN funcionais ───────── CAN clássico ───────── Probe 00 ── USB ── TUI
 sensores/atuadores locais                              instrumentação
 associação e plano de dados wireless
```

A Probe 00 não é um gateway funcional da aplicação distribuída. Nomes internos antigos como `GATEWAY_CAN` permanecem apenas como identificadores de compatibilidade do protocolo e da linha de comando.

## Estado funcional

O CAN clássico opera a 500 kbit/s com eleição de líder, supervisão, comandos administrativos, estado dos Nodes e uma função local de demonstração. O valor `LOCAL_SENSOR_DEMO_VALUE=0xAA` pertence ao próprio Node CAN e demonstra que o módulo pode possuir funções locais independentes dos sensores wireless.

A descoberta BLE e a associação lógica estão implementadas. O Pico W anuncia UUID, perfil e versão de protocolo. Vários Nodes podem observar o mesmo UUID e enviar pelo CAN suas medições independentes de RSSI. O operador escolhe explicitamente qual Node será responsável pelo sensor; a TUI apresenta como recomendação o Node com melhor recepção recente, mas não decide automaticamente. Depois da confirmação pelo Node, o sensor recebe um identificador lógico `parent.child`, por exemplo `04.01`.

O Node associado é a autoridade sobre o liveness wireless. A continuidade dos advertisements BLE mantém o vínculo `ONLINE`; 15 s sem advertisement levam a `STALE` e 45 s levam a `LOST`. O retorno do sensor recupera `ONLINE` sem recriar o filho lógico. `WIRELESS UNBIND` remove o vínculo somente depois da confirmação `UNBOUND` publicada pelo Node.

A associação é o **plano de controle**. O **plano de dados** é estabelecido a partir dela: o Node responsável anuncia por BLE uma oferta assinada, o sensor entra no ponto de acesso desse Node e os dois se autenticam por desafio-resposta com HMAC-SHA256 e chave por dispositivo. Telemetria, espectro, diagnósticos e configuração passam então pelo Node e chegam à TUI pelo CAN, em identificadores de menor prioridade que os de coordenação da rede. A TUI só oferece essas ações enquanto o Node informa sessão autenticada.

Essa camada está implementada e coberta por testes automatizados no computador, incluindo uma simulação que executa o código do Node e da Probe. **Ainda não foi validada em hardware.** O desenho, o modelo de ameaças e o roteiro de bancada estão em [`Documentacao/arquitetura/plano-de-dados-wireless.md`](Documentacao/arquitetura/plano-de-dados-wireless.md).

## TUI

A interface é uma aplicação Textual em tela cheia organizada por assunto, sem a antiga concentração de topologia, telemetria, comandos e logs na mesma visão. A navegação principal é:

```text
Início
├── Módulo CAN NN
│   └── Sensor NN.CC
│       └── Espectro FFT        # com sessão autenticada no plano de dados
├── Comandos
├── Rede CAN
├── Mensagens
├── Sensores sem fio
└── Ajuda
```

A tela inicial responde primeiro ao estado geral do sistema e ao que requer atenção. Cada módulo e cada sensor possui uma tela própria. Comandos ficam centralizados em uma única tela declarativa. Mensagens completas ficam em uma tela separada e o painel de mensagens recentes na tela inicial é opcional.

A associação wireless não depende de um atalho oculto. `F7` ou `w` abre **Sensores sem fio**, que apresenta candidatos disponíveis, observações por Node, recomendação de RSSI, vínculos ativos, associação explícita e desassociação. `Ctrl+A` permanece apenas como alias de compatibilidade. Os comandos internos `:wireless`, `:bind` e `:unbind` ficam disponíveis no campo manual avançado, mas não são necessários para o fluxo normal.

Teclas globais:

| Tecla | Ação |
|---|---|
| `F1` ou `?` | Ajuda |
| `F2` ou `i` | Início |
| `F3` ou `p` | Conexão |
| `F4` ou `c` | Comandos |
| `F5` ou `r` | Rede CAN |
| `F6` ou `m` | Mensagens |
| `F7` ou `w` | Sensores sem fio |
| `F10` ou `q` | Sair |
| `Esc` | Voltar |

A interface usa a paleta industrial **Aço industrial**: superfícies neutras em repouso, azul para interação, âmbar para atenção e vermelho para condições críticas. Toda condição é indicada por símbolo e palavra, não apenas por cor. O layout passa de duas colunas para uma conforme a largura e foi estruturado para evitar truncamento horizontal.

A especificação completa da interface está em [`Documentacao/interface/tui.md`](Documentacao/interface/tui.md).

## Estrutura do repositório

```text
iot-over-can/
├── Front/                       TUI Python/Textual e testes
├── Codigo/
│   ├── common/                  código C compartilhado: autenticação do enlace e dados no CAN
│   ├── node-can/                firmware ESP32 + MCP2515
│   ├── node-wifi/               firmware Raspberry Pi Pico W
│   └── scripts/                 build, upload, testes e segurança
├── Documentacao/                documentação técnica oficial
├── .github/workflows/           integração contínua
├── README.md
└── MANIFEST.json
```

A estrutura detalhada está em [`Documentacao/estrutura-do-projeto.md`](Documentacao/estrutura-do-projeto.md).

## Instalação, execução e testes

A TUI usa Python 3.11+ e as versões fixadas em `Front/pyproject.toml`: Textual 8.2.8, Rich 15.0.0 e pySerial 3.5.

```bash
./Codigo/scripts/setup_tui.sh
./Codigo/scripts/test_tui.sh
./Codigo/scripts/run_tui.sh
```

Para iniciar já apontando para a Probe 00:

```bash
./Codigo/scripts/run_tui.sh /dev/ttyUSB0
```

A TUI continua abrindo sem hardware. `F3` ou `p` permite escolher a porta depois.

Build de um Node CAN com ID 1:

```bash
./Codigo/scripts/build_esp32_can_node.sh 1
```

Upload de um Node CAN com ID 2:

```bash
./Codigo/scripts/upload_esp32_can_node.sh 2 /dev/ttyUSB0
```

O ID é fornecido ao build pelo script por meio de `IOT_NODE_ID`; não é necessário editar o código-fonte. O ID 0 é reservado à Probe 00.

Build do Pico W, de bancada ou com as chaves derivadas para um sensor:

```bash
./Codigo/scripts/build_pico.sh
./Codigo/scripts/build_pico.sh --sensor E6616408432B6F39
```

Testes nativos:

```bash
./Codigo/scripts/test_native_firmware.sh
```

Os procedimentos completos estão em [`Documentacao/build_e_teste.md`](Documentacao/build_e_teste.md).

## Segurança operacional

A TUI, o firmware do sensor e o enlace entre sensor e Node aplicam controles em fronteiras diferentes. Leitura permanece disponível conforme o modo operacional; comandos mutáveis passam pelo `SecurityManager`, e comandos mutáveis do sensor direto também precisam ser autorizados pelo firmware. Eleição, administração de Node, ajuste de liveness, associação/desassociação wireless e comandos `CMD ...` são classificados como ações mutáveis na TUI.

Arquivos locais:

```text
<raiz>/.env.local                         # token de manutenção e chave mestra do enlace
<raiz>/.env.sensor-<UUID>.local           # chaves derivadas para um sensor
~/.config/iot-over-can/security.json
~/.config/iot-over-can/tui.json          # preferências não sensíveis
```

Os três primeiros contêm material de segurança e devem permanecer fora do Git; em POSIX, os arquivos sensíveis usam permissão `0600`. `tui.json` contém apenas preferências visuais.

Provisionamento e verificação:

```bash
./Codigo/scripts/provision_sensor_security.sh
./Codigo/scripts/provision_sensor_security.sh --check
./Codigo/scripts/provision_sensor_security.sh --add-link-key       # instalação anterior à v0.17
./Codigo/scripts/provision_sensor_security.sh --sensor <UUID64>
```

Detalhes em [`Documentacao/modelo_de_seguranca.md`](Documentacao/modelo_de_seguranca.md).

## Documentação

O índice oficial está em [`Documentacao/README.md`](Documentacao/README.md). A arquitetura fica em [`Documentacao/arquitetura/arquitetura.md`](Documentacao/arquitetura/arquitetura.md), o protocolo em [`Documentacao/protocolo/protocolo.md`](Documentacao/protocolo/protocolo.md), os requisitos em [`Documentacao/requisitos.md`](Documentacao/requisitos.md) e a TUI em [`Documentacao/interface/tui.md`](Documentacao/interface/tui.md).

## Itens ainda a implementar

- validação em bancada do plano de dados wireless e da autenticação do enlace;
- persistência e recuperação dos vínculos após reinicialização dos Nodes;
- autenticação de origem dos comandos e dados no barramento CAN;
- entrega da chave do sensor pela estação no momento da associação, retirando a chave mestra dos Nodes;
- verificação FIDO2/OTP real da chave física do operador;
- migração experimental para CAN FD com MCP2518FD após estabilização do protocolo funcional.
