# IoT-over-CAN — Simulation Studio (etapa 4)

Aplicação web **local** para visualizar e manipular a bancada virtual das etapas 1–3 e executar experimentos reprodutíveis. Não cria uma segunda implementação dos algoritmos de rede: usa `sim_network --interactive` e as fábricas de firmware existentes.

## Instalar no checkout local

Na raiz do clone do projeto, depois de descompactar o pacote:

```bash
bash /caminho/ioc_sim_stage4/install.sh .
git diff --stat
python3 -m unittest discover -s Simulacao/tests -v
./Codigo/scripts/build_network_sim.sh /tmp/ioc_sim_network
```

O instalador pode acrescentar as etapas 2 e 3 quando ainda não estão instaladas. Caso elas já estejam presentes, preserva os arquivos existentes de interface e bridge e aplica apenas o complemento de simulação e os novos módulos da etapa 4. **Não realiza commit, merge, push ou modificações remotas.** Faça revisão de mudanças locais antes de executar o instalador.

## Abrir o Studio (inicia o simulador automaticamente)

```bash
python3 Simulacao/studio/server.py --binary /tmp/ioc_sim_network --browser
```

Se o navegador não abrir automaticamente, acesse `http://127.0.0.1:8765`. O Studio disponibiliza a topologia, os instrumentos, o inspetor de dispositivos e a aba de experimentos. Ele cria uma pseudo-serial (`PORT=/dev/pts/N`) e um socket Unix privado (`CONTROL=...`).

É possível abrir, em outro terminal, a TUI na mesma bancada sem criar outra simulação:

```bash
cd Front
python3 -m pico_tui --port /dev/pts/N --mode probe --security-mode off
```

Substitua `/dev/pts/N` pelo caminho exibido na inicialização do Studio. O modo de segurança desativado é apropriado **somente** à bancada local de teste, que utiliza credenciais fictícias. Não use essa opção para dispositivos físicos.

## Anexar o Studio a uma TUI já aberta

No primeiro terminal, da raiz do projeto:

```bash
cd Front
python3 -m pico_tui --sim --sim-record /tmp/ioc_probe.log --sim-binary /tmp/ioc_sim_network
```

No segundo terminal:

```bash
python3 Simulacao/studio/server.py --attach --record /tmp/ioc_probe.log --binary /tmp/ioc_sim_network --browser
```

No modo `--attach`, o Studio não encerra a bancada quando for fechado. Sem `--record`, as métricas de STATUS funcionam, mas não haverá fluxo de eventos da Probe disponível no painel. Se houver várias sessões, use `--socket /tmp/ioc-sim-XXXX/control.sock`.

## Funcionalidades

Na **Topologia**, a Probe 00 aparece ligada ao barramento CAN e os Nodes aparecem com seu estado de alimentação, eleição e transmissão. Sensores de vibração e acústicos aparecem com todos os enlaces de rádio declarados no simulador. Selecionar um dispositivo abre o inspetor: é possível ligar ou desligar Nodes, interromper transmissões, alterar o orçamento de loop, inserir/remover sensores e configurar RSSI e probabilidade de perda em cada enlace. Arrastar um sensor sobre um Node abre a configuração de RSSI, criando ou modificando a observação de alcance. **Isso não altera automaticamente a associação lógica**. A vinculação e a desvinculação são comandos separados, encaminhados ao firmware através da Probe 00.

Em **Métricas**, os gráficos mostram a taxa de quadros confirmados por segundo e a ocupação aproximada com base no tempo virtual, calculada como:

\[
U_{CAN}=100\frac{\Delta\mathrm{bits\_confirmados}}{500000\,\Delta t_{sim}}
\]

A janela usa diferenças das amostras STATUS coletadas pelo servidor. Como o contador do motor considera quadros confirmados, o indicador **não mede toda a utilização física sob erros ou retransmissões**. A Probe fornece, separadamente, as linhas BUS_STATS no arquivo de captura. Valores de ocupação CAN não devem ser interpretados como validação de escalonabilidade sem o modelo de pior caso e ensaios físicos.

