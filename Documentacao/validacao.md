# Validação

Como o projeto é verificado, do teste de uma função à bancada com o hardware, e o que já foi confirmado em cada nível.

## 1. Níveis de verificação

| Nível | O que verifica | Quando roda |
|---|---|---|
| testes das bibliotecas C | criptografia (com vetores do NIST e da RFC 4231), canal de dados, segmentação, medição do barramento, reassociação, relógio, painel | a cada envio, na integração contínua |
| fuzz dos decodificadores | entradas aleatórias e mensagens válidas alteradas, com AddressSanitizer e UBSan | idem |
| simulação do canal de dados | o código real do canal de dados de um módulo, com rádio, Wi-Fi e CAN substituídos | idem |
| bancada virtual | o firmware completo de vários módulos num processo, com barramento, rádios e sensores simulados | idem |
| leitor da documentação | conversão dos documentos, diagramação de todas as páginas e o firmware sobre imitações da placa: telas dentro dos 400×600, navegação, posição guardada, sono e desligamento | idem |
| testes da estação | decodificadores, estado, telas, segurança, identificação do operador e auditoria, métricas e ensaios, alimentados pelas gravações da bancada virtual | idem |
| compilação | todos os firmwares para as placas reais, as opções de compilação documentadas e a versão única (`versao.py --check`) | idem |
| bancada física | comportamento com o hardware: rádio, alimentação, tempos reais | em sessões de bancada |

## 2. Testes automatizados

```bash
./Codigo/scripts/test_native_firmware.sh          # bibliotecas, fuzz, sensor acústico, simulações, bancada virtual e leitor
./Codigo/scripts/test_tui.sh                      # estação de operação
python3 -m unittest discover -s Simulacao/tests   # laboratório virtual
python3 -m unittest discover -s Pesquisa/tests    # ferramenta de pesquisa
python3 Codigo/scripts/versao.py --check          # a versão do arquivo VERSION em todos os lugares
./Codigo/scripts/check_build_variants.sh          # as opções de compilação documentadas ainda compilam
```

`test_native_firmware.sh` precisa de `cc` e `c++` e executa, em ordem: a validação de configuração do sensor; os testes de `ioc_link` (criptografia, canal de dados, segmentação, relatório acústico, custo da criptografia); a parte de rede de `ioc_link` (duração de quadro, mensagem autenticada, segmentação em quadros maiores, reassociação, tabela de vínculos, relógio, motivo de reinício, autoteste, medição da alimentação, detecção de isolamento e autenticação dos comandos CAN); os testes do painel; o fuzz dos decodificadores; o processamento do sensor acústico; a simulação do canal de dados; a bancada virtual com todos os cenários; a mesma bancada com a autenticação de origem ligada (`IOT_CAN_AUTH=1`); as opções de compilação documentadas; e o leitor da documentação.

`versao.py --check` falha se `MANIFEST.json`, os pacotes Python ou os `library.json` repetirem uma versão diferente da do arquivo `VERSION`, ou se algum firmware voltar a ter a versão escrita no código. `check_build_variants.sh` compila (só a verificação de sintaxe) o firmware dos módulos com cada opção documentada ligada: medição da alimentação, isolamento ajustado ou desligado, esperas do sensor, potência do Wi-Fi, autenticação dos comandos CAN. A bancada virtual só roda com os padrões, e uma opção que ninguém liga acumula erros sem que ninguém veja; o script também confere que a autenticação ligada sem a chave de enlace não compila.

Os testes da estação percorrem as telas em 48×18, 60×24, 80×24, 100×30 e 160×48 para garantir que nada seja cortado, conferem a biblioteca C contra implementações independentes em Python e reproduzem o exemplo publicado da análise de tempo de resposta. Um deles refaz as gravações da bancada virtual e as compara com as versionadas. Outros cobrem a identificação do operador: senhas e cadastro (permissões, formato, adulteração), bloqueio de tentativas, expiração por inatividade, perfis, os três modos, a gravação do autor de cada ação no histórico, a atribuição da resposta de uma associação a quem a pediu, a ausência de segredos nos registros, a migração de bancos antigos e a detecção de registros de auditoria editados ou apagados. Outros cobrem a abertura da estação com a rede já montada: o sensor pré-aprovado e ausente não gera alerta nem entra nas contagens, e o que foi visto vivo e depois some continua alertando.

## 3. Bancada virtual

