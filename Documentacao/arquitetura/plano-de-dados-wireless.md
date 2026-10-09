# Plano de dados wireless e autenticação do enlace

Este documento descreve como a telemetria, o espectro, os diagnósticos e a configuração de um sensor wireless trafegam entre o Pico W e a TUI, passando pelo Node CAN responsável, e como cada trecho é autenticado. Corresponde à versão 0.17 do projeto e ao protocolo `0x06` do sensor.

> **Estado de validação.** A lógica dos dois firmwares e o formato de todas as mensagens são exercitados por testes automatizados no computador (seção 9). O firmware do Pico W compila com o Pico SDK. **Nada desta camada foi ainda executado em hardware**: coexistência BLE + Wi-Fi no ESP32 e no Pico W, alcance, temporização e consumo precisam ser confirmados em bancada (seção 10).

## 1. Visão geral

```text
                (1) anúncio BLE: UUID, perfil, protocolo
   Pico W  ───────────────────────────────────────────────►  Nodes CAN
           ◄───────────────────────────────────────────────  Node responsável
                (2) oferta BLE assinada: "Node N assumiu você"

   Pico W  ══ (3) Wi-Fi do Node N ═ (4) HELLO/CHALLENGE/CONFIRM ═►  Node N
           ══ (5) dados autenticados (UDP, contador + HMAC) ══════►

   Node N  ── (6) CAN clássico, 0x380+N, segmentado ──►  Probe 00 ── USB ──► TUI
           ◄─ (7) CAN clássico, 0x304, um quadro ──────  Probe 00 ◄─ USB ──  TUI
```

O vínculo lógico criado por `WIRELESS BIND` continua sendo a decisão do operador. O plano de dados é consequência dele: ao aceitar o vínculo, o Node passa a anunciar uma oferta e a manter um ponto de acesso; ao receber `WIRELESS UNBIND`, libera o sensor e desliga o rádio quando não restam vínculos.

Não existe concentrador wireless. Cada Node com sensores associados é o ponto de acesso dos seus próprios filhos, em um canal de 2,4 GHz derivado do seu ID (1, 6 ou 11).

## 2. Hierarquia de chaves

Todas as chaves têm 32 bytes e todas as derivações usam HMAC-SHA256.

```text
K_master                                   Nodes CAN e máquina de provisionamento
  ├── K_dev  = HMAC(K_master, "ioc-dev-v1" || UUID64)        uma por sensor
  └── K_net  = HMAC(K_master, "ioc-net-v1")                  rede Wi-Fi local
        └── senha do AP do Node N = hex(HMAC(K_net, "ioc-psk-v1" || N))[0..31]

K_sess = HMAC(K_dev, "ioc-sess-v1" || UUID64 || N || nonce_sensor || nonce_node)
```

| Onde fica | O que guarda | Consequência da extração |
|---|---|---|
| Node CAN | `K_master` | compromete o enlace de todos os sensores; exige `--renew` |
| Sensor (build com `--sensor`) | `K_dev`, `K_net` | permite personificar **aquele** sensor e entrar na rede Wi-Fi; não revela `K_dev` de outros sensores nem `K_master` |
| Probe 00 e TUI | nenhuma chave de enlace | — |

`K_net` protege apenas o acesso à rede Wi-Fi. A identidade de cada sensor é provada com `K_dev`, na camada de aplicação: entrar no ponto de acesso não basta para enviar dados.

Um sensor compilado sem `--sensor` recebe `K_master` e deriva as próprias chaves no boot. Esse modo existe para a bancada, quando o UUID ainda não é conhecido, e não deve ser usado de forma permanente.

Sem chave, ou com `K_dev` derivada para outro UUID, o sensor ignora ofertas e nunca liga o Wi-Fi. Um Node sem chave continua descobrindo e associando sensores, mas publica o estado `NO_KEY` e não abre ponto de acesso.

## 3. Oferta de vínculo (BLE)

O Node responsável anuncia, a cada 100 ms, um *Manufacturer Specific Data* de 27 bytes, não conectável:

```text
company(2)=0xFFFF | 'I' 'O' | versão=1 | node | UUID64(8, BE) | canal | nonce(4, BE) | tag(8)

tag = HMAC(K_dev, "ioc-offer-v1" || bytes[2..18])[0..7]
```

O sensor mantém varredura passiva (60 ms a cada 500 ms) enquanto não tem sessão. Ele só processa ofertas com o próprio UUID e tag válida. Uma oferta válida informa o número do Node; SSID (`IOC-NN`) e senha são derivados localmente e nunca trafegam pelo ar.

Havendo mais de um sensor aguardando sessão, o Node alterna as ofertas a cada 400 ms. A oferta deixa de ser anunciada quando a sessão é estabelecida.

O nonce da oferta não é conferido pelo sensor: uma oferta capturada continua autêntica. Para limitar o efeito de uma repetição, o sensor ignora ofertas enquanto tem sessão e, depois de entrar no ponto de acesso de um Node, só segue a oferta de outro Node após 15 s.

## 4. Handshake

Todo datagrama UDP (porta 4242) tem o mesmo envelope:

```text
'I' 'L' | versão=6 | tipo | contador(4, LE) | payload(n) | tag(8)

tag = HMAC(chave, cabeçalho || payload)[0..7]
```

| Tipo | Sentido | Chave | Payload |
|---|---|---|---|
| `0x01 HELLO` | sensor → Node | `K_dev` | UUID64, perfil, protocolo, `nonce_sensor`(8) |
| `0x02 CHALLENGE` | Node → sensor | `K_dev` | node, filho, `nonce_node`(8), eco de `nonce_sensor` |
| `0x03 CONFIRM` | sensor → Node | `K_sess` | eco dos dois nonces |
| `0x10 DATA_UP` | sensor → Node | `K_sess` | mensagem do sensor |
| `0x11 DATA_DOWN` | Node → sensor | `K_sess` | comando do Node |

O que cada passo prova:

1. **HELLO.** O Node lê o UUID, verifica que há vínculo para ele e deriva `K_dev`. Um HELLO de UUID sem vínculo não recebe resposta.
2. **CHALLENGE.** A tag prova ao sensor que o Node conhece `K_dev`; o eco do nonce prova que a resposta é ao HELLO mais recente.
3. **CONFIRM.** A tag com `K_sess` prova ao Node que o sensor conhece `K_dev` e respondeu ao nonce recém-gerado. Só então a sessão passa a valer.

Um HELLO recebido durante uma sessão ativa cria um handshake *pendente* e não derruba a sessão em vigor; a troca só ocorre quando um CONFIRM, ou um dado autenticado com a chave pendente, é aceito. Isso cobre o sensor que reiniciou e o CONFIRM que se perdeu.

O HELLO não contém nada que o Node possa conferir quanto à atualidade, então uma cópia capturada continua autêntica. Três regras limitam o que uma repetição consegue:

- o HELLO que originou a sessão em vigor é ignorado e contado em `replay`;
- o mesmo HELLO recebido de novo reenvia o mesmo CHALLENGE, sem reiniciar a troca;
- um handshake iniciado há menos de 1 s não é substituído por outro HELLO.

## 5. Sessão

- Contador de 32 bits por sentido, iniciado em 1. O receptor aceita apenas valores estritamente maiores que o último aceito; repetidos e fora de ordem são descartados e contados.
- O Node envia `PING` a cada 3 s. O sensor volta a `DISCOVERY` após 10 s sem tráfego válido do Node; o Node encerra a sessão após 10 s sem tráfego válido do sensor e volta a anunciar a oferta.
- Em `DISCOVERY`, se o enlace Wi-Fi fica inativo por 10 s, o sensor refaz a associação ao ponto de acesso por inteiro (o Node pode ter reiniciado).
- Após 60 s em `DISCOVERY` sem conseguir autenticar e sem ver novas ofertas, o sensor desliga o Wi-Fi e volta a aguardar uma oferta.
- A cada nova sessão o Node pede o menu de configuração ao sensor e descarta o que sabia da sessão anterior; edições de configuração não aplicadas também são descartadas.
- Com sessão ativa o sensor ignora ofertas. A troca de Node responsável é sempre `UNBIND` seguido de `BIND`.
- Em `UNBIND`, o Node envia `CMD_RELEASE`; o sensor encerra a sessão, apaga SSID e senha da memória e desliga o Wi-Fi. O ponto de acesso do Node é desligado 400 ms depois, se não houver outro vínculo, para que o aviso chegue a sair.

