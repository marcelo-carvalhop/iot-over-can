# Metodologia de pesquisa

Este documento descreve como o projeto passa de protótipo que funciona a **instrumento para responder a uma pergunta comparativa em condições controladas**. Ele é o esqueleto do capítulo de metodologia: a pergunta, o desenho experimental, as métricas com definição operacional e domínio de relógio, o protocolo de injeção de falhas, a estatística e o que torna os resultados reprodutíveis. As ferramentas estão em [`Pesquisa/`](../../Pesquisa/README.md); a instrumentação do firmware, em `Codigo/node-can/include/pesquisa.h`.

## 1. Pergunta de pesquisa

> A integração de sensores sem fio com sessão autenticada (HMAC-SHA256) a uma rede CAN clássica, por meio de Nodes que fazem a ponte Wi-Fi → CAN, impõe ao tráfego determinístico do barramento um custo mensurável e limitado. Quanto custa, em latência, variação e perda, em função da carga de fundo e da prioridade dela, e com que rapidez a rede percebe e recupera falhas do enlace?

### 1.1 O que muda em relação à proposta de "acoplamento forte × fraco"

A sugestão de comparar "Pico 2 W lendo o CAN diretamente" com "ESP32 supervisor sem acoplamento ao barramento" não corresponde à arquitetura deste projeto. Aqui o sensor (Pico W) **nunca** está no barramento: quem está no CAN é o Node ESP32 com MCP2515, que mantém a sessão autenticada com o sensor e encaminha a telemetria. Inventar o fator exigiria hardware novo (um Pico 2 W com transceptor CAN atuando como Node). Fica como trabalho futuro (seção 12).

A comparação que a arquitetura oferece, sem hardware novo, é a **integração** como fator: o barramento com o plano de dados sem fio desligado (os sensores mantêm a sessão, mas os Nodes não encaminham ao CAN) contra o barramento com a integração ligada, sob carga de fundo controlada e de prioridade controlada.

| Fator | Níveis | Como é controlado |
|---|---|---|
| Integração sem fio | `off`, `on` | comando `TELEMETRY STATE=OFF/ON` aos Nodes, pela Probe 00 |
| Carga de fundo | 0, 10, 50, 80 % | gerador da Probe 00 (`FAULT LOAD`), com semente |
| Prioridade da carga | `HIGH` (0x010), `MID` (0x1F0), `LOW` (0x7EF) | `HIGH` vence todo o tráfego; `MID` perde para eleição, controle e sinal de presença e vence o tráfego dos sensores; `LOW` perde para tudo |
| Falha injetada | sensor desligado, queda do Node responsável, queda do ponto de acesso, sessões descartadas | bancada virtual (instante exato) ou comandos `FAULT` da Probe 00 |
| Parâmetros da máquina de estados (sensibilidade) | `L` (STALE), `H` (histerese) | compilação (`-D IOT_ASSOC_STALE_MS=...`) |

## 2. Desenho experimental

Desenho fatorial completo por bloco, com **30 repetições por condição**, sementes por execução e **ordem de execução sorteada** (a mesma semente base gera sempre o mesmo plano). A ordem aleatória impede que a deriva do equipamento ao longo de uma campanha (temperatura, bateria, interferência) se confunda com o efeito de um fator.

| Arquivo | Bancada | Condições | Execuções |
|---|---|---|---|
| `Pesquisa/desenhos/bancada_virtual.json` | virtual | 20 de desempenho (carga × prioridade × integração) e 8 de falhas (falha × carga) | 840 |
| `Pesquisa/desenhos/bancada_fisica.json` | física | 10 de desempenho, 3 de falhas comandadas e o sensor desligado à mão | 420 (cerca de 12 h) |
| `Pesquisa/desenhos/sensibilidade_virtual.json` | virtual | `L` ∈ {10, 15, 25 s} × `H` ∈ {0, 5 s}, com falha e com carga alta | 360 |
| `Pesquisa/desenhos/demonstracao.json` | virtual | caminho inteiro em minutos (usado nos testes) | 16 |