`Codigo/node-can/test/host/sim_network.cpp` executa a Probe 00 e três módulos num único processo. Cada módulo é o firmware real, compilado uma vez por módulo, cada cópia dentro de um *namespace* próprio, com as próprias variáveis globais. O que no laboratório é físico, a bancada fornece:

- **barramento CAN:** arbitragem por identificador, duração real de cada quadro a 500 kbit/s e confirmação (um quadro sem outro módulo para confirmá-lo é retransmitido e conta como erro);
- **relógio:** passos de 0,5 ms;
- **rádio:** anúncios BLE com o sinal definido pelo cenário, ofertas e pontos de acesso, com atraso configurável;
- **sensores:** seguem as regras do firmware do Pico W e autenticam-se com a biblioteca real.

```bash
./Codigo/scripts/build_network_sim.sh /tmp/sim_network
/tmp/sim_network --list
/tmp/sim_network owner_failover              # linhas da Probe 00: "@12345 [GW] ..."
/tmp/sim_network owner_failover --verbose    # inclui a serial de cada módulo: "#N2@12345 ..."
```

| Cenário | O que verifica |
|---|---|
| `formation` | eleição, sinal de presença e estatísticas do barramento |
| `sensor` | associação, sessão, telemetria, tempos de resposta e mudança de política |
| `owner_failover` | o responsável sai do ar, o líder reassocia e o módulo volta sem disputar o sensor |
| `sensor_moves` | o sensor muda de alcance; reassociação por perda de alcance |
| `manual_policy` | com a política manual, ninguém assume o sensor |
| `leader_failure` | o líder sai do ar e volta como seguidor |
| `leader_failure_no_probe` | o mesmo, com a Probe impedida de transmitir |
| `silence_leader` | o líder congela e retoma; a rede termina com um só líder e sem falhas indevidas |
| `silence_leader_with_sensor` | o líder fica 43 s parado e não reassocia um sensor que continuou funcionando |
| `ap_outage`, `drop_session` | o sensor se autentica de novo |
| `bus_load` | carga de 40 % de alta prioridade e carga de baixa prioridade |
| `two_sensors` | dois sensores redistribuídos entre dois módulos |
| `failover_retry` | o módulo escolhido perde o sensor antes de assumir; o líder escolhe outro, mas só depois do prazo de 2 min da primeira sessão dele |
| `failover_dead_target` | o módulo escolhido sai do ar; o responsável anterior mantém o vínculo |
| `restart_keeps_binding` | o módulo reinicia e retoma da memória o vínculo gravado; uma partida normal não escreve nada sobre a alimentação, e a que vem de uma queda de tensão é avisada, contada na memória e consultável por `ENERGIA` |
| `handshake_slow_radio` | dois sensores no mesmo módulo com 3,6 s de atraso de rádio; cada sessão abre em cerca de 5 s |
| `first_session_slow` | primeiro canal de dados lento (40 s para entrar no Wi-Fi) com o módulo responsável surdo ao BLE enquanto o cria: o sensor em reassociação automática abre a sessão no módulo escolhido, no mesmo tempo que o em manual, sem nenhuma reassociação |
| `isolated_node` | o cabo CAN do módulo responsável se solta (o módulo segue ligado, com o Wi-Fi no ar): ele percebe o isolamento (6 s sem quadro algum e transmissões sem confirmação), libera o sensor e desliga o ponto de acesso, não pede eleição, o líder entrega o vínculo a outro módulo e a telemetria volta por ele em cerca de 8 s; na volta do cabo o módulo sai do isolamento sem disputar o sensor nem refazer a eleição, e os quadros que ficaram na fila do controlador saem de uma vez |
| `selftest` | a identificação (`VERSION`, `PROBE_VERSION`, `PROBE_STATUS`) e o autoteste `SELFTEST`: laço interno do controlador CAN, contadores de erro e LEDs; cada defeito injetado (nada volta, bit trocado, controlador que não entra no modo laço, bus-off, contadores altos) aparece no resultado com a dica certa, o controlador sempre volta ao modo normal e, dentro da rede, o comando é recusado sem calar ninguém |
| `can_auth` | só existe na compilação com `IOT_CAN_AUTH=1` (seção 8 de [Segurança](seguranca.md)): o comando legítimo, do ensaio e do sensor, é obedecido; o comando forjado sem etiqueta, com etiqueta inventada ou repetido do que passou pelo cabo é descartado e contado, sem efeito na rede; a etiqueta sem comando é descartada; `CANAUTH SYNC` acerta um módulo à frente da Probe; a atualidade sobrevive ao reinício do módulo. Um controle negativo, compilado sem a verificação, mostra que o comando forjado passaria |
| `acoustic_sensor` | o sensor acústico de ponta a ponta |
| `panel` | o painel: retrato, marcas, alerta e ciência, sono na bateria e relógio da rede |

