# IoT-over-CAN — laboratório virtual, etapas 2 e 3

Extensão local da bancada nativa. Não substitui os 17 cenários de regressão nem altera o firmware real embarcado. Compatível com a etapa 1, mas o instalador também consegue aplicá-la diretamente sobre o checkout original.

## Instalação

Na raiz do clone local (`iot-over-can`):

```bash
bash /caminho/ioc_sim_stage23/install.sh .
git diff --stat
git diff -- Codigo/node-can/test/host/sim_network.cpp Front/pico_tui/__main__.py Codigo/scripts/build_network_sim.sh
bash Simulacao/verify.sh
```

O instalador não executa `git add`, `commit`, `push` ou ações remotas. As transformações estruturais do código e da TUI são verificadas em uma cópia temporária antes de gravá-las. Ele não se responsabiliza por preservar mudanças locais não comitadas já existentes nos arquivos substituídos: faça backup ou revise `git diff` antes de aplicar.

## Compilação com fábrica configurável

```bash
# padrão: até 6 IDs funcionais (1..6), duas gerações por ID
./Codigo/scripts/build_network_sim.sh /tmp/ioc_sim_network

# bancada ampliada, 10 IDs e 4 boots por ID (compilação substancial)
IOC_SIM_MAX_NODES=10 IOC_SIM_GENERATIONS=4 IOC_SIM_BUILD_JOBS=4 \
  ./Codigo/scripts/build_network_sim.sh /tmp/ioc_sim_network
```

A compilação gera fábricas C++ isoladas por namespace para cada combinação ID/geração. O número de Nodes *ativos* muda em execução (`NODE ... ADD/REMOVE`), mas a quantidade total de IDs e gerações ainda é um limite de compilação; não é virtualização ilimitada. IDs permitidos neste estágio: 1..31, pelo espaço atual do protocolo CAN. O modo legado sempre inicia com Probe 00 e Nodes 1, 2 e 3 para conservar os cenários históricos.

Um `POWER OFF` consome a geração corrente; o próximo `POWER ON` precisa de outra. `NODE REMOVE` também exige uma nova geração para reinserção. Após esgotar as gerações, reinicie a simulação ou compile uma quantidade maior. Em reinicialização do firmware, a NVS virtual é preservada, enquanto a RAM é descartada ao trocar de instância.

## Etapa 2 — iniciar diretamente na TUI

Depois de compilar e instalar as alterações:

```bash
cd Front
python3 -m pico_tui --sim --sim-binary /tmp/ioc_sim_network
```

O parâmetro `--sim` inicia internamente a ponte e o motor nativo, cria uma porta `/dev/pts/N` e conecta a mesma TUI Textual usada na bancada física à Probe virtual. Não há necessidade de copiar uma porta manualmente nem abrir três terminais. A conexão usa o protocolo textual real da Probe 00, não replay pré-gravado.

A simulação tem segurança desativada por padrão somente com `--sim`, por empregar chave de teste isolada e socket Unix privado. A execução com hardware conserva a política anterior (`presence`). Pode-se informar `--security-mode presence` explicitamente mesmo com simulação, se desejado.

A TUI apresenta a rede. Para controlar o ambiente durante seu funcionamento, abra outro terminal na raiz do projeto:

```bash
python3 Simulacao/sim_ctl.py STATUS
python3 Simulacao/sim_ctl.py PAUSE
python3 Simulacao/sim_ctl.py NODE 4 ADD
python3 Simulacao/sim_ctl.py RESUME
```

O cliente encontra automaticamente uma *única* sessão local ativa. Havendo duas ou mais, exige `--socket <caminho>` para eliminar ambiguidades. A porta serial e o caminho do socket também são anunciados na inicialização da TUI, antes da interface em tela cheia.

O controle pelo socket continua sendo a ferramenta para montar o ambiente; as operações normais de descoberta, associação, desassociação e configuração continuam na TUI.

O replay não é uma sessão interativa: não combine `--sim` com `--replay`, `--demo` ou `--port`. A captura pode ser solicitada com `--sim-record /tmp/ensaio.log` e reproduzida posteriormente com `--replay`.

## Etapa 3 — topologia e condições mutáveis

O simulador inicia a Probe e três Nodes. Adicione outros Nodes sem encerrar a execução:

```text
NODE 4 ADD
NODE 5 ADD
NODE 4 POWER OFF
NODE 4 POWER ON
NODE 5 TX OFF
NODE 5 TX ON
NODE 5 REMOVE
```

`NODE REMOVE` representa remoção física: não executa `WIRELESS UNBIND` e não apaga a tabela de associação do firmware. O sensor perde comunicação se seu responsável foi removido; anúncios BLE para um Node removido são ignorados em vez de encerrarem o processo.

Sensores também são mutáveis:

```text
SENSOR ADD 0xE6616408432B6F39 VIBRATION
SENSOR RSSI 0xE6616408432B6F39 1 -52
SENSOR RSSI 0xE6616408432B6F39 4 -68
SENSOR LINK 0xE6616408432B6F39 1 LOSS 20
SENSOR POWER 0xE6616408432B6F39 OFF
SENSOR POWER 0xE6616408432B6F39 ON
```

`LOSS` ajusta a probabilidade simulada de descartar pacotes BLE e datagramas UDP nos dois sentidos daquele enlace, em percentual de 0 a 100, sobre gerador pseudoaleatório determinístico. RSSI e perda são independentes neste modelo — RSSI não implica automaticamente PER. A reprodução exata exige mesma sequência de comandos, condições iniciais e versão do motor.

É possível reduzir a capacidade computacional aparente do Node por fração de ciclos de execução do `loop()`:

```text
NODE 4 BUDGET 60
```

Isso executa aproximadamente 60% das passagens do laço daquele Node, em média, com seleção pseudoaleatória. **Não** é medição real de consumo de CPU, interrupções ou prioridades de escalonamento. A Probe não é afetada.

Para as configurações de enlace, o tempo de transmissão BLE/Wi-Fi, a coexistência física dos rádios, a latência SPI, as retransmissões reais de Wi-Fi e o consumo energético **ainda não são simulados**. A nova perda percentual é um modelo de injeção de falha, não um modelo físico calibrado de propagação.

A associação lógica deve ocorrer através da Probe ou TUI, e não automaticamente ao criar o sensor. Por exemplo, na TUI, selecione o candidato e associe-o a um Node após os anúncios aparecerem.

## Diagnóstico e validação

```bash
python3 -m unittest discover -s Simulacao/tests -v
bash Simulacao/verify.sh
./Codigo/scripts/test_native_firmware.sh
./Codigo/scripts/test_tui.sh
```

`verify.sh` compila contra o firmware real, verifica a lista dos 17 cenários, executa comandos de inserção/remoção e o cenário `formation`. A suíte completa nativa e da TUI permanecem recomendações adicionais. Não interprete um PASS dos testes de stubs ou da sintaxe isolada como validação temporal no hardware.

## Próximos passos

Para eliminar a necessidade de pré-compilar uma quantidade de gerações por ID, será necessária uma reestruturação maior, por exemplo um processo de firmware isolado por Node com IPC e relógio dirigido pelo motor, ou outro mecanismo confiável de isolamento e reinicialização de variáveis globais. Um modelo mais fiel de rádio/capacidade deverá ser calibrado com dados de bancada física. A interface gráfica de montagem de topologias ainda não integra esta entrega.