Cada execução segue o mesmo protocolo: rede formada, sensor associado ao Node 1 com reassociação automática, aquecimento (`warmup_s`), marca de início da janela, carga de fundo durante toda a janela, trens de eco (seção 3.2), falha no instante `fault_at_s` da janela quando houver, marca de fim. Tudo o que é medido está dentro da janela.

Dentro de cada condição de desempenho há dois controles naturais: a **linha de base** (`iot=off`, que mede o tráfego nativo com a mesma carga) e a **operação normal** (`iot=on`). O bloco de falhas mede detecção e recuperação; a contagem de transições espúrias nas execuções sem falha mede a correção da máquina de estados em operação normal.

## 3. Métricas

### 3.1 Definições

| Métrica | Definição operacional | Unidade | Relógio |
|---|---|---|---|
| Ida e volta no CAN (`eco_local_us`) | do instante em que a Probe entrega o comando `0x304` (opcode ECHO, caminho 0) ao controlador até a confirmação remontada no identificador de dados do Node (`0x380+N`) | µs | Probe |
| Ida e volta até o sensor (`eco_sensor_us`) | idem, com o Node enviando um PING ao sensor pela sessão e confirmando no PONG | µs | Probe |
| Parte Wi-Fi (`eco_wifi_us`) | PING → PONG medido pelo Node, informado no detalhe da confirmação | µs | Node |
| Parte CAN do eco ao sensor (`eco_can_us`) | `eco_sensor_us − eco_wifi_us` | µs | intervalos de dois relógios |
| Intervalo do sinal de presença (`hb_intervalo_us`) | diferença entre os instantes em que a Probe trata dois sinais de presença consecutivos do líder; a variação é o desvio-padrão e a diferença p99 − p50 | µs | Probe |
| Intervalo da telemetria (`tel_intervalo_us`) | diferença entre duas amostras remontadas do mesmo sensor | µs | Probe |
| Perda de telemetria (`tel_perda`) | saltos na sequência `SEQ` atribuída pelo Node (amostra encaminhada e não remontada), com IC de Wilson | proporção | contagem |
| Ocupação do barramento | média das janelas de 1 s de `BUS_STATS` e a carga efetivamente gerada (`achieved_pct`) | % | Probe |
| Verificação HMAC por datagrama (`cripto_verificacao_us`) | média na janela de 5 s do tempo de `ioc_env_verify` dos dados recebidos | µs | Node |
| Selo HMAC (`cripto_selo_us`) | média do tempo de `ioc_env_seal` dos datagramas enviados ao sensor | µs | Node |
| Handshake (`cripto_handshake_us`) | CPU da última autenticação: verificação do HELLO, chave de sessão, selo do CHALLENGE, verificação do CONFIRM | µs | Node |
| Ponte Wi-Fi → CAN (`ponte_media_us`, `ponte_max_us`) | do instante em que o laço do Node lê o datagrama de telemetria ao último segmento entregue ao controlador CAN | µs | Node |
| Perda da sessão percebida (`deteccao_sessao_ms`) | da falha até o primeiro relatório com a sessão encerrada | ms | referência |
| `STALE` e `LOST` (`deteccao_stale_ms`, `deteccao_lost_ms`) | da falha até a transição publicada pelo Node | ms | referência |
| Decisão de reassociação | da falha até `WIRELESS_FAILOVER` do líder | ms | referência |
| Sessão restabelecida | da falha até a primeira sessão autenticada de novo (em qualquer Node) | ms | referência |
| Telemetria interrompida / retomada | maior intervalo sem telemetria do sensor a partir da falha; e o instante em que ele termina, contado da falha | ms | referência |
| Transições espúrias | transições para `STALE`/`LOST` e quedas de sessão em execuções sem falha | contagem | — |
| Custo isolado da criptografia (`BENCH`) | cada operação cronometrada 200 vezes na placa: HMAC de 16 a 1024 bytes, selo e verificação de um datagrama de 96 bytes, handshake de cada lado | µs | Probe, Node, sensor, computador |

Para toda distribuição o relatório traz média, mediana, p95, p99 e máximo, e IC 95 %.