Em **Experimentos**, o usuário escolhe um dos quatro modelos ou edita diretamente sua especificação JSON. Cada execução cria um novo processo de firmware simulado que começa com o relógio pausado, após a formação inicial da rede, e recebe os eventos em tempos relativos `at_ms`. Cada repetição possui sua própria semente pseudoaleatória, que pode ser fixa (`seed_mode: fixed`) ou incrementada (`seed_mode: increment`). A formação inicial é idêntica; a semente é aplicada após o estabelecimento da rede.

### Exemplos predefinidos

| Arquivo | Objetivo |
|---|---|
| `01_baseline.json` | Linha de base sem intervenções |
| `02_leader_failure.json` | Desligamento e retorno do líder |
| `03_wireless_failover.json` | Associação wireless e perda de alcance |
| `04_processing_budget.json` | Redução e restauração do orçamento sintético de processamento |

Formato mínimo de um ensaio:

```json
{
  "name": "Falha do Node 03",
  "duration_ms": 40000,
  "repetitions": 3,
  "seed": 19088743,
  "seed_mode": "increment",
  "steps": [
    {"at_ms": 2000, "command": "NODE 3 POWER OFF"},
    {"at_ms": 22000, "command": "NODE 3 POWER ON"}
  ]
}
```

São aceitos os comandos de modificação de Nodes, sensores, enlaces e associação operacional permitidos pelo Studio. `STEP`, `RESUME`, `PAUSE`, `QUIT` e `SEED` não são permitidos nas etapas de um ensaio porque sua execução temporal é administrada exclusivamente pelo runner. O limite atual é de 200 intervenções, 300 segundos virtuais por repetição e até 10 repetições.

### Executar sem navegador

```bash
python3 Simulacao/studio/experiments.py \
  --binary /tmp/ioc_sim_network \
  --spec Simulacao/studio/experiments/03_wireless_failover.json \
  --out Simulacao/results
```

Cada experimento é armazenado em `Simulacao/results/<id>/` com `spec.json`, `summary.json`, `comparison.csv`, `comparison.json` e um diretório por repetição (`run-01`, etc.) contendo `trace.log`, `events.jsonl`, `bus_stats.csv` e `metrics.json`. `trace.log` pode ser reproduzido na TUI com:

```bash
cd Front
python3 -m pico_tui --replay ../Simulacao/results/<id>/run-01/trace.log
```

Na interface web, o botão **Exportar ZIP** empacota integralmente o experimento selecionado. O SHA-256 do executável é incluído no resumo. Os relatórios agregados mostram média, mínimo, máximo e desvio-padrão amostral, quando aplicável. **Repetições com a mesma semente não são amostras independentes**, mesmo que gerem linhas de estatística por repetição.

## Segurança e limites

O servidor HTTP fica restrito a `127.0.0.1`, verifica o host e exige um token aleatório de sessão nas requisições à API; comandos são construídos a partir de tipos explicitamente permitidos. Isso não equivale a autenticação multiusuário ou isolamento de um ambiente exposto à Internet. Não encaminhe a porta do servidor e não utilize `--security-mode off` em hardware físico.

O laboratório preserva as limitações da etapa 3: IDs e gerações dos Nodes são pré-compilados (`IOC_SIM_MAX_NODES`, `IOC_SIM_GENERATIONS`); CPU é orçamento sintético de loops, e o rádio usa modelos parametrizados. O simulador não resolve temporização SPI, latência do stack físico BLE/Wi-Fi, consumo elétrico e erros analógicos do CAN. Os ensaios devem ser calibrados com medições reais antes de fundamentar conclusões sobre o sistema físico.

## Testes

```bash
python3 -m unittest discover -s Simulacao/tests -v
bash Simulacao/verify.sh
./Codigo/scripts/test_native_firmware.sh
./Codigo/scripts/test_tui.sh
```

Os testes Python da etapa 4 validam o parser do protocolo, a whitelist dos comandos, a separação de processos entre repetições, execução temporal simulada, exportação de relatórios, proteção da API e idempotência das alterações C++. A bateria nativa depende de um checkout completo do projeto no Linux Mint. A compilação e os testes contra o firmware completo ainda precisam ocorrer na máquina de desenvolvimento; sucesso dos testes auxiliares não implica validação do hardware.