Uma verificação que falha encerra o cenário com erro e informa o instante simulado. `Codigo/scripts/record_network_scenarios.sh` grava a saída de cada cenário em `Front/replays/`, que os testes da estação usam e que a estação reproduz com `--replay`.

**O que a bancada virtual não reproduz:** a temporização real do rádio (além de um atraso fixo), a convivência entre BLE e Wi-Fi, perda de quadros por ruído, a comunicação SPI com o MCP2515 e a latência da serial. Ela verifica a lógica do firmware e a integração com a estação; o comportamento do rádio só a bancada física mostra.

`Codigo/node-can/test/host/sim_wireless_link.cpp` é uma simulação menor: o canal de dados de um único módulo, com o mesmo código fazendo o papel de módulo e de Probe, e um sensor simulado. Cobre a sessão, a telemetria, o espectro de 64 e 256 faixas, a configuração, as recusas, as repetições, as mensagens adulteradas, a expiração e a desassociação.

## 4. Laboratório virtual

O laboratório liga a bancada virtual à estação, em tempo real, como se fosse a bancada física. Os arquivos ficam em `Simulacao/`.

**Na estação.** `--sim` inicia a bancada e conecta a estação a uma Probe virtual, pelo mesmo protocolo de texto da Probe real:

```bash
./Codigo/scripts/build_network_sim.sh /tmp/ioc_sim_network
cd Front && .venv/bin/python -m pico_tui --sim --sim-binary /tmp/ioc_sim_network [--sim-record /tmp/sessao.log]
```

Com a bancada rodando, `python3 Simulacao/sim_ctl.py` muda a topologia: `STATUS`, `PAUSE`, `RESUME`, `NODE 4 ADD`, ou `PROBE <comando>` para mandar um comando como se fosse a estação.

**Simulation Studio.** Uma página local, no navegador, com a topologia, os instrumentos, um inspetor de cada equipamento e experimentos reprodutíveis:

```bash
python3 Simulacao/studio/server.py --binary /tmp/ioc_sim_network --browser     # http://127.0.0.1:8765
```

O Studio cria a porta virtual (`PORT=/dev/pts/N`), à qual a estação se conecta com `--port /dev/pts/N --mode probe`. `--attach` liga o Studio a uma estação já aberta com `--sim`. Os experimentos prontos estão em `Simulacao/studio/experiments/` (linha de base, queda do líder, reassociação, orçamento de processamento) e rodam também pela linha de comando, com `Simulacao/studio/experiments.py`. O servidor só aceita conexões da própria máquina e exige um token de sessão; não o exponha na rede.

**Sensor real no laboratório.** Um sensor gravado com o firmware real participa da bancada virtual pela USB, no lugar do rádio:

```bash
Front/.venv/bin/python Simulacao/usb_sensor_bridge.py --port /dev/ttyACM0 --node 1 --rssi -55
```

A ponte não contorna a autorização do firmware: use `--passive` se o sensor já envia telemetria, ou `--token-file <arquivo>` com o token de manutenção. Associe o sensor pela estação (`F7`) ou com `sim_ctl.py PROBE WIRELESS BIND 1 0x<UUID> AUTO`. A ponte leva também o espectro do sensor acústico físico. `Simulacao/usb_panel_bridge.py` faz o mesmo com um painel.

**Tudo de uma vez.** `./Simulacao/iniciar_laboratorio.py` abre Studio, estação e ponte USB em três terminais e encerra tudo com `stop`. Opções: `status`, `--rebuild`, `--no-usb`, `--no-browser`, `--dry-run`, `--device /dev/serial/by-id/...`.

## 5. Integração contínua

