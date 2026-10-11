# Firmware do Node CAN no computador

Este diretório contém duas simulações. As duas compilam os arquivos reais de `Codigo/node-can/src/` com os substitutos de hardware de `stubs/` (Arduino, SPI, ACAN2515, NimBLE, Wi-Fi e UDP). Nada aqui faz parte do firmware: o PlatformIO só compila `src/`.

## 1. Plano de dados de um Node — `sim_wireless_link.cpp`

Exercita `wireless_link.cpp` nos dois papéis que ele tem no firmware:

```text
sensor simulado <-UDP-> wireless_link.cpp como Node 1 <-CAN-> wireless_link.cpp como Probe 00
```

O sensor simulado usa a biblioteca `Codigo/common/ioc_link` e as estruturas de `Codigo/node-wifi/edge_protocol_definitions.h`, como o firmware do Pico W.

O que verifica:

- ponto de acesso e oferta BLE gerados na associação;
- recusa explícita de comandos enquanto não há sessão;
- handshake, telemetria periódica, espectro de 64 e de 256 faixas;
- configuração aceita e configuração recusada pelo sensor;
- controle do encaminhamento, limpeza de DTC, DTC urgente;
- repetição de datagrama, datagrama adulterado, sensor com chave errada e HELLO repetido durante uma sessão;
- expiração da sessão, recuperação e desassociação;
- tempo de ida e volta do enlace e tempo de resposta de comandos.

```bash
./Codigo/scripts/build_wireless_link_sim.sh /tmp/sim_wireless_link
/tmp/sim_wireless_link
```

`stubs_single/node_config.h` torna `NODE_ID` uma variável, para que o mesmo executável atue como Node e como Probe. `Front/tests/test_wireless_data_plane_v017.py` entrega a saída ao decodificador e ao domínio da TUI.

## 2. Bancada virtual — `sim_network.cpp`

Executa a Probe 00 e três Nodes em um único processo. Cada Node é o firmware completo (`main.ino`, `comandos.cpp`, `falhas.cpp`, `wireless_discovery.cpp`, `wireless_link.cpp`, `net_metrics.cpp`, `fault_injection.cpp`).

### Como várias cópias do firmware convivem

O firmware usa variáveis globais, como todo programa Arduino. Para ter várias cópias no mesmo processo, cada arquivo é compilado uma vez por Node, dentro de um namespace:

| Arquivo | Papel |
|---|---|
| `sim_prelude.h` | inclui, no escopo global, a biblioteca padrão e a biblioteca C do projeto, antes de qualquer namespace ser aberto |
| `sim_stubs.h` | inclui os substitutos de hardware; dentro do namespace, cada Node ganha os seus (`Serial`, `WiFi`, `can`, fila UDP) |
| `sim_unit.cpp` | `namespace SIM_NS { #include <arquivo do firmware> }` |
| `sim_instance.cpp` | relógio, serial e rádios do Node, e a classe que o expõe à bancada (`SimInstance`, em `sim_api.h`) |
| `sim_network.cpp` | barramento, rádios, sensores, cenários e verificações |

Um cabeçalho de sistema que faltasse em `sim_prelude.h` seria incluído dentro do namespace e a compilação falharia; é assim que o esquecimento aparece.

Um reinício (`ESP.restart()`) consome uma cópia: o estado em RAM de quem reiniciou precisa voltar ao inicial, e isso só existe em outra cópia das variáveis. Os Nodes 1 a 3 têm duas cópias cada.

### O que a bancada fornece

- **Barramento CAN.** A cada instante vence o menor identificador entre os quadros prontos. O quadro ocupa o barramento pela sua duração real a 500 kbit/s. Sem outro nó para confirmá-lo, o quadro é retransmitido e o contador de erros de transmissão sobe.
- **Relógio.** Passos de 0,5 ms. `delay()` avança o relógio e deixa o barramento escoar. Dentro de um passo, cada leitura de `micros()` avança 37 µs, para que dois quadros tratados no mesmo passo não tenham o mesmo instante.
- **Rádio.** Os anúncios BLE de cada sensor chegam aos Nodes que o alcançam, com o RSSI definido pelo cenário. As ofertas de vínculo e o ponto de acesso de cada Node são lidos dos substitutos.
- **Sensores.** Seguem as regras de `edge_network_driver.c`: ignoram ofertas com sessão ativa, desistem da sessão após 10 s de silêncio, só aceitam a oferta de outro Node depois de 15 s. Autenticam-se com `ioc_link`, enviam duas amostras de telemetria por segundo (o Node encaminha uma por segundo) e devolvem o contador do `PING`.

```bash
./Codigo/scripts/build_network_sim.sh /tmp/sim_network
/tmp/sim_network --list
/tmp/sim_network owner_failover            # linhas da Probe 00, com o instante: "@12345 [GW] ..."
/tmp/sim_network owner_failover --verbose  # inclui a serial de cada Node: "#N2@12345 ..."
```

Cada cenário é uma execução, por causa dos reinícios. Uma verificação que falhe encerra com código 1 e informa o instante simulado. Os cenários estão descritos em `Documentacao/arquitetura/metricas-e-ensaios.md`.

`Codigo/scripts/record_network_scenarios.sh` grava a saída de todos os cenários em `Front/replays/`. A TUI as usa nos testes e as reproduz com `--replay`.

## Limites

Os substitutos não reproduzem temporização de rádio, coexistência BLE/Wi-Fi, perda de quadros por ruído, a comunicação SPI com o MCP2515 nem a latência da serial. Os tempos de associação Wi-Fi e de reinício são parâmetros da bancada. O que se verifica aqui é a lógica do firmware e a integração com a TUI; a compilação para o ESP32 é verificada pelo PlatformIO na integração contínua, e o comportamento em rádio, em bancada física.

Ferramentas que poderiam complementar esta bancada no futuro (SocketCAN, GNU Octave, OMNeT++, Renode), com prioridade muito baixa, estão em `Documentacao/arquitetura/metricas-e-ensaios.md`, seção 8.1.
