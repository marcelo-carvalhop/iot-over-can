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
10. abrir o filho associado e verificar que telemetria/configuração via Node são indicadas como indisponíveis, sem ações falsas.

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

Monitor direto, quando houver porta disponível:

```bash
cd Codigo/node-can
pio device monitor -b 115200
```

O diagnóstico normal da associação pode ser feito pelo JSONL da TUI/Probe, sem exigir uma porta USB adicional para cada Node.

## Sensor wireless — Raspberry Pi Pico W

Build:

```bash
./Codigo/scripts/build_pico.sh
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

## Validação wireless de bancada

Com o Pico anunciando:

- vários Nodes devem reportar o mesmo UUID com RSSI independente;
- o Node associado deve manter `ONLINE` enquanto recebe advertisements;
- após 15 s sem BLE, o vínculo passa a `STALE`;
- após 45 s desde o último BLE, passa a `LOST`;
- o retorno do Pico recupera `ONLINE` sem novo `BIND`;
- `UNBIND` deve produzir `UNBOUND` antes de a TUI remover o filho.

## Integração contínua

`.github/workflows/ci.yml` usa os scripts operacionais e executa os testes da TUI, validação nativa e builds de firmware. Scripts devem estar versionados como executáveis (`100755`).

Verificação:

```bash
git ls-files -s Codigo/scripts/*.sh
```