O payload de `DATA_UP`/`DATA_DOWN` são as estruturas já definidas em `Codigo/node-wifi/edge_protocol_definitions.h` (telemetria com FFT, menu de configuração, `SET_CONFIG`, `ACK_CONFIG`, `PING/PONG`, `CLEAR_DTC`, DTC urgente). Os campos `session_token` e `request_counter` dessas estruturas permanecem por compatibilidade de layout; a proteção vem do envelope.

O CLAIM por chave compartilhada de 64 bits e a opção de build que permitia mutação UDP sem autenticação (protocolo `0x05`) foram removidos.

## 6. Do Node ao barramento CAN

### Identificadores

| ID | Sentido | Uso |
|---|---|---|
| `0x304` | Probe 00 → Node | comando do plano de dados, um quadro |
| `0x380 + N` (`0x381`–`0x39F`) | Node N → barramento | dados dos sensores do Node N, segmentados |

São os identificadores de menor prioridade do projeto. Tráfego de sensores nunca vence a arbitragem contra eleição (`0x050`), controle (`0x080`), liveness (`0x100`), descoberta (`0x280`) ou associação (`0x300`–`0x35F`).

### Segmentação

```text
byte 0   filho(3 bits) | tipo(5 bits)
byte 1   transferência(3 bits) | índice do segmento(5 bits)
índice 0:  byte 2 = tamanho total, byte 3 = CRC-8 (polinômio 0x07), bytes 4..7 = dados
índice >0: bytes 2..7 = dados
```

Até 190 bytes por transferência (32 quadros). Os segmentos saem em ordem do mesmo Node; uma lacuna, um CRC incorreto ou 1,5 s sem completar abortam a remontagem.

O Node enfileira as transferências e as escoa pelo laço principal com duas restrições:

- só acrescenta quadros enquanto o buffer de transmissão do controlador tem menos de 4 quadros pendentes. Esse buffer é compartilhado com eleição, liveness e associação e é FIFO; sem o limite, um quadro de controle esperaria atrás de uma rajada de dados ou seria descartado por falta de espaço, anulando a prioridade garantida pelos identificadores;
- aguarda 40 ms após cada bloco de espectro, tempo para a Probe escrever a linha correspondente na serial.

O buffer de recepção CAN foi ampliado de 32 para 128 quadros, e a serial da Probe ganhou buffer de transmissão de 2 kB.

| Tipo | Conteúdo | Tamanho | Quadros |
|---|---|---|---|
| 1 `TELEMETRY` | sequência, modo, aquisição, janela, taxas, RMS, curtose, crista, pico, entropia, PPV, bateria, DTC | 50 bytes | 9 |
| 2 `SPECTRUM` | cabeçalho (17 bytes) + até 80 faixas de 16 bits | até 177 bytes | até 30 |
| 3 `CMD_ACK` | seq, opcode, resultado, detalhe | 5 bytes | 2 |
| 4 `DTC` | código, sintoma, severidade, instante | 8 bytes | 2 |
| 5 `LINK` | estado do enlace, RSSI Wi-Fi, idade da sessão, contadores, encaminhamento | 15 bytes | 3 |
| 6 `CONFIG` | modo, janela, taxas, limiar, ganho, resultado | 21 bytes | 4 |

### O que é encaminhado

O sensor envia ao Node, por Wi-Fi, uma amostra completa (com o espectro) a cada janela processada. O Node decide o que vai ao CAN:

- **telemetria**: uma amostra por período (padrão 1000 ms, ajustável de 200 a 10 000 ms), 9 quadros;
- **espectro**: somente sob pedido, reduzido para 16 a 256 faixas pelo máximo de cada faixa e normalizado em 16 bits com a escala em ponto flutuante — 64 faixas cabem em uma transferência, 256 em quatro;
- **DTC urgente** e **confirmações**: imediatamente;
- **estado do enlace**: a cada mudança e a cada 5 s.

A 500 kbit/s o barramento comporta cerca de 3 800 quadros de 8 bytes por segundo; a telemetria padrão de um sensor ocupa 9.

### Comandos

```text
seq | node | filho | opcode | arg0 | arg1 | arg2 | arg3
```

| Opcode | Ação | Onde é executada |
|---|---|---|
| `0x01 STREAM` | liga, desliga, envia uma amostra, define o período | Node |
| `0x02 FFT` | pede um espectro com N faixas | Node, com a próxima amostra |
| `0x03 STATUS` | republica `LINK` e `CONFIG` | Node |
| `0x10`–`0x14 SET_*` | prepara modo, taxa, janela, limiar ou ganho | Node (cópia em edição) |
| `0x1F APPLY` | envia a configuração preparada | sensor |
| `0x20 DTC_CLEAR` | limpa os diagnósticos | sensor |

Cada comando recebe `CMD_ACK` com `ACCEPTED`, `APPLIED`, `REJECTED`, `NO_LINK` (sem sessão autenticada) ou `UNKNOWN`. Ações que dependem do sensor respondem `ACCEPTED` de imediato e o resultado final quando o sensor confirma, ou `REJECTED` após 3 s sem resposta (`REASON=SENSOR_TIMEOUT`).

Alterações de configuração partem da configuração atual do sensor. Enquanto o Node não a conhece, `SET_*` e `APPLY` são recusados com `REASON=CONFIG_UNKNOWN` e o Node pede o menu novamente; basta repetir o comando.

## 7. Probe 00 e TUI

A Probe converte as linhas da TUI em quadros de comando e remonta as transferências em linhas de texto.

Entrada (já usada pela TUI; passa pelo `SecurityManager` como comando mutável):

```text
CMD TARGET=01.01 ACTION=TELEMETRY TX=A1B2C3 STATE=ON|OFF|ONCE|FAST|SLOW
CMD TARGET=01.01 ACTION=TELEMETRY_PERIOD TX=... PERIOD_MS=500
CMD TARGET=01.01 ACTION=FFT TX=... BINS=64
CMD TARGET=01.01 ACTION=CONFIG TX=... MODE=STRUCTURAL RATE_HZ=500 WINDOW=HAMMING WINDOW_SIZE=256 STALTA=3.5 GAIN=1.25
CMD TARGET=01.01 ACTION=DTC_CLEAR TX=...
CMD TARGET=01.01 ACTION=STATUS TX=...
```

Saída:

```text
TEL NODE=1 CHILD=1 SEQ=0 GW_TS_MS=... MODE=ROTATING ACQ=POLLING WIN=512 AXIS=VECTOR FFT_VALID=YES RATE_REQ_HZ=... RMS=... RMS_UNIT=m/s2 ...
FRAG NODE=1 CHILD=1 TYPE=FFT TRANSFER=1 INDEX=0 COUNT=1 FORMAT=U16_SCALED SCALE=... RATE_HZ=... FFT_SIZE=128 WINDOW=HANN DATA=<hex>
DTC NODE=1 CHILD=1 CODE=0x2002 SYMPTOM=0x16 SEVERITY=1 TS_MS=...
ACK COMMAND=CONFIG STATE=APPLIED TX=A1B2C3 NODE=1 CHILD=1 DETAIL=1
[GW] WIRELESS_LINK node=1 child=1 state=SECURE rssi=-47 age_s=12 rx=30 auth_fail=0 replay=0 lost=0 stream=ON period_ms=1000
[GW] WIRELESS_CONFIG node=1 child=1 status=APPLIED mode=STRUCTURAL rate_req_hz=500.00 rate_eff_hz=500.00 window=HAMMING window_size=256 stalta=3.500 gain=1.2500
```