### 3.2 Por que o eco

O MCP2515 não registra o instante de chegada e o Node não compartilha relógio com a Probe; uma latência "do CAN ao Wi-Fi" medida com instantes de dois relógios não seria defensável. O eco mede **tudo num relógio só**: a Probe envia um comando ao plano de dados e cronometra a confirmação. O caminho `LOCAL` percorre exatamente o caminho CAN da integração (o comando em `0x304` e a resposta segmentada no identificador de dados do Node, os de menor prioridade do projeto); o caminho `SENSOR` acrescenta a ida e volta Wi-Fi, que o Node mede no relógio dele. Subtrair **intervalos** de relógios diferentes é legítimo, porque a deriva entre cristais é de partes por milhão. Subtrair **instantes**, não.

Sem sensor associado (linha de base), o Node responde ao eco `LOCAL` pelo mesmo caminho, com estado `UNKNOWN`: a medida continua válida para o barramento.

## 4. Relógios e carimbos de tempo

| Domínio | Fonte | Onde aparece | Resolução prática |
|---|---|---|---|
| Probe 00 | `esp_timer_get_time()` (64 bits, µs) | `t_us=` em `HEARTBEAT`, `TEL` (`T_US`), `ACK` (`RTT_US`), `FAULT_*`, `WIRELESS_*`, `ECHO`, `BUS_STATS` | latência do laço principal, da ordem de 1 ms |
| Node | o mesmo, em cada placa | intervalos (eco Wi-Fi, criptografia, ponte); nunca instantes | 1 µs (a criptografia), laço (a ponte) |
| Sensor | `time_us_64()` | `BENCH` | 1 µs |
| Referência | bancada virtual: relógio da simulação; física: relógio monotônico do computador | `t_ref_ms` no registro bruto | 1 ms; na física, mais a latência da serial |

O carimbo da Probe é tomado quando o laço principal trata o quadro, não quando ele chegou ao barramento. Efeitos de arbitragem abaixo de cerca de 1 ms não são resolvidos por esta medida; o limite analítico (pior tempo de resposta, Davis et al., 2007) está em `Front/pico_tui/metrics/rta.py` e deve acompanhar as distribuições medidas. Um controlador com carimbo de tempo no hardware (MCP2518FD) eliminaria essa limitação.

## 5. Carga de fundo determinística

`FAULT LOAD <pct> <s> [HIGH|MID|LOW|MIX] [semente]`. O gerador da Probe dosa os bits por crédito, na taxa pedida, e informa a carga obtida. Com semente, os seis últimos bytes de cada quadro saem de um xorshift32, o que varia o preenchimento de bits como num tráfego real. Em `MIX` a classe de prioridade de cada quadro também é sorteada. A Probe publica no fim o `seq_hash` (FNV-1a sobre identificador, DLC e dados de cada quadro gerado). A análise recalcula o valor esperado a partir da semente e do número de quadros (`Pesquisa/iocpesquisa/carga.py`) e acusa qualquer divergência: o barramento recebeu exatamente a carga declarada no plano.

## 6. Protocolo de injeção de falhas

| Falha | Como é provocada | Instante de referência |
|---|---|---|
| Sensor desligado (`sensor_off`) | bancada virtual: o sensor para; física: o operador desliga e tecla Enter no mesmo instante (o coletor registra) | marca `verdade=SENSOR_OFF`/`SENSOR_ON` |
| Queda do Node responsável (`owner_power`) | `FAULT POWER_CYCLE 1 <s>` | `FAULT_EVENT ... state=STARTED` |
| Queda do ponto de acesso (`ap_outage`) | `FAULT AP_OUTAGE 1 <s>` | idem; o fim é o fim do ensaio |
| Sessões descartadas (`drop_session`) | `FAULT DROP_SESSION 1` | idem |

Na bancada física, o tempo de reação do operador (da ordem de 200 a 300 ms) entra no erro do instante de referência do sensor desligado. É pequeno diante dos tempos medidos (10 a 60 s), mas deve ser declarado.

