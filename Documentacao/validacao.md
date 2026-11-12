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
| compilação | todos os firmwares para as placas reais | idem |
| bancada física | comportamento com o hardware: rádio, alimentação, tempos reais | em sessões de bancada |

## 2. Testes automatizados

```bash
./Codigo/scripts/test_native_firmware.sh          # bibliotecas, fuzz, sensor acústico, simulações, bancada virtual e leitor
./Codigo/scripts/test_tui.sh                      # estação de operação
python3 -m unittest discover -s Simulacao/tests   # laboratório virtual
python3 -m unittest discover -s Pesquisa/tests    # ferramenta de pesquisa
```

`test_native_firmware.sh` precisa de `cc` e `c++` e executa, em ordem: a validação de configuração do sensor; os testes de `ioc_link` (criptografia, canal de dados, segmentação, relatório acústico, custo da criptografia); a parte de rede de `ioc_link` (duração de quadro, mensagem autenticada, segmentação em quadros maiores, reassociação, tabela de vínculos, relógio); os testes do painel; o fuzz dos decodificadores; o processamento do sensor acústico; a simulação do canal de dados; a bancada virtual com todos os cenários; e o leitor da documentação.

Os testes da estação percorrem as telas em 48×18, 60×24, 80×24, 100×30 e 160×48 para garantir que nada seja cortado, conferem a biblioteca C contra implementações independentes em Python e reproduzem o exemplo publicado da análise de tempo de resposta. Um deles refaz as gravações da bancada virtual e as compara com as versionadas. Outros cobrem a identificação do operador: senhas e cadastro (permissões, formato, adulteração), bloqueio de tentativas, expiração por inatividade, perfis, os três modos, a gravação do autor de cada ação no histórico, a atribuição da resposta de uma associação a quem a pediu, a ausência de segredos nos registros, a migração de bancos antigos e a detecção de registros de auditoria editados ou apagados.

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
| `failover_retry` | o módulo escolhido perde o sensor antes de assumir; o líder escolhe outro |
| `failover_dead_target` | o módulo escolhido sai do ar; o responsável anterior mantém o vínculo |
| `restart_keeps_binding` | o módulo reinicia e retoma da memória o vínculo gravado |
| `handshake_slow_radio` | dois sensores no mesmo módulo com 3,6 s de atraso de rádio; cada sessão abre em cerca de 5 s |
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

A cada envio, `.github/workflows/ci.yml` roda os testes da estação, os testes nativos (com a bancada virtual), os testes do laboratório e da ferramenta de pesquisa e a compilação de todos os firmwares: sensor de vibração, sensor acústico (Pico W e Pico 2 W), Probe 00 e módulos, painel e leitor da documentação. A lista dos trabalhos está em [Integração](integracao.md#8-testes-e-integração-contínua).

## 6. Bancada física

### O que já foi observado

**08/10/2026**, primeira sessão do canal de dados (Probe 00, quatro módulos e um sensor de vibração): funcionaram a descoberta e a associação pela estação, a oferta autenticada, a sessão autenticada com um dos módulos, a telemetria e o espectro até a estação e uma reassociação automática, sem intervenção do operador. A sessão com outro módulo não chegou a abrir. A sessão também revelou três problemas, já tratados: a alimentação por power bank, que não sustentava os módulos e foi resolvida com um hub USB com fonte própria; credenciais antigas sem a chave mestra; e a ida e volta exibida como 0,0 ms. O registro completo está em [`historico/bancada-fisica-2026-10-08.md`](historico/bancada-fisica-2026-10-08.md).

Depois dessa sessão, a abertura do canal de dados foi corrigida: o nonce passou a valer pela tentativa inteira, a escuta BLE cede rádio durante a entrada do sensor e a oferta não é mais reiniciada ([Desafios](desafios.md#abertura-do-canal-de-dados-wireless)). A correção foi verificada na bancada virtual, com atraso de rádio, e ainda precisa ser confirmada no hardware.

### O que ainda precisa ser conferido

| Item | O que observar |
|---|---|
| abertura do canal de dados | tempo do vínculo à sessão, com um e com dois sensores no mesmo módulo, pela seção Criação do canal de dados |
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

### Roteiro

Grave a Probe 00 e todos os módulos com a mesma versão.

1. **Credenciais.** `provision_sensor_security.sh --check`. No monitor de cada módulo, `[WLINK] LINK_KEY=PROVISIONED`.
2. **Rede.** Na estação, todos os módulos e um líder. Em Métricas (`F8`), ocupação da ordem de 1 %, período de `0x100` de 1000 ms e contadores de erro em zero.
3. **Associação.** Associe um sensor em Sensores sem fio (`F7`) e acompanhe a Criação do canal de dados até a sessão. No console do sensor, `NET` mostra `STATE=BOUND`.
4. **Dados.** Telemetria na tela do sensor; espectro com 64 e com 256 faixas; mude a configuração e confirme "Aplicada pelo sensor".
5. **Perda e retorno.** Desligue o sensor: em cerca de 10 s o canal volta à oferta; religue e confirme a volta sem nova associação.
6. **Desassociação.** O módulo desliga o ponto de acesso e o sensor volta a `STATE=DISABLED`.
7. **Chaves por sensor.** Grave o sensor com `--sensor <UUID>` (`NET` mostra `LINK_KEYS=DEVICE`). Um segundo sensor gravado com as chaves do primeiro deve mostrar `LINK_KEYS=UUID_MISMATCH` e não abrir sessão.
8. **Vigilância.** Desligue a Probe 00 e depois o líder: os outros módulos elegem um novo em cerca de 14 s (no monitor: `[VIGILANCIA]`).
9. **Ensaios.** Em Ensaios (`F9`): queda do líder, queda de um módulo com um sensor em reassociação automática e carga de alta prioridade de 40 % por 15 s. Depois, *Limpar falha* no módulo.
10. **Exportação.** `x` em Métricas ou Ensaios; guarde os arquivos de `exports/` junto do registro da sessão.

Registre cada sessão em `historico/`, como a de 08/10/2026, e os problemas encontrados em [Desafios](desafios.md).