`ACQ`, `WIFI` e `DTC_LIST` existem apenas no console USB do sensor; pela Probe respondem `REJECTED REASON=UNSUPPORTED_VIA_NODE`, e a TUI não as oferece nesse modo.

Na TUI, o sensor associado ganha o campo `data_link_state` (`NONE`, `DOWN`, `OFFERING`, `HANDSHAKE`, `SECURE`, `NO_KEY`). Telemetria, FFT e configuração só são oferecidas em `SECURE`. Queda de sessão e crescimento dos contadores de rejeição geram intercorrências (`ENLACE`, `SEGURANCA`).

O Node republica o estado da associação a cada 10 s, de modo que uma TUI aberta depois do `BIND` reconstrói a topologia.

## 8. Modelo de ameaças

| Ameaça | Resultado |
|---|---|
| Dispositivo sem chave anuncia o UUID de um sensor legítimo | é descoberto e pode ser associado pelo operador, mas não autentica: o enlace fica em `OFFERING`/`HANDSHAKE` e nenhum dado chega ao CAN |
| Oferta BLE forjada para atrair o sensor a outro ponto de acesso | descartada: a tag depende de `K_dev` |
| Oferta autêntica capturada e repetida | não dá acesso a nada: o sensor só entra em pontos de acesso com a senha derivada de `K_net`. Sem sessão ativa, porém, uma oferta antiga de outro Node repetida continuamente pode atrasar ou impedir a associação (negação de serviço por rádio) |
| Ponto de acesso falso com o SSID do Node | não conhece a senha derivada de `K_net`; mesmo com ela, não produz um CHALLENGE válido sem `K_dev` |
| Telemetria ou comando forjado na rede Wi-Fi | descartado: tag inválida, contado em `auth_fail` |
| Repetição de datagrama autêntico | descartada pelo contador, contada em `replay` |
| HELLO ou CHALLENGE autêntico repetido | não altera a sessão em vigor. Um dispositivo já dentro da rede Wi-Fi, repetindo HELLOs antigos no instante certo, ainda pode atrapalhar o estabelecimento de uma sessão nova (negação de serviço) |
| Extração da memória de um sensor | compromete apenas aquele sensor e a senha do Wi-Fi |
| Leitura do tráfego | **não protegida** na camada de aplicação: os dados são autenticados, não cifrados. A confidencialidade é a do WPA2 do ponto de acesso |

Limites conhecidos:

- **CAN sem autenticação de origem.** Qualquer nó no barramento pode emitir `0x304` ou falsificar `0x380+N`. O enlace autenticado termina no Node; o barramento é tratado como domínio confiável, como nos demais comandos CAN do projeto.
- **Tag de 64 bits.** Adequada ao volume e à vida útil das sessões deste projeto; não é um parâmetro para uso geral.
- **Chave mestra em todos os Nodes.** A extração da memória de um Node compromete o enlace inteiro. A entrega de `K_dev` pela estação no momento da associação eliminaria isso e fica como evolução.
- **Sem elemento seguro.** As chaves ficam na memória flash dos microcontroladores.
- **Vínculos não persistem.** A reinicialização de um Node apaga vínculos e sessões.
- **Negação de serviço por rádio está fora do escopo.** Além das repetições citadas na tabela, qualquer interferência em 2,4 GHz interrompe o plano de dados. O que o protocolo garante é que isso não resulta em dados ou comandos falsos.

## 9. Verificação automatizada

| Verificação | O que cobre | Onde |
|---|---|---|
| Vetores do NIST e da RFC 4231 | SHA-256 e HMAC-SHA256 | `Codigo/common/tests/test_ioc_link.c` |
| Handshake, oferta, envelope, contador | aceitação e todas as rejeições (bit alterado, truncado, chave errada, replay) | idem |
| Segmentação | ida e volta para todos os tamanhos de 1 a 190 bytes, lacuna, CRC, expiração, filhos intercalados | idem |
| Conferência cruzada com Python | derivações, oferta e envelope idênticos a `hmac`/`hashlib` | `Front/tests/test_wireless_data_plane_v017.py` |
| Script de provisionamento | chaves derivadas, permissões, ausência de segredos na saída | idem |
| Simulação sensor ↔ Node ↔ Probe | código real de `wireless_link.cpp` com rádio, UDP e CAN substituídos; sessão, telemetria, espectro, limite de ocupação do buffer CAN, configuração, CONFIRM e menu perdidos, ataques, expiração e desassociação | `Codigo/node-can/test/host/` |
| Ponta a ponta até a TUI | a saída da Probe simulada alimenta o decodificador e o domínio reais da TUI | `Front/tests/test_wireless_data_plane_v017.py` |