A cada envio, `.github/workflows/ci.yml` roda os testes da estação, os testes nativos (com a bancada virtual), os testes do laboratório e da ferramenta de pesquisa e a compilação de todos os firmwares: sensor de vibração, sensor acústico (Pico W e Pico 2 W), Probe 00 e módulos, painel e leitor da documentação. A lista dos trabalhos está em [Integração](integracao.md#8-testes-e-integração-contínua). Fora desse fluxo, `.github/workflows/bancada-fisica.yml` roda só quando alguém pede, num computador do laboratório com as placas ligadas por USB (seção 6, «Bancada física na integração contínua»).

## 6. Bancada física

### O que já foi observado

**08/10/2026**, primeira sessão do canal de dados (Probe 00, quatro módulos e um sensor de vibração): funcionaram a descoberta e a associação pela estação, a oferta autenticada, a sessão autenticada com um dos módulos, a telemetria e o espectro até a estação e uma reassociação automática, sem intervenção do operador. A sessão com outro módulo não chegou a abrir. A sessão também revelou três problemas, já tratados: a alimentação por power bank, que não sustentava os módulos e foi resolvida com um hub USB com fonte própria; credenciais antigas sem a chave mestra; e a ida e volta exibida como 0,0 ms. O registro completo está em [`historico/bancada-fisica-2026-10-08.md`](historico/bancada-fisica-2026-10-08.md).

Depois dessa sessão, a abertura do canal de dados foi corrigida: o nonce passou a valer pela tentativa inteira, a escuta BLE cede rádio durante a entrada do sensor e a oferta não é mais reiniciada ([Desafios](desafios.md#abertura-do-canal-de-dados-wireless)). A correção foi verificada na bancada virtual, com atraso de rádio, e ainda precisa ser confirmada no hardware.

### O que ainda precisa ser conferido

| Item | O que observar |
|---|---|
| abertura do canal de dados | tempo do vínculo à sessão, com um e com dois sensores no mesmo módulo, pela seção Criação do canal de dados |
| primeiro acesso com reassociação automática | o mesmo tempo da política manual, sem mensagens "Reassociação automática" (linhas `WIRELESS_FAILOVER` da Probe) antes da sessão; se o módulo responsável aparecer como `STALE` durante a criação, é o BLE dele surdo, e o líder deve esperar (CH-078); se a sessão não abrir em 2 min, o líder tenta outro módulo |
| rádio do módulo | se a entrada no Wi-Fi ainda falha com a escuta BLE em 25 % (se falhar, reduzir `SCAN_WINDOW_QUIET` em `wireless_discovery.cpp`) |
| espectro de 256 faixas | os quatro blocos completos; se faltarem, aumentar a pausa entre blocos em `wireless_link.cpp` |
| medição e análise | ocupação, período do sinal de presença, contadores de erro |
| vigilância do líder | eleição sem a Probe em cerca de 14 s |
| reassociação | tempos com o responsável desligado e com o sensor fora de alcance |
| ensaios de falha | modo somente escuta do MCP2515, volta do ponto de acesso, carga que a Probe sustenta |
| sensor acústico | calibração com calibrador de 94 dB, ocupação do processamento nas duas placas, níveis e doses contra um medidor de referência |
| painel CYD | cores, toque, LED e som, tempo de redesenho, hora vinda da rede |
| leitor da documentação | fontes e cores na tela, botões, sono e despertar, desligamento e volta à página sem redesenho, CardKB |
| relógio da rede | ajuste ao conectar a estação; hora no painel; pedidos sem resposta sem terminal |
| isolamento do módulo | CAN_H solto de um módulo com sensor: `[ISOLADO]` em cerca de 6 s, sensor liberado e reassociado em cerca de 8 s, volta do cabo sem disputar o sensor; quadros antigos que ficaram na fila do MCP2515 aparecem na volta? (RT 71) |
| autoteste | `SELFTEST` em cada placa, fora da rede: laço interno do MCP2515 com os quatro quadros, contadores de erro e LEDs (RT 86) |
| medição da alimentação | tensão pelo divisor no ADC1 contra um multímetro e corrente pelo INA219 contra um medidor, em repouso e em carga: só o patamar, não o pico (RT 87) |
| autenticação dos comandos CAN | com `--auth-can`, os comandos `0x304` e `0x305` passam, o comando sem etiqueta é descartado, a etiqueta chega a tempo com o barramento carregado e o contador sobrevive ao reinício (RT 88) |
| esperas do sensor | tempo da decisão do líder até a sessão no módulo novo, com os padrões, contra a bancada virtual, antes de qualquer mudança (RT 89) |
| coleta de exemplos sonoros | `:rotulo` com sons reais diante do sensor acústico: quantos eventos viram linha em `exemplos_sonoros.csv` (RT 49) |
| alimentação e LEDs | `ENERGIA` em cada módulo depois de horas ligado (contagens em zero); medida entre 3V3 e GND com mola de aterramento, na Probe 00, no módulo sem sensor e no módulo com sensor, com o ponto de acesso a 11 e a 19,5 dBm; se o sensor ainda entra no Wi-Fi do módulo a 11 dBm, na distância de uso |

### Roteiro

Grave a Probe 00 e todos os módulos com a mesma versão.

1. **Credenciais.** `provision_sensor_security.sh --check`. No monitor de cada módulo, `[WLINK] LINK_KEY=PROVISIONED`.
2. **Rede.** Na estação, todos os módulos e um líder. Em Métricas (`F8`), ocupação da ordem de 1 %, período de `0x100` de 1000 ms e contadores de erro em zero.
3. **Associação.** Associe um sensor em Sensores sem fio (`F7`), com a reassociação automática (padrão), e acompanhe a Criação do canal de dados até a sessão: não deve aparecer nenhuma mensagem "Reassociação automática". Repita com a manual e compare os tempos. No console do sensor, `NET` mostra `STATE=BOUND`.
4. **Dados.** Telemetria na tela do sensor; espectro com 64 e com 256 faixas; mude a configuração e confirme "Aplicada pelo sensor".
5. **Perda e retorno.** Desligue o sensor: em cerca de 10 s o canal volta à oferta; religue e confirme a volta sem nova associação.
6. **Desassociação.** O módulo desliga o ponto de acesso e o sensor volta a `STATE=DISABLED`.
7. **Chaves por sensor.** Grave o sensor com `--sensor <UUID>` (`NET` mostra `LINK_KEYS=DEVICE`). Um segundo sensor gravado com as chaves do primeiro deve mostrar `LINK_KEYS=UUID_MISMATCH` e não abrir sessão.
8. **Vigilância.** Desligue a Probe 00 e depois o líder: os outros módulos elegem um novo em cerca de 14 s (no monitor: `[VIGILANCIA]`).
9. **Ensaios.** Em Ensaios (`F9`): queda do líder, queda de um módulo com um sensor em reassociação automática e carga de alta prioridade de 40 % por 15 s. Depois, *Limpar falha* no módulo.
10. **Exportação.** `x` em Métricas ou Ensaios; guarde os arquivos de `exports/` junto do registro da sessão.

O roteiro completo, com 89 requisitos de teste (RT) em 12 fases, da compilação à longa duração, está em [`bancada-fisica/roteiro-de-testes.md`](bancada-fisica/roteiro-de-testes.md). Cada RT traz os comandos, as saídas esperadas, o que fazer quando sai outra coisa, a condição de aceite e o que anotar. Ele é versionado com o repositório e acompanha o firmware: ao mudar um comando ou uma mensagem, corrija o RT que a cita.

### Registro da sessão

Cada sessão vira uma entrada em `historico/bancada-fisica-AAAA-MM-DD.md`, escrita a partir do [modelo de sessão](bancada-fisica/modelo-de-sessao.md) (identificação, montagem, resultado por RT, medidas contra a bancada virtual, o que funcionou, o que falhou, o que não foi validado, arquivos e mudanças pedidas ao repositório). Os problemas encontrados entram em [Desafios](desafios.md), com um número `CH-`.

### Verificação de fumaça depois de gravar

```bash
python3 Codigo/scripts/smoke_boot.py <probe|modulo|vibracao|acustico|painel|leitor> --porta <porta> [--no <n>] [--selftest] [--estrito]
```

Confere, logo depois de gravar, o que antes só aparecia no meio de um ensaio: a partida e os comandos de identificação, a chave do enlace (`LINK_KEY=MISSING` reprova), o microfone e o MPU-6050 dos sensores, o controlador CAN, e a versão e o commit contra o arquivo `VERSION` e o repositório (placas de versões diferentes na mesma captura reprovam). As placas ESP32 são reiniciadas ao abrir a porta; os sensores Pico respondem a `VERSION`, `STATUS` e `NET`. Com `--selftest` o módulo ou a Probe 00, fora da rede, roda o `SELFTEST CAN`. `--estrito` trata o aviso (commit `-dirty` ou desconhecido) como falha. A saída termina em `SMOKE RESULT=PASS|WARN|FAIL`, e o código de saída é 0 (aprovado), 1 (reprovado), 2 (uso incorreto) ou 3 (nenhuma linha recebida). `--arquivo <captura>` confere um texto já capturado, sem placa.

Foi testado com capturas de texto (parte delas tirada da bancada virtual) e confere que cada linha esperada é escrita pelo código-fonte do firmware, mas nunca com uma porta real: a primeira rodada numa placa é a validação dele.

### Comparar a gravação física com a bancada virtual

```bash
python3 Simulacao/comparar_bancada.py logs/gravacao_<data>_<sessão>.log --cenario owner_failover
```

A estação grava a sessão com `--record-session`, no mesmo formato (`@<ms> <linha>`) das gravações de `Front/replays/`. O script mede, nas duas, a troca de líder (`troca_de_lider_ms`: do último sinal de presença do líder que saiu ao primeiro do novo), a reassociação (`reassociacao_ms`: da decisão `WIRELESS_FAILOVER` até o sensor voltar a ter sessão `SECURE` no módulo escolhido) e a abertura do canal (`abertura_do_canal_ms`: da primeira associação à primeira sessão `SECURE`), e as põe lado a lado. Uma medida está dentro da tolerância se a diferença for de no máximo 1500 ms (a latência da USB e do laço do firmware) ou 25 % do valor da bancada virtual, o que for maior (`--tolerancia-absoluta-ms`, `--tolerancia-relativa`). As medidas são comparadas na ordem em que aparecem. A saída termina em `COMPARACAO RESULT=OK|FORA|SEM_DADOS`; código de saída 0 (todas dentro), 1 (alguma fora, ou sem dado na gravação com `--exigir`), 2 (uso incorreto) e 3 (nenhuma medida comparável). `--bancada <arquivo>` usa outra gravação virtual como referência, e `--json` escreve o resultado em JSON.

Um resultado `FORA` não é uma falha do firmware: diz que o modelo da bancada virtual não descreve a montagem, e o tempo medido na física é o que vale.

### Esperas do sensor

Duas esperas do sensor pesam na reassociação: 10 s sem `CMD_PING` até largar a sessão e 15 s, depois de entrar no ponto de acesso de um módulo, até seguir a oferta de outro. Elas são parâmetros de compilação (`build_pico.sh --espera-sessao <ms>` e `--espera-oferta <ms>`, valem para o sensor de vibração e para o acústico), com os padrões de sempre, e só devem mudar com medição na bancada física: o RT 89 descreve o procedimento. A bancada virtual dá a projeção do ganho (cenário `owner_failover`, 5,2 s do corte do módulo até a decisão do líder, em todas as linhas):

| Sessão / oferta | decisão → sessão (`reassociacao_ms`) | falha → sessão |
|---|---|---|
| 10 s / 15 s (padrão) | 19,1 s | 24,3 s |
| 10 s / 5 s | 9,2 s | 14,4 s |
| 7 s / 15 s | 16,1 s | 21,3 s |
| 7 s / 5 s | 6,2 s | 11,4 s |
| 7 s / 3 s | 4,1 s | 9,3 s |
| 7 s / 1 s | 2,3 s | 7,5 s |

A espera da sessão tem piso de 7 s (mais de dois `CMD_PING` perdidos, um a cada 3 s) e a da oferta vai de 1 s a 120 s; fora disso a compilação falha. A espera da oferta existe para limitar o efeito de uma oferta antiga repetida: curta demais, o sensor pode alternar entre módulos. Para repetir a projeção: `IOC_SIM_CXXFLAGS="-DIOT_SESSION_TIMEOUT_MS=7000 -DIOT_OFFER_SWITCH_HOLD_MS=5000" ./Codigo/scripts/build_network_sim.sh /tmp/sim_espera`, e `comparar_bancada.py` sobre a saída de `owner_failover`.

### Bancada física na integração contínua

`.github/workflows/bancada-fisica.yml` roda só por pedido (Actions, *Bancada física*, *Run workflow*), num computador que o laboratório registra como runner com o rótulo `bancada-fisica` e que tem a Probe 00, os módulos e os sensores ligados por USB. Os runners do GitHub não enxergam as placas e nunca pegam esse trabalho. O fluxo **não grava nada nas placas**: roda `smoke_boot.py` na Probe 00, em cada módulo informado (`NÓ=PORTA`, com `SELFTEST` se pedido) e em cada sensor informado (`TIPO=PORTA`, com `vibracao` ou `acustico`), sempre com `--estrito`, e, se for dada uma gravação da sessão, a compara com a bancada virtual no cenário escolhido. Os resultados saem como artefato `bancada-fisica`. Um teste do repositório confere a estrutura do fluxo, mas ele só pôde ser lido, não executado: a primeira execução num runner com placas é a validação dele.