Valores de referência na bancada virtual, com os parâmetros padrão: a sessão cai em 10 s (`NET_SESSION_TIMEOUT_MS`), o vínculo fica `STALE` 15 s depois (`L`) e `LOST` aos 45 s sem o sensor; na queda do Node responsável, o líder decide a reassociação em cerca de 5 s e a telemetria volta em cerca de 24 s, limitada pela espera do sensor.

## 7. Custo da segurança

São três medidas complementares:

1. **No caminho, durante o experimento (Node)**: tempo de cada verificação e selo HMAC dos dados e CPU do handshake, publicados a cada 5 s (`WIRELESS_STATS`). Isola o custo atribuível à criptografia no caminho crítico, com a carga e o rádio reais.
2. **Isolado, em cada placa (`BENCH CRYPTO`)**: Probe e Nodes pela serial, Pico W pelo console USB, computador como referência. Mede a implementação usada pelo enlace (SHA-256 e HMAC em software, `ioc_sha256.c`), não um acelerador que o projeto não usa.
3. **Bytes no ar**: cada datagrama leva 8 bytes de cabeçalho e 8 de etiqueta (`IOC_ENV_OVERHEAD`); o custo em tempo de ar se calcula da taxa do enlace.

**Por que não há ablação com a autenticação desligada.** O protocolo não tem modo sem autenticação, e criar um seria produzir um firmware inseguro que poderia acabar em campo. A ablação também confundiria o efeito da criptografia com a variabilidade do rádio entre execuções. A medida direta (1) separa exatamente a parcela da criptografia; somada à (3), dá o que a ablação daria, com menos ruído. Se uma banca exigir a ablação, ela deve ser feita só na bancada virtual, onde não há risco, e reportada como estimativa.

## 8. Máquina de estados e parâmetros

O vínculo de cada sensor passa por `BOUND → ONLINE ↔ STALE → LOST`. Parâmetros (em `pesquisa.h`, ajustáveis na compilação):

| Símbolo | Define | Padrão | Papel |
|---|---|---|---|
| `T` | `IOT_STREAM_PERIOD_MS` | 1000 ms | período de encaminhamento da telemetria ao CAN |
| `L` | `IOT_ASSOC_STALE_MS` | 15 000 ms | sensor não ouvido por `L` → `STALE` |
| — | `IOT_ASSOC_LOST_MS` | 45 000 ms | sensor não ouvido por esse tempo → `LOST` |
| `H` | `IOT_ASSOC_HYSTERESIS_MS` | 0 | tempo mínimo em `STALE`/`LOST` antes de voltar a `ONLINE` |

A Probe publica os valores da própria compilação na partida (`[GW] CONFIG ...`; o comando `CONFIG` repete a linha). O coletor confere que a Probe roda a compilação pedida pelo desenho e recusa a execução se não rodar. Na bancada física, os Nodes precisam ser gravados com os mesmos parâmetros: a linha só comprova os da Probe. Cada transição aparece no monitor do Node com o estado anterior, o tempo nele e a idade da última observação (`prev=`, `in_prev_ms=`, `age_ms=`).

## 9. Estatística

- **Unidade de replicação**: a repetição. As comparações entre condições usam um valor por repetição (média, p99 ou variação daquela repetição). Juntar as amostras de todas as repetições serve para descrever a distribuição (percentis), não para testar diferenças.
- **Intervalos**: IC 95 % da média entre repetições pela distribuição t; proporções (perdas) com o intervalo de Wilson; bootstrap percentílico com semente fixa quando a estatística não é a média.
- **Tamanho de efeito**: g de Hedges com IC aproximado e, porque latências são assimétricas e de cauda longa, o delta de Cliff (não paramétrico), com a magnitude de Romano et al. (2006).
- **Testes**: Mann-Whitney bicaudal, com valor-p ajustado por Holm dentro de cada métrica, porque há muitas comparações.
- **Repetições**: 30 por condição. Com efeitos grandes, como os da carga alta, menos bastariam; com efeitos pequenos, como o da integração sem carga, 30 é o mínimo razoável para ver a cauda (p99).

## 10. Reprodutibilidade