```bash
./Codigo/scripts/test_native_firmware.sh
./Codigo/scripts/test_tui.sh
```

A simulação usa um sensor escrito para o teste, não o firmware do Pico W. O driver de rede do Pico (`edge_network_driver.c`) é verificado por compilação (Pico SDK 2.2.0 e 2.3.1) e por revisão, não por execução. O firmware do ESP32 é compilado apenas na integração contínua.

Uma revisão independente do código, feita antes da primeira entrega, apontou defeitos que foram corrigidos e passaram a ter teste: o caminho de inclusão que faria o `lwipopts.h` do Pico substituir o do ESP-IDF, a saturação do buffer de transmissão CAN, o travamento da configuração após a perda do menu, a recuperação com CONFIRM perdido e uma condição de corrida no tempo limite de sessão do sensor.

## 10. Validação em bancada (pendente)

Ordem sugerida, do menor risco ao maior:

1. `provision_sensor_security.sh --add-link-key` (ou sem argumentos em uma instalação nova) e `--check`.
2. Gravar a Probe 00 e um Node. No monitor serial do Node, confirmar `[WLINK] LINK_KEY=PROVISIONED` e que eleição, liveness e descoberta BLE continuam como antes.
3. Gravar o Pico W em modo de bancada (`build_pico.sh`). No console USB, `NET` deve mostrar `LINK_KEYS=MASTER`.
4. Associar pela TUI (`F7`). Esperado no Node: `AP=ON ssid=IOC-NN`; na TUI: plano de dados em "módulo anunciando a oferta", depois "autenticação em andamento", depois "sessão autenticada ativa". No Pico, `NET` passa a `STATE=BOUND PARENT_NODE=N`.
5. Conferir telemetria na tela do sensor, pedir FFT com 64 e 256 faixas, alterar a configuração e confirmar `Aplicada pelo sensor`.
6. Desligar o Pico: em cerca de 10 s o plano de dados volta a "anunciando a oferta"; religar e confirmar a recuperação sem novo `BIND`.
7. Desassociar: o Node deve registrar `AP=OFF` e o Pico voltar a `STATE=DISABLED`.
8. Recompilar o Pico com `--sensor <UUID>` e repetir o passo 4; `NET` deve mostrar `LINK_KEYS=DEVICE`.
9. Gravar um segundo Pico com as chaves do primeiro: `NET` deve mostrar `LINK_KEYS=UUID_MISMATCH` e o sensor não deve abrir sessão.

Pontos a observar e registrar:

- primeira compilação do firmware do ESP32 (job `esp32-platformio` da integração contínua ou `build_esp32_can_node.sh`): tamanho (a partição passou a `min_spiffs.csv`) e memória livre;
- estabilidade da varredura BLE do Node com o ponto de acesso ativo (a varredura ocupa 50% do tempo de rádio; reduzir a janela se a associação Wi-Fi ou o DHCP falharem), e da eleição/liveness CAN durante um espectro de 256 faixas;
- espectro de 256 faixas completo na TUI (quatro blocos); se faltarem blocos, aumentar a pausa entre eles em `wireless_link.cpp`;
- retorno do sensor após reiniciar o Node e refazer o `BIND`;
- tempo entre o `BIND` e a sessão autenticada;
- perdas Wi-Fi (`lost`) e CAN em função da distância e do período de telemetria;
- comportamento com dois sensores no mesmo Node e com sensores em Nodes diferentes.

Divergências devem ser registradas em `Documentacao/desafios_e_resolucoes.md`.
