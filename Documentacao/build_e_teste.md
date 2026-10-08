# Build e teste

Os scripts operacionais ficam em `Codigo/scripts/` e são executados a partir da raiz do repositório.

## TUI

Instalação/atualização do ambiente virtual:

```bash
./Codigo/scripts/setup_tui.sh
```

O script instala `Front/` em modo editável com as dependências de desenvolvimento de `Front/pyproject.toml`.

Execução offline-first:

```bash
./Codigo/scripts/run_tui.sh
```

A TUI abre mesmo sem hardware. Use `F3` ou `p` para escolher uma porta.

Execução com a Probe 00:

```bash
./Codigo/scripts/run_tui.sh /dev/ttyUSB0
```

Execução direta do pacote:

```bash
cd Front
source .venv/bin/activate
iot-over-can-tui --demo
iot-over-can-tui --port /dev/ttyUSB0 --mode probe
iot-over-can-tui --ascii
iot-over-can-tui --messages
```

`--mode gateway` continua aceito como alias interno de compatibilidade; a documentação usa Probe 00.

Testes completos:

```bash
./Codigo/scripts/test_tui.sh
```

A suíte inclui protocolo, estado, segurança, preferências, catálogo de comandos, intercorrências, associação wireless e contratos do redesenho. O teste de layout percorre múltiplas telas em 48×18, 60×24, 80×24, 100×30 e 160×48 para detectar conteúdo horizontalmente oculto/truncado.

Validação rápida de sintaxe sem abrir a TUI:

```bash
python -m compileall Front/pico_tui
```

## Validação manual da TUI

Com a Probe conectada:

1. abrir a TUI e confirmar que **Início** mostra condição, equipamentos, rede e conexão;
2. percorrer um módulo e voltar com `Esc`;
3. abrir `F4/c Comandos` e confirmar que ações têm descrição e alvo explícito;
4. abrir `F5/r Rede` e conferir barramento/quadros;
5. abrir `F6/m Mensagens` e conferir que DEBUG fica oculto por padrão;
6. abrir `F7/w Sensores sem fio` e confirmar que o mesmo UUID aparece uma vez em Disponíveis, com todas as observações nos detalhes;
7. associar escolhendo explicitamente um Node e acompanhar `ASSOCIATING → BOUND → ONLINE`;
8. realçar o sensor em Associados, pressionar `d`, confirmar e verificar que o filho só desaparece após `UNBOUND`;
9. se o Pico continuar anunciando, confirmar que o UUID volta a Disponíveis;
10. abrir o filho associado e acompanhar o campo **Plano de dados** até "sessão autenticada ativa"; antes disso, telemetria/configuração via Node devem aparecer como indisponíveis, sem ações falsas;
11. com a sessão ativa, conferir telemetria, pedir FFT e alterar a configuração, aguardando "Aplicada pelo sensor".

O roteiro completo da primeira validação do plano de dados está em [`arquitetura/plano-de-dados-wireless.md`](arquitetura/plano-de-dados-wireless.md), seção 10.

O alias `Ctrl+A` pode continuar abrindo o gerenciamento wireless, mas o teste operacional deve usar o caminho oficial visível `F7/w`.

## Node CAN — ESP32 + MCP2515

Build do Node 1:

```bash
./Codigo/scripts/build_esp32_can_node.sh 1
```

Upload do Node 2:

```bash
./Codigo/scripts/upload_esp32_can_node.sh 2 /dev/ttyUSB0
```

`IOT_NODE_ID` é fornecido pelo script. Não edite `main.ino` para mudar o ID. O ID 0 é reservado à Probe 00.

Os mesmos scripts leem a chave mestra do enlace de `.env.local` e a entregam ao build dos Nodes funcionais. Sem ela o firmware compila e funciona, mas com o plano de dados wireless desabilitado. A Probe 00 não recebe chave.

Monitor direto, quando houver porta disponível:

```bash
cd Codigo/node-can
pio device monitor -b 115200
```

O diagnóstico normal da associação pode ser feito pelo JSONL da TUI/Probe, sem exigir uma porta USB adicional para cada Node.

Opções de compilação do Node, passadas pela variável `PLATFORMIO_BUILD_FLAGS`:

