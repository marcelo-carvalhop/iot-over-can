# Pesquisa: o projeto como instrumento de medição

Ferramentas para planejar, coletar e analisar experimentos com a rede, na bancada virtual ou na física. A metodologia (pergunta, métricas, relógios, estatística, ameaças à validade) está em [`Documentacao/pesquisa/metodologia.md`](../Documentacao/pesquisa/metodologia.md).

Só usam a biblioteca padrão do Python (3.11+). O `pyserial` só é necessário na bancada física (é o mesmo da TUI) e o `matplotlib` só para as figuras.

## Fluxo

```bash
cd Pesquisa
# 1. Plano: condições, execuções, sementes, ordem e tempo estimado
python3 -m iocpesquisa planejar desenhos/bancada_virtual.json

# 2. Coleta na bancada virtual (firmware real dos Nodes; ~10 min para 840 execuções)
../Codigo/scripts/build_network_sim.sh /tmp/sim_network
python3 -m iocpesquisa coletar desenhos/bancada_virtual.json --binario /tmp/sim_network -j 4

# 2'. Coleta na bancada física (Probe 00 na USB; sensores já associados)
../Front/.venv/bin/python -m iocpesquisa coletar desenhos/bancada_fisica.json --porta /dev/ttyUSB0

# 3. Custo da criptografia em cada placa (grava em resultados/<campanha>/bench)
python3 -m iocpesquisa bench --porta /dev/ttyUSB0 --placa probe --saida resultados/bancada_fisica
python3 -m iocpesquisa bench --porta /dev/ttyACM0 --placa sensor --saida resultados/bancada_fisica
python3 -m iocpesquisa bench --host --saida resultados/bancada_fisica

# 4. Análise e verificação
python3 -m iocpesquisa analisar resultados/bancada_virtual
python3 -m iocpesquisa verificar resultados/bancada_virtual
```

Uma campanha interrompida continua de onde parou: chamar `coletar` de novo executa só o que falta. `--limite N` faz no máximo N execuções por chamada (por exemplo, uma sessão de laboratório por dia).

## Saída

```text
resultados/<campanha>/
  desenho.json, plano.json        desenho e plano expandido (sementes, ordem, hash)
  ambiente.json                   commit, versões fixadas, parâmetros e hash das fontes do firmware
  execucoes/<id>/traco.jsonl      registro bruto (uma linha JSON por linha da Probe, marca ou comando)
  execucoes/<id>/manifesto.json   parâmetros, semente, linha CONFIG da Probe, hash de configuração
  execucoes/<id>/metricas.json    cache das métricas (apagável)
  bench/<placa>-<data>.jsonl      medições BENCH CRYPTO
  relatorio/relatorio.md          tabelas, comparações, proveniência e ressalvas
  relatorio/condicoes.csv         todas as métricas por condição
  relatorio/comparacoes.csv       efeitos (g de Hedges, delta de Cliff, Mann-Whitney com Holm)
  relatorio/figuras/*.png         cauda do eco (por carga e prioridade) e variação do sinal de presença
```

Um exemplo completo dessa saída (a campanha virtual de validação do método, com o que ela encontrou) está em [`Documentacao/pesquisa/validacao-virtual/`](../Documentacao/pesquisa/validacao-virtual/README.md).

O relatório inteiro sai dos registros brutos: apagar `relatorio/` e rodar `analisar` reproduz os mesmos números. As métricas de cada execução ficam em cache (`metricas.json`), calculadas em paralelo e refeitas sozinhas quando o código das métricas muda.

## Desenho

