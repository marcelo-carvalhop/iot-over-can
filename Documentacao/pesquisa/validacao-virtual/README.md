# Validação do método na bancada virtual

Esta pasta guarda a saída de `python -m iocpesquisa analisar` da campanha `Pesquisa/desenhos/bancada_virtual.json`: 840 execuções, 28 condições, 30 repetições cada. A coleta foi feita no commit `347eddb`, com a árvore limpa, e o `verificar` não apontou nenhum problema. O relatório completo está em [`relatorio.md`](relatorio.md), e as tabelas estão em [`condicoes.csv`](condicoes.csv) e [`comparacoes.csv`](comparacoes.csv).

**Para que serve.** Para mostrar o instrumento funcionando de ponta a ponta e validar o método. A bancada virtual executa o firmware real dos Nodes, mas não modela o tempo de CPU, o rádio nem a temporização do MCP2515. Por isso, **os números de tempo para o texto devem vir da bancada física** (`Pesquisa/desenhos/bancada_fisica.json`). Os valores abaixo servem como hipóteses a confirmar e para conferir a lógica.

## O que a validação encontrou no próprio projeto

1. **Sondas em fase com a rede.** A primeira campanha usava trens de eco de período fixo. Eles entraram em fase com o relatório do enlace do Node, publicado a cada 5 s, e produziram um efeito espúrio: sem carga, a integração ligada parecia *reduzir* o p99 do eco. Corrigido com amostragem de Poisson e fase da janela sorteada por repetição ([metodologia, seção 3.3](../metodologia.md)).
2. **Disputa na reassociação.** Com a fase sorteada, 1 das 30 repetições de "sensor desligado com 50 % de carga" moveu o sensor, ao reaparecer, para um Node de sinal pior. A telemetria voltou em 16,8 s em vez de 1,5 s. A política foi corrigida, e a campanha desta pasta já roda com a correção ([metricas-e-ensaios, seção 5.2](../../arquitetura/metricas-e-ensaios.md)).

## Resultados preliminares (bancada virtual)

| Pergunta | O que os dados mostram |
|---|---|
| A integração muda a latência típica do barramento? | Não. A média e a mediana do eco LOCAL ficam em 1,57 a 1,62 ms até 50 % de carga, com ou sem integração. A 80 % de carga, as diferenças de média não são significativas depois do ajuste de Holm. |
| E a cauda? | Sim, com carga alta. O p99 por repetição do eco LOCAL vai de 3,84 para 5,48 ms com carga HIGH a 80 % (delta de Cliff 0,43; p de Holm 0,025), de 4,20 para 6,28 ms com MID a 80 % (0,52; 0,004) e de 1,65 para 2,17 ms com LOW a 80 % (0,46; 0,003). Ver `figuras/eco_local_ccdf.png`. |
| Por quê? (hipótese) | A ponte Wi-Fi → CAN do Node passa de 1,0 ms sem carga para 5,7 ms com carga HIGH ou MID a 80 %. A confirmação do eco sai pela mesma fila de transmissão do Node que os segmentos da telemetria e pode esperar atrás deles. A hipótese deve ser testada na bancada física. |
| O sinal de presença do líder sofre? | A variação cresce com a carga (de 5 para cerca de 25 µs; algumas repetições com HIGH a 80 % chegam a 300 µs), mas não com a integração: nenhuma comparação `iot` é significativa. |
| Perde-se telemetria? | Não: zero amostras perdidas em todas as condições. |
| Falhas (tempos contados da falha) | Sensor desligado: sessão perdida em 9,8 s, STALE em 24,8 s, LOST em 54,8 s; telemetria de volta 1,7 s depois de o sensor religar. Queda do Node responsável: decisão do líder em 4,4 s, telemetria de volta em 25,2 s. Queda do ponto de acesso: volta 0,2 s depois do retorno. Sessões descartadas: volta em 8,4 s. Carga de 50 % não altera esses tempos. |

Reproduzir:

```bash
cd Pesquisa
../Codigo/scripts/build_network_sim.sh /tmp/sim_network
python3 -m iocpesquisa coletar desenhos/bancada_virtual.json --binario /tmp/sim_network -j 4
python3 -m iocpesquisa analisar resultados/bancada_virtual
python3 -m iocpesquisa verificar resultados/bancada_virtual
```

Com o mesmo commit, as sementes reproduzem os mesmos registros, linha a linha.