| O quê | Onde |
|---|---|
| Desenho, plano (execuções, sementes, ordem) e hash do desenho | `desenho.json`, `plano.json` |
| Ambiente: commit, alterações não gravadas, versões fixadas (PlatformIO, bibliotecas, placa do Pico), compiladores, parâmetros das fontes, hash das fontes do firmware | `ambiente.json` |
| Por execução: parâmetros, semente, linha `CONFIG` da Probe, firmware, hash de configuração (parâmetros sem semente + compilação + fontes) | `execucoes/<id>/manifesto.json` |
| Registro bruto (JSONL, esquema em `Pesquisa/iocpesquisa/coleta.py`) | `execucoes/<id>/traco.jsonl` |
| Tabelas, comparações e figuras | `relatorio/` |

`python -m iocpesquisa verificar` confere o plano contra o desenho, a configuração idêntica entre as repetições de cada condição, a sequência de carga contra a semente e se a coleta foi feita com alterações não gravadas no código. Antes de uma campanha que vá para o texto, grave um commit.

**Grau de reprodutibilidade.** O caminho da análise (registros brutos → tabelas e figuras) é automático. Na bancada virtual a coleta também é: a mesma semente produz o mesmo registro, linha a linha (há teste para isso). Na bancada física, a coleta exige o equipamento descrito abaixo; o plano, as sementes e a ordem são os mesmos, mas o rádio não se repete.

Equipamento da bancada física: Probe 00 e três Nodes ESP32 (`esp32dev`) com MCP2515 a 500 kbit/s, barramento terminado nas duas pontas; um sensor Pico W (vibração) associado ao Node 1 com reassociação automática e ao alcance do Node 2; firmware de todas as placas do mesmo commit.

## 11. Ameaças à validade

- **Bancada e veículo**: barramento curto, de bancada; tráfego de fundo sintético, sem a distribuição de identificadores de um veículo real.
- **Bancada virtual**: executa o firmware real dos Nodes, mas não modela o tempo de CPU (a criptografia tem custo zero ali), o rádio, a coexistência BLE/Wi-Fi nem a temporização do MCP2515 por SPI. Serve para validar o método e a lógica; números de tempo para o texto vêm da bancada física.
- **Resolução**: carimbos com a latência do laço (cerca de 1 ms). Diferenças abaixo disso não são afirmadas; o limite analítico acompanha.
- **Escala**: um ou dois sensores, três Nodes. Os limites práticos do projeto (quatro sensores por ponto de acesso e quatro vínculos por Node) e a análise de capacidade (`rta.py`) indicam até onde se pode extrapolar.
- **Instante de referência humano** no sensor desligado da bancada física (seção 6).
- **Prioridade da carga**: as três classes são pontos de uma escala contínua; resultados intermediários exigem outros identificadores.

## 12. Trabalho futuro

- **Acoplamento forte**: um Pico 2 W com transceptor CAN (MCP2518FD por SPI, que também dá carimbo de tempo no hardware) atuando como Node, para comparar com a ponte pelo ESP32.
- **Carimbo no hardware**: o MCP2518FD na Probe permitiria medir a arbitragem abaixo de 1 ms.
- **Ponte com o SocketCAN** (`vcan`), para observar a bancada virtual com `candump` e Wireshark (ver `../arquitetura/metricas-e-ensaios.md`, seção 8.1).

## 13. Mapeamento para os capítulos

| Capítulo | O que vem daqui |
|---|---|
| Introdução | a pergunta da seção 1 e a lacuna: sensores sem fio autenticados na fronteira de uma rede CAN legada |
| Trabalhos relacionados | análise de tempo de resposta do CAN (Tindell 1995; Davis et al. 2007), custo de autenticação em CAN e reprodutibilidade de artefatos de sistemas embarcados |
| Arquitetura | `Documentacao/arquitetura/`, escrito como decisões e compromissos |
| Metodologia | este documento |
| Resultados | `relatorio/` de cada campanha: tabelas com IC, efeitos e figuras |
| Discussão | seção 11 e o que os resultados não mostram |