```json
{
  "nome": "minha_campanha",
  "bancada": "virtual",
  "semente_base": 20261029,
  "ordem_aleatoria": true,
  "blocos": [{
    "nome": "desempenho",
    "repeticoes": 30,
    "fatores": {"load_pct": [0, 50, 80], "load_prio": ["HIGH", "LOW"], "iot": ["off", "on"]},
    "fixos": {"sensors": 1, "warmup_s": 10, "duration_s": 60, "echo_n": 40, "echo_period_ms": 500, "fault": "none"},
    "excluir": [{"load_pct": 0, "load_prio": "LOW"}]
  }],
  "comparacoes": [{"fator": "iot", "base": "off"}, {"fator": "load_pct", "base": 0, "ignorar": ["load_prio"]}]
}
```

| Parâmetro | Significado |
|---|---|
| `load_pct`, `load_prio` | carga de fundo da Probe 00 (0 = sem carga; `HIGH`, `MID`, `LOW` ou `MIX`) |
| `iot` | `on`: os Nodes encaminham a telemetria ao CAN; `off`: sessão mantida, encaminhamento desligado |
| `sensors` | sensores associados (bancada virtual: 0 a 2) |
| `warmup_s`, `duration_s` | aquecimento e janela de medição |
| `echo_n`, `echo_period_ms` | trens de eco (primeiro `LOCAL`, depois `SENSOR`); `echo_period_ms` é a média dos intervalos |
| `echo_sampling` | `poisson` (padrão: intervalos exponenciais, sem entrar em fase com as atividades periódicas da rede) ou `periodic` |
| `echo_seed`, `phase_ms` | sorteados por repetição a partir da semente da execução: semente do eco e fase do início da janela (0 a 5 s). Podem ser fixados em `fixos` |
| `fault`, `fault_at_s`, `fault_s` | `none`, `sensor_off`, `owner_power`, `ap_outage`, `drop_session`; instante na janela e duração |
| `alvo`, `alvo_uuid` | sensor de referência (padrão: filho 1 do Node 1, o sensor A da bancada virtual) |
| `fw_assoc_stale_ms`, `fw_assoc_lost_ms`, `fw_assoc_hysteresis_ms`, `fw_stream_period_ms` | parâmetros de compilação: a bancada virtual é compilada para cada combinação; na física, a coleta confere a linha `CONFIG` da Probe |

## Na TUI

A tecla `/` abre a tela **Pesquisa**, fora da barra de teclas: eco LOCAL e SENSOR com média, IC 95 %, mediana, p95 e p99, variação do sinal de presença, ocupação e carga de fundo, custo da criptografia e da ponte nos Nodes, `BENCH CRYPTO` da Probe e os parâmetros da compilação. A tecla `k` abre o lançador de comandos, para enviar `ECHO`, `FAULT LOAD`, `CONFIG` ou `BENCH CRYPTO` sem sair da tela. Os números são da sessão e servem para acompanhar um experimento na bancada; a análise da campanha continua aqui. Detalhes em [`Documentacao/interface/tui.md`](../Documentacao/interface/tui.md).

## Comandos novos do firmware

| Placa | Comando | Saída |
|---|---|---|
| Probe 00 | `ECHO <node>.<filho> <n> <período_ms> [LOCAL\|SENSOR] [semente]`, `ECHO CANCEL` (com semente, amostragem de Poisson) | `[GW] ECHO_START ... sampling= seed=`, `[GW] ECHO ... rtt_us= wl_us= t_us=`, `[GW] ECHO_DONE ...` |
| Probe 00 | `FAULT LOAD <pct> <s> [HIGH\|MID\|LOW\|MIX] [semente]` | `FAULT_EVENT ... seed= seq_hash= t_us=` |
| Probe 00 e Nodes | `CONFIG` | `[GW] CONFIG` com os parâmetros da compilação |
| Probe 00 e Nodes | `BENCH CRYPTO [n]` | uma linha `BENCH` por operação e `BENCH_DONE` |
| Sensor (Pico W) | `BENCH CRYPTO [n]` no console USB | idem (pausa a aquisição durante a medição) |

A Probe também passa a publicar `[GW] WIRELESS_STATS` (custo da criptografia e da ponte, medidos no Node) e `t_us=` (relógio dela) nas linhas de eventos.