| Opção | Padrão | Efeito |
|---|---|---|
| `-D IOT_FAULT_INJECTION=0` | `1` | remove o tratamento dos comandos de ensaio nos Nodes |
| `-D IOT_FOLLOWER_LEADER_WATCHDOG=0` | `1` | desativa a vigilância do líder pelos seguidores |

```bash
PLATFORMIO_BUILD_FLAGS="-D IOT_FAULT_INJECTION=0" ./Codigo/scripts/build_esp32_can_node.sh 1
```

A versão 0.18 muda o formato do estado de vínculo e do relatório de enlace no CAN. Grave todos os Nodes e a Probe 00 com a mesma versão.

## Sensor wireless — Raspberry Pi Pico W

Build de bancada (usa a chave mestra de `.env.local`, se existir):

```bash
./Codigo/scripts/build_pico.sh
```

Build para um sensor específico, apenas com as chaves derivadas para o UUID dele:

```bash
./Codigo/scripts/provision_sensor_security.sh --sensor E6616408432B6F39
./Codigo/scripts/build_pico.sh --sensor E6616408432B6F39
```

Artefato:

```text
Codigo/node-wifi/build/edge_node_firmware.uf2
```

O alvo operacional é `pico_w`.

## Testes nativos

```bash
./Codigo/scripts/test_native_firmware.sh
```

O script executa, no computador, cinco verificações. Requer `cc` e `c++`.

1. Validação de configuração do sensor.
2. Biblioteca `ioc_link`: SHA-256/HMAC, autenticação do enlace, segmentação CAN.
3. Biblioteca `ioc_link`, parte de rede: duração de quadro e CRC-15, PDU autenticada, segmentação em quadros de até 64 bytes e decisão de reassociação.
4. Simulação sensor ↔ Node ↔ Probe, que compila `Codigo/node-can/src/wireless_link.cpp` com os substitutos de `Codigo/node-can/test/host/stubs/`.
5. Bancada virtual: a Probe 00 e três Nodes, cada um o firmware completo, em quinze cenários.

Para inspecionar a saída que a Probe 00 produziria:

```bash
./Codigo/scripts/build_wireless_link_sim.sh /tmp/sim_wireless_link
/tmp/sim_wireless_link

./Codigo/scripts/build_network_sim.sh /tmp/sim_network
/tmp/sim_network --list
/tmp/sim_network leader_failure --verbose      # inclui a serial de cada Node
```

### Gravações da bancada virtual

`Front/replays/` guarda a saída da Probe 00 em cada cenário. Os testes da TUI usam essas gravações e conferem que elas correspondem ao firmware atual. Depois de alterar o firmware do Node de um modo que mude o que a Probe escreve, refaça-as:

```bash
./Codigo/scripts/record_network_scenarios.sh
```

A TUI reproduz uma gravação sem hardware, com as telas de métricas e de ensaios funcionando sobre ela:

```bash
cd Front
python -m pico_tui --replay replays/owner_failover.log --replay-speed 4
```

## Validação wireless de bancada

Com o Pico anunciando:

- vários Nodes devem reportar o mesmo UUID com RSSI independente;
- o Node associado deve manter `ONLINE` enquanto recebe advertisements;
- após 15 s sem BLE, o vínculo passa a `STALE`;
- após 45 s desde o último BLE, passa a `LOST`;
- o retorno do Pico recupera `ONLINE` sem novo `BIND`;
- `UNBIND` deve produzir `UNBOUND` antes de a TUI remover o filho;
- após o `BIND`, o Node deve registrar `AP=ON` e, em seguida, `SECURE` para o filho;
- com o Pico desligado, o plano de dados deve voltar a `OFFERING` em cerca de 10 s e se recuperar sem novo `BIND` quando o Pico retornar.

O roteiro para métricas, vigilância do líder, reassociação e ensaios de falha está em [`arquitetura/metricas-e-ensaios.md`](arquitetura/metricas-e-ensaios.md), seção 9.

## Integração contínua

`.github/workflows/ci.yml` usa os scripts operacionais e executa os testes da TUI, a validação nativa (com a bancada virtual) e os builds de firmware: Pico W, Node CAN e Probe 00. Scripts devem estar versionados como executáveis (`100755`).

Verificação:

```bash
git ls-files -s Codigo/scripts/*.sh
```
