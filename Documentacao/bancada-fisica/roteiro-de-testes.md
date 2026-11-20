# Roteiro de testes da bancada física — IoT over CAN

Procedimento de validação em hardware real, com 89 requisitos de teste (RT). Cada sessão de bancada se registra com o [modelo de sessão](modelo-de-sessao.md) e entra em [`historico/`](../historico/); o que a bancada virtual já confere está em [Validação](../validacao.md).

## 0. Visão geral e como usar

Este roteiro tem 89 requisitos de teste (RT) que levam o sistema da compilação à validação física completa, em 12 fases. Foi escrito sobre o commit `b8fcbab` (12/11/2026, firmware `0.21.0`) e acompanha o repositório: nesta versão, firmware da Probe 00, dos módulos e dos sensores `0.22.0`, protocolo do canal de dados `6`. O que mudou desde o commit base está em «O que mudou desde o commit `b8fcbab`»; quando um RT e essa seção divergirem, vale a seção.

Os comandos, as mensagens e os prazos vêm do código e da documentação do repositório. Nada aqui foi executado em hardware: os tempos marcados com ≈ vêm da bancada virtual e são justamente o que a bancada física precisa confirmar. Onde a saída exata depende da sua montagem, o RT diz o que procurar, não o texto literal.

### O que o commit de 12/11 acrescentou

O commit `b8fcbab` deu à estação a identificação de quem opera e uma trilha de auditoria no histórico. Firmware, protocolo e montagem da bancada continuam iguais. Os testes dessa parte são os RT 74 a 80, na Fase 8.

**A decisão que define a ordem.** Enquanto não houver operador cadastrado, a estação funciona como antes e registra as ações em nome da sua conta do Linux (`sys:<conta>`). A partir do primeiro cadastro (RT 74), toda alteração na rede passa a exigir login, e a sessão expira após 15 minutos sem uso. Por isso o cadastro só acontece na Fase 8, depois dos testes de tempo das fases 4 a 7: um comando recusado por sessão expirada no meio de uma medida estraga a medida.

As saídas dos RT 74, 79 e 80 foram obtidas executando a ferramenta de cadastro e uma sessão simulada da estação neste commit, sem hardware. As dos RT 75 a 78 vêm do código e da mesma sessão simulada; o comportamento com a Probe real é o que a bancada confirma.

### O que mudou desde o commit `b8fcbab`

Os RT afetados foram atualizados no próprio texto; a tabela diz onde procurar. Nada disto foi executado em hardware: a bancada virtual e os testes do computador cobrem o que o computador consegue cobrir, e o que só uma placa mostra está nos RT 86 a 89, da Fase 11, e nos RT 71 e 49.

| RT | O que mudou |
| --- | --- |
| 2 | A versão tem uma única fonte, o arquivo `VERSION`. `python3 Codigo/scripts/versao.py --check` falha se `MANIFEST.json`, os pacotes Python ou os `library.json` repetirem outra versão, ou se algum firmware voltar a ter a versão escrita no código. Não procure mais a versão em `comandos.cpp`. |
| 9 e 15 | O build de um módulo (`build_esp32_can_node.sh`, `upload_esp32_can_node.sh`) para quando falta a chave de enlace; `--sem-chave` compila sem ela de propósito. |
| 10 e 11 | `build_pico.sh` grava cada variante em pasta própria: `Codigo/node-wifi/build/<placa>[-<UUID>]/edge_node_firmware.uf2` e `Codigo/node-audio/build/<placa>[-<UUID>][-mic<SCK>-<SD>]/edge_audio_firmware.uf2`. Compilar outra placa não apaga o `.uf2` anterior. `--mic-pins SCK,SD` troca os pinos do microfone sem `cmake` à mão. |
| 14 a 17 | Todos os firmwares dizem o que têm gravado: `[NODE <n>] [BOOT] FIRMWARE=<versão> COMMIT=<commit> PROTOCOL=CAN_CLASSIC_V1 ROLE=PROBE` (ou `ROLE=NODE`) na partida de cada ESP32, `[PROBE 00] FIRMWARE=<versão> COMMIT=<commit> FEATURES=...` e `PROBE_VERSION` com `COMMIT=`, `VERSION` nos módulos (`NODE_VERSION NODE=<n> FIRMWARE=... COMMIT=... PROTOCOL=... FEATURES=...`) e `VERSION FIRMWARE=... COMMIT=... PROTOCOL=6 NODE_UUID=0x...` nos sensores, mais a linha `BOOT FIRMWARE=... COMMIT=... PROFILE=...` quando a porta está aberta na partida. O módulo passou a mostrar a versão no boot. |
| 14 a 19 | `python3 Codigo/scripts/smoke_boot.py <probe\|modulo\|vibracao\|acustico\|painel\|leitor> --porta <porta>` confere essas linhas, a chave do enlace (`LINK_KEY=MISSING`), a versão contra o arquivo `VERSION` e o commit, e termina em `SMOKE RESULT=PASS\|WARN\|FAIL`. Não substitui os RT: pega na hora o que antes aparecia só no meio de um ensaio. `--selftest` no módulo roda o autoteste. |
| 17, 45, 46 e 64 | O microfone informa `MIC=OK`, `NO_HARDWARE`, `SILENT` ou `STUCK_HIGH`. `MIC=YES` e `MIC=NO` deixaram de existir. |
| 86 (novo) | O módulo e a Probe têm `SELFTEST` (laço interno do MCP2515, contadores de erro e LEDs), recusado depois que a placa entra na rede. Separa defeito de placa de defeito de barramento; os RT 20 a 22 e 30 apontam para ele. |
| 23, 61 e 68 | `python3 Simulacao/comparar_bancada.py <gravacao> --cenario <cenário>` compara a eleição, a reassociação e a abertura do canal da gravação física com a bancada virtual, com tolerância e código de saída. Ver [Validação](../validacao.md). |
| 55 e 61 | O primeiro canal de dados de um sensor não é interrompido pela reassociação automática durante 120 s (`IOT_FO_FIRST_SESSION_GRACE_MS`, CH-078). |
| 89 (novo) | As esperas do sensor (10 s e 15 s) viram parâmetros de compilação (`build_pico.sh --espera-sessao` e `--espera-oferta`), com os padrões de sempre. Mudá-las exige a medição do RT 89; os RT 61 a 63 e 68 fornecem os tempos. |
| 88 (novo) | Opcional: `--auth-can` nos scripts de gravação autentica os comandos `0x304` e `0x305` da Probe; `CANAUTH` mostra o estado. Grave todos os módulos e a Probe com a mesma opção. Ver [Segurança](../seguranca.md). |
| 71 | O módulo detecta o próprio isolamento: com o cabo solto, 6 s sem quadro e transmissões sem confirmação, ele libera o sensor, desliga o ponto de acesso, escreve `[ISOLADO]` e não pede eleição; o comando `ISOLAMENTO` mostra o estado. O «limite conhecido» do passo 3 deixou de valer, mas isso só foi visto na bancada virtual: é o que este RT confirma. O RT 87 trata do comando `ENERGIA`, que mostra o motivo e a contagem dos reinícios e, com divisor resistivo e INA219 ligados, a tensão e a corrente. |
| 49 | `:rotulo <nome>` na estação grava cada evento sonoro detectado, com o rótulo, em `exemplos_sonoros.csv`; `:rotulo off` encerra. É só coleta: o classificador do firmware não muda. |
| 74 a 77 | O aviso do `check` com o cadastro vazio diz o que o modo padrão faz (volta a registrar a conta do Linux e não bloqueia); o bloqueio vale para operadores desativados e para `--operator-mode required`. |
| 29, 67, 81 e 89 | A gravação integral da sessão (`logs/gravacao_*.log`) só existe com `--record-session`; a primeira versão do roteiro supunha que toda sessão era gravada. |
| 78 | `run_tui.sh` repassa `OPERATOR_IDLE=<minutos>` e `OPERATORS_FILE=<arquivo>` e, depois da porta, qualquer opção da estação (`./Codigo/scripts/run_tui.sh <porta> --operator-idle 1`). |
| 79 | Uma associação recusada deixa de ser intitulada «autorizada», e o identificador do sensor é comparado pelo valor: decimal ou sem zeros à esquerda também recebe o «Autorizado por». |
| 80 | Ao encerrar a sessão a estação anota o último hash da cadeia de auditoria no arquivo `logs/session_*.jsonl`, e `operadores.sh audit --verify` o confere: o corte dos últimos registros passa a ser detectado (`CORTE OU SUBSTITUIÇÃO`). |

Para repetir a bancada virtual que serve de linha de base, nada mudou no RT 6, exceto que o número de cenários e de testes é maior.

### O que será testado

| Peça | Hardware | Firmware | Console |
| --- | --- | --- | --- |
| Probe 00 (ID 0) | ESP32 + MCP2515 (8 MHz, TJA1050) | `Codigo/node-can` | USB, 115200, texto |
| Módulo CAN (ID 1 a 31) | ESP32 + MCP2515 | `Codigo/node-can` | USB, 115200, monitor |
| Sensor de vibração | Pico W + MPU6050 | `Codigo/node-wifi` | USB, 115200, prompt `EDGE>` |
| Sensor acústico | Pico W ou Pico 2 W + INMP441 | `Codigo/node-audio` | USB, 115200, prompt `EDGE>` |
| Painel | ESP32-2432S028 (CYD) | `Codigo/node-paper` | USB, 115200, linhas `#IOC` |
| Leitor (fora da rede) | M5Stack PaperColor | `Codigo/leitor` | USB, 115200, linhas `#LEITOR` |
| Estação (TUI) | computador, Python 3.11+ | `Front/pico_tui` | terminal |

### Ordem das fases

1. Fase 0, preparação: ferramentas, credenciais, testes no computador (RT 1 a 7).
2. Fase 1, compilação de cada firmware (RT 8 a 13).
3. Fase 2, gravação e primeiro boot de cada placa (RT 14 a 19).
4. Fase 3, barramento CAN: camada física, eleição, métricas (RT 20 a 28).
5. Fase 4, cada módulo individualmente (RT 29 a 36).
6. Fase 5, cada sensor individualmente, pelo console USB (RT 37 a 52).
7. Fase 6, integração: descoberta, canal de dados, reassociação, sensor acústico, painel (RT 53 a 67).
8. Fase 7, robustez: ensaios de falha, alimentação e barramento degradados (RT 68 a 71).
9. Fase 8, segurança: chaves do sensor, autorização na estação, operador e auditoria (RT 72 a 80).
10. Fase 9, longa duração (RT 81).
11. Fase 10, opcionais para fazer por último (RT 82 a 85).
12. Fase 11, recursos acrescentados depois do commit `b8fcbab` (RT 86 a 89). O RT 86 pede a placa fora da rede: faça-o logo depois do RT 14 ou do RT 15, em vez de esperar o fim.

A numeração segue a ordem de execução: faça os RT em sequência. Cada fase depende da anterior; se um RT vital falhar, pare e resolva antes de seguir, porque os seguintes vão falhar pelo mesmo motivo e confundir o diagnóstico. Os RT marcados como opcionais dentro de uma fase podem ser pulados sem prejudicar os seguintes.

### Convenções

- **Prioridade:** *vital* (sem ele o sistema não está validado), *recomendado* (confirma algo que só a bancada física mostra) ou *opcional* (bom ter).
- **Placeholders:** `<porta>` é a porta serial (`/dev/ttyUSB0`, `/dev/ttyACM0`), `<UUID>` é o identificador de 64 bits do sensor, `NN.CC` é módulo e filho (`01.01`), `<token>` é o token de manutenção.
- **Registro no caderno, para cada RT:** cada RT termina com a linha **Registro**, logo depois da condição de aceite. É ali que você anota, sempre nesta ordem: data e hora, placa e ID, versão gravada, resultado (passou, falhou ou não se aplica), valor medido e o que observou de inesperado. A linha já diz, para aquele RT, de qual placa anotar o ID e quais valores medir; *não há* e *não se aplica* indicam um campo sem nada a anotar. Em *versão*, vale o commit do RT 2, salvo quando a linha disser outra coisa.
- **Três terminais abertos ajudam muito:** a estação na Probe 00, o monitor serial do módulo em teste e o console do sensor em teste. Um sintoma quase sempre se explica cruzando os três.
- **Gravação da sessão:** abra a estação com `--record-session` (`./Codigo/scripts/run_tui.sh <porta> --record-session`) em toda sessão de bancada. Sem ela não há `gravacao_*.log` para comparar com a bancada virtual nem para reproduzir depois.
- **Uma porta, um programa:** a estação e `pio device monitor` não dividem a mesma porta. Feche um antes de abrir o outro.

## Fase 0. Preparação

Tudo aqui roda no computador, sem nenhuma placa ligada. O objetivo é chegar à bancada sabendo que o código, as ferramentas e as chaves estão corretos, para que qualquer falha posterior seja do hardware.

### [ ] RT 1 - Conferindo as ferramentas do computador

Prioridade: vital.

**Comandos necessários**

```bash
python3 --version
pio --version
cmake --version
arm-none-eabi-gcc --version
echo "$PICO_SDK_PATH" && ls "$PICO_SDK_PATH/pico_sdk_init.cmake"
cc --version && c++ --version
groups | tr ' ' '\n' | grep -E 'dialout|uucp'
```

**Variações possíveis**

- PlatformIO ausente: `python3 -m pip install platformio`.
- No macOS não existe o grupo `dialout`; a última linha não se aplica.

**Saídas esperadas**

- Python 3.11 ou mais novo; CMake 3.13 ou mais novo; `arm-none-eabi-gcc` presente.
- `PICO_SDK_PATH` apontando para o Pico SDK 2.3.1, com os submódulos baixados.
- Usuário no grupo `dialout` (Linux).

**Se sair isso; faça isso**

- Se `PICO_SDK_PATH` sair vazio; exporte a variável no `~/.bashrc` e abra outro terminal.
- Se o SDK não tiver `lib/cyw43-driver` ou `lib/btstack` preenchidos; rode `git submodule update --init` dentro do SDK. Sem eles o build do Pico W falha no Wi-Fi/BLE.
- Se o usuário não estiver em `dialout`; `sudo usermod -aG dialout $USER`, saia da sessão e entre de novo.

**Condição de aceite:** todas as ferramentas respondem com versão igual ou superior à exigida.

**Registro:** data e hora · placa e ID: não se aplica (só o computador) · versão: não se aplica · resultado · medido: versão de cada ferramenta · inesperado.

### [ ] RT 2 - Conferindo a versão do repositório

Prioridade: vital.

**Comandos necessários**

```bash
git clone https://github.com/marcelo-carvalhop/iot-over-can.git
cd iot-over-can
git log -1 --format='%h %ad %s' --date=short
git status --short
```

**Variações possíveis**

- Repositório já clonado: `git pull` antes de conferir.

**Saídas esperadas**

- O commit do repositório que você vai gravar, por exemplo `git log -1` mostrando o hash e a data. O roteiro foi escrito sobre o `b8fcbab` (12/11/2026); os commits seguintes estão descritos em «O que mudou desde o commit `b8fcbab`».
- `cat VERSION` com o número que todos os firmwares vão mostrar (`0.22.0` nesta versão do roteiro) e `python3 Codigo/scripts/versao.py --check` terminando sem erro.
- `git status --short` sem arquivos modificados.

**Se sair isso; faça isso**

- Se houver arquivos modificados; anote quais. Um firmware gravado com alterações locais não é a versão que este roteiro descreve.
- Se `versao.py --check` apontar divergência; alguma cópia da versão ficou para trás (a única fonte é o arquivo `VERSION`). Corrija antes de gravar qualquer placa.
- Se o commit for mais novo que o desta versão do roteiro; leia o histórico de `Documentacao/desafios.md` e as mudanças de `Documentacao/validacao.md` antes de seguir.

**Condição de aceite:** commit e versão anotados no caderno, `versao.py --check` sem erro e árvore de trabalho limpa. Todas as placas serão gravadas a partir deste mesmo commit.

**Registro:** data e hora · placa e ID: não se aplica · versão: o hash do commit, que é o valor deste RT · resultado · medido: não há · inesperado.

### [ ] RT 3 - Instalando a estação de operação (TUI)

Prioridade: vital.

**Comandos necessários**

```bash
./Codigo/scripts/setup_tui.sh
Front/.venv/bin/iot-over-can-tui --demo
```

**Variações possíveis**

- Terminal sem Unicode: acrescente `--ascii`.
- Reprodução de uma sessão gravada: `cd Front && .venv/bin/python -m pico_tui --replay replays/owner_failover.log --replay-speed 4`.
- Ambiente criado antes de 12/11: rode `./Codigo/scripts/setup_tui.sh` de novo. Ele reinstala o pacote e cria o comando `iot-over-can-operators`, usado na Fase 8.

**Saídas esperadas**

- `TUI instalada no ambiente virtual.` e o caminho do executável.
- A demonstração abre na tela Início, com módulos, sensores de vibração e um sensor acústico inventados. `F10` ou `q` sai.
- `Front/.venv/bin/iot-over-can-operators --help` lista os subcomandos `list`, `add`, `passwd`, `enable`, `disable`, `role`, `remove`, `check` e `audit`.

**Se sair isso; faça isso**

- Se o `pip` falhar por versão do Python; a estação exige 3.11+. Crie o ambiente com outro interpretador: `python3.11 -m venv Front/.venv`.
- Se a tela sair com símbolos quebrados; use `--ascii` ou troque a fonte do terminal.
- Se o texto aparecer cortado; o layout mínimo testado é 48×18. Aumente a janela.

**Condição de aceite:** a demonstração abre, as teclas `F2`, `F7`, `F8` e `F9` trocam de tela e o programa sai sem erro.

**Registro:** data e hora · placa e ID: não se aplica · versão · resultado · medido: não há · inesperado.

### [ ] RT 4 - Gerando e conferindo as credenciais

Prioridade: vital. É a causa do principal problema da bancada de 08/10: placas gravadas sem chave sem ninguém perceber.

**Comandos necessários**

```bash
./Codigo/scripts/provision_sensor_security.sh
./Codigo/scripts/provision_sensor_security.sh --check
ls -l .env.local ~/.config/iot-over-can/security.json
```

**Variações possíveis**

- `.env.local` antigo, sem a chave mestra: `--add-link-key`.
- Chaves de um único sensor: `--sensor <UUID>` (gera `.env.sensor-<UUID>.local`).
- Trocar tudo: `--renew`. Exige regravar todos os módulos e todos os sensores.

**Saídas esperadas**

- Primeira execução: `Credenciais locais provisionadas.`
- `--check`: `OK: .../.env.local` e `Credenciais locais válidas e protegidas.`
- Permissão `-rw-------` (600) nos dois arquivos.

**Se sair isso; faça isso**

- Se sair `Credenciais já existem. Nenhum arquivo foi alterado.`; está certo, siga para o `--check`.
- Se sair `ERRO: chave mestra do enlace ausente ou inválida`; rode `--add-link-key` e depois regrave os módulos.
- Se sair `ERRO: ... deve usar permissão 600`; `chmod 600 <arquivo>`.
- Se sair `AVISO: EDGE_NODE_PRESHARED_KEY (protocolo v5) não é mais utilizada`; é só um resto de versão antiga, pode remover a linha.

**Condição de aceite:** `--check` termina com `Credenciais locais válidas e protegidas.` Nenhum segredo aparece no terminal nem vai para o caderno.

**Registro:** data e hora · placa e ID: não se aplica · versão · resultado · medido: não há (nenhum segredo vai para o caderno) · inesperado.

### [ ] RT 5 - Rodando os testes automáticos no computador

Prioridade: vital.

**Comandos necessários**

```bash
./Codigo/scripts/test_native_firmware.sh
./Codigo/scripts/test_tui.sh
python3 -m unittest discover -s Simulacao/tests
python3 -m unittest discover -s Pesquisa/tests
python3 Codigo/scripts/versao.py --check
./Codigo/scripts/check_build_variants.sh
```

**Variações possíveis**

- Sem AddressSanitizer no compilador, o script pula as variantes com sanitizador e roda as demais.

**Saídas esperadas**

- `native firmware validation tests: PASS` na última linha do primeiro script.
- `pytest` sem falhas; os dois `unittest` terminam com `OK`. No commit `b8fcbab`, a suíte da estação terminava com `452 passed`, em cerca de 5 minutos; o número cresce a cada commit, então vale que não haja falha, não o total.
- `Versão 0.22.0: todas as cópias conferem e nenhum firmware a escreve no código.` (a versão é a do arquivo `VERSION`).
- `check_build_variants: 11 variantes compilam e a autenticacao sem chave e recusada` na última linha do último script.
- Entre os testes da estação estão os de operador e auditoria: `test_operators.py`, `test_operators_cli.py`, `test_audit.py`, `test_history_actor.py`, `test_history_actor_screen.py` e `test_app_operator.py`.

**Se sair isso; faça isso**

- Se um teste da estação que refaz as gravações falhar; as gravações de `Front/replays/` não batem com o firmware atual. Rode `./Codigo/scripts/record_network_scenarios.sh` e veja o `git diff`.
- Se a bancada virtual falhar; o erro informa o cenário e o instante simulado. Reproduza só ele (RT 6) com `--verbose`.
- Se falhar por `-Werror`; seu compilador é mais novo e achou um aviso novo. Anote o aviso; não é defeito de lógica.
- Se `versao.py --check` apontar uma cópia; rode `python3 Codigo/scripts/versao.py --write` e confira o `git diff`.
- Se `check_build_variants.sh` acusar uma variante; uma opção de compilação documentada deixou de compilar. Não grave uma placa com essa opção até corrigir.

**Condição de aceite:** os seis comandos terminam sem falha. Com isso, a lógica do firmware está verificada; o que resta é rádio, alimentação e tempos reais.

**Registro:** data e hora · placa e ID: não se aplica · versão · resultado · medido: a linha final de cada um dos seis comandos · inesperado.

### [ ] RT 6 - Rodando a bancada virtual como linha de base

Prioridade: recomendado. Dá os tempos de referência para comparar com a bancada física.

**Comandos necessários**

```bash
./Codigo/scripts/build_network_sim.sh /tmp/sim_network
/tmp/sim_network --list
/tmp/sim_network formation
/tmp/sim_network owner_failover --verbose
```

**Variações possíveis**

- Cenários úteis para comparar depois: `sensor`, `leader_failure`, `leader_failure_no_probe`, `handshake_slow_radio`, `restart_keeps_binding`, `acoustic_sensor`, `panel`, `bus_load`, `isolated_node` (RT 71) e `selftest` (RT 86).
- Com a autenticação dos comandos CAN: `IOC_SIM_CXXFLAGS="-DIOT_CAN_AUTH=1" ./Codigo/scripts/build_network_sim.sh /tmp/sim_auth`; aparece o cenário `can_auth` (RT 88).
- Laboratório interativo: `cd Front && .venv/bin/python -m pico_tui --sim --sim-binary /tmp/sim_network`.

**Saídas esperadas**

- Linhas da Probe no formato `@12345 [GW] ...`; com `--verbose`, a serial de cada módulo como `#N2@12345 ...`.
- Código de saída 0 em cada cenário.

**Se sair isso; faça isso**

- Se um cenário encerrar com erro; ele informa a verificação que falhou e o instante. Não vá para o hardware com um cenário quebrado.

**Condição de aceite:** todos os cenários listados terminam com sucesso. Anote do `owner_failover` e do `leader_failure` os instantes da falha e da recuperação: são a referência dos RT 33, 34 e 61. O `comparar_bancada.py` (RT 23, 61 e 68) faz essa comparação por você.

**Registro:** data e hora · placa e ID: não se aplica · versão · resultado · medido: instantes da falha e da recuperação no `owner_failover` e no `leader_failure` · inesperado.

### [ ] RT 7 - Inspecionando a bancada antes de energizar

Prioridade: vital.

**Comandos necessários**

Nenhum; multímetro e inspeção visual, com tudo desligado.

**Variações possíveis**

- Bancada de referência: Probe 00 e 3 módulos. O firmware aceita até 31.

**Saídas esperadas**

- MCP2515 de cada placa: SCK no GPIO 13, SO no 19, SI no 23, CS no 5, INT no 4.
- CAN_H com CAN_H e CAN_L com CAN_L em todas as placas, num cabo único (sem estrela).
- Cristal do módulo MCP2515 de 8 MHz (está gravado na cápsula).
- Sem continuidade entre 5 V e GND em nenhuma placa.
- Uma etiqueta com o ID em cada ESP32.
- Alimentação por hub USB com fonte própria ou fonte de 5 V com 3 A ou mais.

**Se sair isso; faça isso**

- Se o cristal for de 16 MHz; o firmware assume 8 MHz. A placa vai operar na taxa errada e encher o barramento de erros. Troque o módulo ou ajuste `MCP_QUARTZ_HZ` em `main.ino` só para essa placa.
- Se só houver power bank; não use. Três módulos passam de 1 A nos picos e reiniciam juntos.

**Condição de aceite:** ligações conferidas placa por placa e anotadas; fonte adequada disponível.

**Registro:** data e hora · placa e ID: cada placa da bancada · versão: não se aplica (nada gravado ainda) · resultado · medido: ligações conferidas, placa por placa, e a fonte usada · inesperado.

## Fase 1. Compilação

Compile tudo antes de gravar qualquer placa. Um erro de build encontrado aqui custa minutos; o mesmo erro descoberto com a bancada montada custa a sessão.

### [ ] RT 8 - Compilando a Probe 00

Prioridade: vital.

**Comandos necessários**

```bash
./Codigo/scripts/build_esp32_can_node.sh 0
```

**Variações possíveis**

- O ID também pode vir da variável: `IOT_NODE_ID=0 ./Codigo/scripts/build_esp32_can_node.sh`.

**Saídas esperadas**

- `Building iot-over-can ESP32 CAN node with IOT_NODE_ID=0`.
- Nenhum aviso de chave: a Probe 00 não participa do canal de dados e não recebe chave.
- `[SUCCESS]` do PlatformIO no fim.

**Se sair isso; faça isso**

- Se a primeira compilação demorar vários minutos; é o download da plataforma `espressif32` e das bibliotecas (ACAN2515, NimBLE). Precisa de internet só na primeira vez.
- Se falhar ao resolver `symlink://../common/ioc_link`; seu PlatformIO Core é antigo. Atualize com `pio upgrade`.
- Se faltar espaço na partição; confira se `board_build.partitions = min_spiffs.csv` continua em `platformio.ini`. BLE e Wi-Fi juntos não cabem na partição padrão de 1,3 MB.

**Condição de aceite:** `[SUCCESS]`, com o ID 0 confirmado na saída.

**Registro:** data e hora · placa e ID: Probe 00 (ID 0) · versão · resultado · medido: não há · inesperado.

### [ ] RT 9 - Compilando os módulos CAN

Prioridade: vital. Repita para cada ID que vai à bancada.

**Comandos necessários**

```bash
./Codigo/scripts/build_esp32_can_node.sh 1
./Codigo/scripts/build_esp32_can_node.sh 2
./Codigo/scripts/build_esp32_can_node.sh 3
```

**Variações possíveis**

- Sem os ensaios de falha (uso fora da bancada): `PLATFORMIO_BUILD_FLAGS="-D IOT_FAULT_INJECTION=0" ./Codigo/scripts/build_esp32_can_node.sh 1`.
- Sem a vigilância do líder pelos seguidores: `PLATFORMIO_BUILD_FLAGS="-D IOT_FOLLOWER_LEADER_WATCHDOG=0" ...`.
- Teste negativo de ID: `./Codigo/scripts/build_esp32_can_node.sh 32`.
- Teste negativo sem ID: `cd Codigo/node-can && pio run`.

**Saídas esperadas**

- `Building iot-over-can ESP32 CAN node with IOT_NODE_ID=<n>`.
- `iot-over-can: chave de enlace fornecida; plano de dados wireless habilitado`.
- ID 32: `ERRO: NODE_ID deve estar entre 0 e 31.` e código de saída 2.
- Sem ID: o build para com `IOT_NODE_ID não informado. Grave com ./Codigo/scripts/upload_esp32_can_node.sh ...`.

**Se sair isso; faça isso**

- Se sair `AVISO: chave de enlace ausente em .env.local; o plano de dados wireless ficará desabilitado.`; pare. Volte ao RT 4, rode `--add-link-key` e compile de novo. O build passa, mas o módulo nunca abrirá canal com sensor nenhum.
- Se sair `IOT_LINK_MASTER_KEY inválida: use 64 caracteres hexadecimais`; o `.env.local` foi editado à mão. Gere de novo com `--renew`.
- Se os dois testes negativos compilarem sem erro; a proteção contra ID duplicado (CH-057) regrediu. Registre como defeito.

**Condição de aceite:** `[SUCCESS]` para cada ID, sempre com a linha de chave fornecida; os dois testes negativos param com erro. Anote o tamanho do firmware (a 0.18 ocupava 1,06 MB dos 1,9 MB).

**Registro:** data e hora · placa e ID: cada ID compilado · versão · resultado · medido: tamanho do firmware · inesperado.

### [ ] RT 10 - Compilando o sensor de vibração

Prioridade: vital.

**Comandos necessários**

```bash
./Codigo/scripts/build_pico.sh
ls -lh Codigo/node-wifi/build/pico_w/edge_node_firmware.uf2
```

**Variações possíveis**

- Chaves só deste sensor (uso permanente): `./Codigo/scripts/build_pico.sh --sensor <UUID>`, depois de `provision_sensor_security.sh --sensor <UUID>`.
- O script aceita `pico2_w`, mas a integração contínua só compila o sensor de vibração para o Pico W. Trate o Pico 2 W como não validado aqui; a saída dele fica em `build/pico2_w/`, sem apagar a do Pico W.

**Saídas esperadas**

- `Compilando node-wifi para PICO_BOARD=pico_w`.
- `Enlace: chave mestra (bancada); para uso permanente compile com --sensor <UUID>`.
- `Build finalizado com sucesso.` e o caminho do `.uf2`.

**Se sair isso; faça isso**

- Se sair `Enlace: sem chave de enlace: o sensor anuncia por BLE, mas não abre sessão com Nodes`; falta a chave mestra no `.env.local` (RT 4).
- Se sair `AVISO: token serial não provisionado`; os comandos que alteram o sensor pela USB ficarão bloqueados para sempre nesse firmware. Corrija o `.env.local` e recompile.
- Se sair `ERRO: .env.sensor-<UUID>.local não existe.`; gere as chaves do sensor antes.
- Se o CMake não achar o SDK; confira `PICO_SDK_PATH` (RT 1).
- Se falhar ao ligar o BTstack; confira `ENABLE_PRINTF_HEXDUMP` em `btstack_config.h` (CH-007).

**Condição de aceite:** `.uf2` gerado e a linha `Enlace:` mostrando o modo de chave que você pretendia. Cada placa e cada sensor têm a sua pasta em `build/` (`pico_w/`, `pico2_w/`, `pico_w-<UUID>/`): um build não apaga o outro.

**Registro:** data e hora · placa e ID: sensor de vibração · versão · resultado · medido: modo de chave mostrado na linha `Enlace:` · inesperado.

### [ ] RT 11 - Compilando o sensor acústico

Prioridade: vital se houver sensor acústico na bancada.

**Comandos necessários**

```bash
./Codigo/scripts/build_pico.sh pico_w --audio
./Codigo/scripts/build_pico.sh pico2_w --audio
ls Codigo/node-audio/build/pico_w/edge_audio_firmware.uf2 Codigo/node-audio/build/pico2_w/edge_audio_firmware.uf2
```

**Variações possíveis**

- Com chaves do sensor: acrescente `--sensor <UUID>`.
- Pinos diferentes: `./Codigo/scripts/build_pico.sh pico_w --audio --mic-pins <SCK>,<SD>` (WS é sempre SCK + 1; o script recusa os GPIO 23, 24, 25 e 29, que são do rádio). A saída fica em `build/pico_w-mic<SCK>-<SD>/`.

**Saídas esperadas**

- `Compilando node-audio para PICO_BOARD=pico_w` (ou `pico2_w`), sem avisos de compilação.
- `Build finalizado com sucesso.`

**Se sair isso; faça isso**

- Se o build do `pico2_w` falhar por placa desconhecida; o Pico SDK é anterior ao suporte do RP2350. O projeto pede a 2.3.1.
- Se gravar o `.uf2` errado na placa errada; o Pico simplesmente não aceita ou não sobe. Por isso cada placa tem a sua pasta em `build/`.

**Condição de aceite:** um `.uf2` para cada placa que você tem, cada um na pasta da sua placa.

**Registro:** data e hora · placa e ID: cada placa do sensor acústico · versão · resultado · medido: caminho do `.uf2` de cada placa · inesperado.

### [ ] RT 12 - Compilando o painel

Prioridade: recomendado (vital se o painel for à bancada).

**Comandos necessários**

```bash
./Codigo/scripts/build_paper.sh
```

**Variações possíveis**

- Chaves só do painel: `./Codigo/scripts/build_paper.sh --sensor <UUID>`.
- Placa só com micro-USB (controlador ILI9341): acrescente `-D CYD_PANEL_ILI9341=1` em `build_flags` do ambiente `cyd`, em `Codigo/node-paper/platformio.ini`.
- Sem chave nenhuma o painel compila e ainda funciona pela USB com o Simulation Studio.

**Saídas esperadas**

- `[SUCCESS]` no ambiente `cyd`.

**Se sair isso; faça isso**

- Se sair `ERRO: .env.sensor-<UUID>.local ausente`; gere as chaves com `provision_sensor_security.sh --sensor <UUID>`.
- Se falhar por tamanho; confira `board_build.partitions = huge_app.csv`.

**Condição de aceite:** `[SUCCESS]`.

**Registro:** data e hora · placa e ID: painel · versão · resultado · medido: não há · inesperado.

### [ ] RT 13 - Compilando o leitor da documentação

Prioridade: opcional. O leitor não faz parte da rede.

**Comandos necessários**

```bash
./Codigo/scripts/build_leitor.sh
```

**Variações possíveis**

- Nenhuma de build. O texto vem do README e de `Documentacao/` no momento da compilação.

**Saídas esperadas**

- `tools/gerar_documentos.py` converte os `.md` em `src/documentos.h` e o build termina com `[SUCCESS]` no ambiente `papercolor`.

**Se sair isso; faça isso**

- Se a conversão falhar; algum `.md` tem uma construção que o conversor não entende. O erro aponta o arquivo.

**Condição de aceite:** `[SUCCESS]`.

**Registro:** data e hora · placa e ID: leitor · versão · resultado · medido: não há · inesperado.

## Fase 2. Gravação e primeiro boot

Uma placa por vez no USB, para não gravar o ID errado na placa errada. Depois de cada gravação, abra o monitor e confira o boot antes de passar à próxima.

```bash
pio device list                    # descobre a porta
pio device monitor -b 115200       # monitor serial (Ctrl+C sai)
```

### [ ] RT 14 - Gravando e verificando a Probe 00

Prioridade: vital.

**Comandos necessários**

```bash
./Codigo/scripts/upload_esp32_can_node.sh 0 <porta>
pio device monitor -b 115200 -p <porta>
```

No monitor, pressione o botão EN da placa para ver o boot desde o início e digite:

```text
PROBE_VERSION
PROBE_STATUS
```

**Variações possíveis**

- Sem a porta, o PlatformIO tenta detectá-la: `./Codigo/scripts/upload_esp32_can_node.sh 0`. Só use com uma única placa conectada.
- `VERSION` e `STATUS` são sinônimos na Probe; `GW_VERSION` e `GW_STATUS` existem por compatibilidade.

**Saídas esperadas**

- Boot: `Iniciando NODE 0`, `[NODE 0] [BOOT] FIRMWARE=<versão> COMMIT=<commit> PROTOCOL=CAN_CLASSIC_V1 ROLE=PROBE`, `[PROBE 00] CAPS=CAN_MONITOR,SERIAL_INSTRUMENTATION LOCAL_PROFILE=NONE`, `[GW] Probe 00 ativada`, `[GW] Interface serial da Probe 00 pronta`.
- `[PROBE 00] FIRMWARE=<versão> COMMIT=<commit> FEATURES=WIRELESS_DATA,ACOUSTIC_DATA,BUS_METRICS,FAULT_INJECTION,FAILOVER,RESEARCH,TIME_SYNC`.
- `PROBE_VERSION FIRMWARE=<versão> COMMIT=<commit> PROTOCOL=CAN_CLASSIC_V1 NODE=0 FEATURES=...`.
- `PROBE_STATUS NODE=0 STATE=ONLINE CAN=CLASSIC ARB=500000 DATA=0 WIFI=OFF BLE_SCAN=OFF UPTIME_MS=<n>`.

**Se sair isso; faça isso**

- Se sair `[NODE 0] [ERRO] Falha ao iniciar CAN: 0x<código>`; o ESP32 não conversa com o MCP2515 pelo SPI. Confira SCK 13, SO 19, SI 23, CS 5, INT 4, a alimentação do módulo e o cristal de 8 MHz. A placa trava nesse ponto de propósito, com os LEDs em estado de falha.
- Se o upload falhar em `Connecting....`; segure o botão BOOT durante a conexão, ou troque o cabo (muitos cabos são só de carga).
- Se o upload falhar no meio; a velocidade de 921600 baud é alta para alguns conversores USB. Reduza `upload_speed` para 460800 em `platformio.ini`.
- Se o monitor mostrar lixo; a velocidade não está em 115200.
- Se sair `rst:0xf (BROWNOUT_RST)`; alimentação fraca. Troque a porta USB ou o cabo.
- Se faltar algum item em `FEATURES`; a placa foi gravada com outra versão. A estação só usa o que a Probe anuncia.

**Condição de aceite:** as linhas acima, com `FIRMWARE=` igual ao arquivo `VERSION`, o `COMMIT=` do commit que você gravou (sem `-dirty`) e as sete `FEATURES`. Etiquete a placa como "00". Atalho: `python3 Codigo/scripts/smoke_boot.py probe --porta <porta>` confere tudo isto e termina em `SMOKE RESULT=PASS`.

**Registro:** data e hora · placa e ID: Probe 00 (ID 0) · versão: o `FIRMWARE=` e o `COMMIT=` mostrados no boot · resultado · medido: não há · inesperado.

### [ ] RT 15 - Gravando e verificando cada módulo CAN

Prioridade: vital. Repita para cada ID.

**Comandos necessários**

```bash
./Codigo/scripts/upload_esp32_can_node.sh 1 <porta>
pio device monitor -b 115200 -p <porta>
```

**Variações possíveis**

- Com opções de compilação: `PLATFORMIO_BUILD_FLAGS="-D IOT_FAULT_INJECTION=0" ./Codigo/scripts/upload_esp32_can_node.sh 1 <porta>`.

**Saídas esperadas**

- `Iniciando NODE <n>` com o número que você pediu.
- `[NODE <n>] [BOOT] FIRMWARE=<versão> COMMIT=<commit> PROTOCOL=CAN_CLASSIC_V1 ROLE=NODE`.
- `[LEDS] IDLE: todos acesos. Logica invertida: LOW=ligado, HIGH=desligado`, e os três LEDs acesos.
- `[NODE <n>] CAPS=CAN,LOCAL_SENSOR,LOCAL_SENSOR_DEMO,BLE_SCAN LOCAL_PROFILE=DEMO_BYTE`.
- `[NODE <n>] [WLINK] LINK_KEY=PROVISIONED`.
- `[NODE <n>] [INIT] Pronto. Aguardando eleicao manual ou heartbeat para JOIN`.

**Se sair isso; faça isso**

- Se sair `[WLINK] LINK_KEY=MISSING`; a placa foi gravada sem chave. `provision_sensor_security.sh --check`, corrija e grave de novo. Não siga com um módulo assim: ele associa sensores, mas responde `NO_KEY` e nunca abre o ponto de acesso.
- Se o número em `Iniciando NODE` não for o esperado; você gravou a placa errada. Regrave e corrija a etiqueta. Dois módulos com o mesmo ID disputam os mesmos identificadores CAN e o mesmo ponto de acesso.
- Se sair `[ERRO] Falha ao iniciar CAN`; mesmo diagnóstico do RT 14. Para separar defeito da placa de defeito do barramento, use o `SELFTEST` do RT 86.
- Se a placa reiniciar em laço logo após `LINK_KEY`; suspeite de alimentação: é quando os rádios ligam e a corrente sobe.

**Condição de aceite:** para cada módulo, ID correto, `LINK_KEY=PROVISIONED` e `[INIT] Pronto`, com `FIRMWARE=` e `COMMIT=` iguais aos da Probe. Atalho: `python3 Codigo/scripts/smoke_boot.py modulo --porta <porta> --no <n>` (com `--selftest` se o módulo ainda estiver fora da rede).

**Registro:** data e hora · placa e ID: cada módulo, com o ID · versão: o `FIRMWARE=` e o `COMMIT=` do `[BOOT]` · resultado · medido: não há · inesperado.

### [ ] RT 16 - Gravando e verificando o sensor de vibração

Prioridade: vital.

**Comandos necessários**

1. Segure BOOTSEL no Pico W e conecte o USB. Aparece a unidade `RPI-RP2`.
2. Copie o firmware e abra o console:

```bash
cp Codigo/node-wifi/build/pico_w/edge_node_firmware.uf2 /media/$USER/RPI-RP2/
pio device monitor -b 115200 -p /dev/ttyACM0
```

3. No prompt `EDGE>`:

```text
VERSION
STATUS
NET
```

**Variações possíveis**

- O ponto de montagem muda por sistema (`/run/media/$USER/RPI-RP2`, `/Volumes/RPI-RP2`).
- Sensor com chaves próprias: só depois de anotar o UUID com `VERSION` (RT 72).

**Saídas esperadas**

- Banner `EDGE DSP VIBRATION NODE` e `ASCII serial console ready. Type HELP and press Enter.`
- `VERSION FIRMWARE=<versão> COMMIT=<commit> PROTOCOL=6 NODE_UUID=0x<16 dígitos>`, e, se a porta estava aberta na partida, `BOOT FIRMWARE=<versão> COMMIT=<commit> PROTOCOL=6 PROFILE=VIBRATION`.
- `STATUS NET=DISABLED MODE=... ACQ=POLLING ... DTC=0x0000 DTC_COUNT=0 MPU=YES ... AUTH=LOCKED ... BLE_BEACON=ON`.
- `NET STATE=DISABLED WIFI=OFF PROVISIONED=NO BLE_BEACON=ON LINK_KEYS=MASTER PARENT_NODE=0 CHILD=0 AUTH_REJECT=0 REPLAY_REJECT=0`.

**Se sair isso; faça isso**

- Se a unidade `RPI-RP2` não aparecer; o cabo é só de carga, ou o BOOTSEL não estava pressionado ao conectar.
- Se sair `MPU=NO` ou `DTC=0x1001`; o MPU6050 não responde no I²C. Confira SDA em GP0, SCL em GP1, VCC em 3V3(OUT), GND, e o endereço `0x68` (pino AD0 em GND).
- Se sair `LINK_KEYS=NONE`; firmware compilado sem chave (RT 10).
- Se sair `BLE_BEACON=OFF`; o sensor não está anunciando e nenhum módulo vai encontrá-lo. Reinicie; se persistir, o rádio CYW43 não subiu (a placa é mesmo um Pico **W**?).
- Se o banner não aparecer; pressione Enter. Ele sai quando a USB conecta, e o monitor pode ter aberto depois.

**Condição de aceite:** `FIRMWARE=` igual ao arquivo `VERSION`, `PROTOCOL=6`, `MPU=YES`, `DTC=0x0000`, `LINK_KEYS=MASTER` e `BLE_BEACON=ON`. Anote o `NODE_UUID` no caderno e numa etiqueta no sensor. Atalho: `python3 Codigo/scripts/smoke_boot.py vibracao --porta <porta>`.

**Registro:** data e hora · placa e ID: sensor de vibração, com o `NODE_UUID` · versão: `FIRMWARE=` e `COMMIT=` do `VERSION` · resultado · medido: não há · inesperado.

### [ ] RT 17 - Gravando e verificando o sensor acústico

Prioridade: vital se houver sensor acústico.

**Comandos necessários**

```bash
cp Codigo/node-audio/build/pico_w/edge_audio_firmware.uf2 /media/$USER/RPI-RP2/      # Pico W
cp Codigo/node-audio/build/pico2_w/edge_audio_firmware.uf2 /media/$USER/RP2350/      # Pico 2 W
pio device monitor -b 115200 -p /dev/ttyACM0
```

```text
VERSION
STATUS
NET
```

**Variações possíveis**

- No Pico 2 W a unidade de BOOTSEL se chama `RP2350`.

**Saídas esperadas**

- `VERSION FIRMWARE=<versão> COMMIT=<commit> PROTOCOL=6 NODE_UUID=0x... PROFILE=ACOUSTIC CHIP=RP2040 BOARD=...` (ou `CHIP=RP2350`).
- `STATUS PROFILE=ACOUSTIC NET=DISABLED MODE=... ACQ=I2S RATE_HZ=16000.00 ... CHANNEL=... MIC=OK CPU_PCT=<n> COVERAGE_PCT=100 ... DTC=0x0000`.
- Configuração inicial: 16 kHz e FFT de 1024 no Pico W; 48 kHz e FFT de 2048 no Pico 2 W.

**Se sair isso; faça isso**

- Se sair `MIC=NO_HARDWARE`, `MIC=SILENT` ou `MIC=STUCK_HIGH`, ou uma linha `[MIC] ...: confira alimentação, SCK=GP18, WS=GP19, SD=GP20 e o pino L/R`; o microfone não entrega dados válidos: a captura I2S não iniciou (`NO_HARDWARE`: confira SCK, WS e SD, e se `--mic-pins` escolheu GPIO livres), só zeros (`SILENT`: SD aberto ou microfone sem VDD; confira VDD e o pino L/R) ou só uns (`STUCK_HIGH`: confira se SD está em curto com 3,3 V). Confira os quatro fios e o VDD em 3V3(OUT). Logo depois de gravar, `COVERAGE_PCT=0` é normal (o microfone ainda não mediu uma janela inteira), e `MIC=OK` ainda não prova nada: antes do primeiro relatório do DSP o firmware informa `OK` por falta de dado. Repita `STATUS` depois de 2 s.
- Se sair `[MIC] sem sinal no canal anterior; tentando o canal direito`; o pino L/R está em VDD ou solto. O firmware se adapta, mas ligue-o em GND para ficar no canal esquerdo.
- Se `COVERAGE_PCT` ficar abaixo de 100; o DSP não acompanha. Anote `CPU_PCT` e veja o RT 50.

**Condição de aceite:** `FIRMWARE=` igual ao arquivo `VERSION`, `PROFILE=ACOUSTIC`, o chip correto, `MIC=OK`, `COVERAGE_PCT=100` e `DTC=0x0000`. Anote o `NODE_UUID`. Atalho: `python3 Codigo/scripts/smoke_boot.py acustico --porta <porta>`.

**Registro:** data e hora · placa e ID: sensor acústico, com o chip e o `NODE_UUID` · versão · resultado · medido: `COVERAGE_PCT` · inesperado.

### [ ] RT 18 - Gravando e verificando o painel

Prioridade: recomendado.

**Comandos necessários**

```bash
./Codigo/scripts/build_paper.sh --upload <porta>
pio device monitor -b 115200 -p <porta>
```

**Variações possíveis**

- A USB-C dessa placa não liga com cabo USB-C nas duas pontas: use cabo USB-A para USB-C, ou a porta micro-USB.
- Refazer a calibração do toque: ligue a placa segurando BOOT.

**Saídas esperadas**

- Na tela, barras vermelha, verde, azul e branca, nessa ordem.
- Em seguida, marcadores nos quatro cantos para calibrar o toque.
- Na serial: `#IOC HELLO uuid=0x... profile=PANEL fw=... keys=... board=CYD` e, após a calibração, `#IOC LOG toque calibrado e gravado`.

**Se sair isso; faça isso**

- Se as cores saírem trocadas; ajuste `CYD_INVERT` ou `CYD_RGB_ORDER` em `build_flags`.
- Se a imagem sair de lado ou espelhada; ajuste `CYD_ROTATION` (0 a 7).
- Se a tela ficar branca ou com lixo; sua placa pode ser a variante ILI9341 (só micro-USB): `-D CYD_PANEL_ILI9341=1`.
- Se o toque responder no lugar errado depois; recalibre segurando BOOT ao ligar.

**Condição de aceite:** cores na ordem certa, imagem na orientação certa, toque calibrado e UUID do painel anotado.

**Registro:** data e hora · placa e ID: painel, com o UUID · versão · resultado · medido: não há · inesperado.

### [ ] RT 19 - Gravando e verificando o leitor da documentação

Prioridade: opcional.

**Comandos necessários**

```bash
./Codigo/scripts/build_leitor.sh --upload /dev/ttyACM0
pio device monitor -b 115200 -p /dev/ttyACM0
```

No monitor, digite `s`.

**Variações possíveis**

- Pela serial, `a`, `b`, `c` tocam os botões; `A`, `B`, `C` os seguram.

**Saídas esperadas**

- `#LEITOR documentação <versão>, <n> documentos; tela ...` no boot.
- `#LEITOR modo=... doc=... pagina=.../... secao=... teclado=... desenhando=... desenhos=...` em resposta ao `s`.
- A biblioteca desenhada na tela em 15 a 30 s, com o LED azul durante o desenho.

**Se sair isso; faça isso**

- Se a porta não aparecer; o ESP32-S3 usa USB nativo (`/dev/ttyACM*`), não `ttyUSB`.
- Se o upload não conectar; coloque a placa em modo de download conforme o manual do PaperColor.

**Condição de aceite:** biblioteca visível na tela e resposta ao `s` na serial.

**Registro:** data e hora · placa e ID: leitor · versão · resultado · medido: não há · inesperado.

## Fase 3. Barramento CAN

Aqui os sensores ficam desligados. O objetivo é provar que o barramento é saudável e que a rede se forma, antes de pôr rádio na história. Os comandos são digitados no monitor serial da Probe 00; a estação entra na Fase 4.

### [ ] RT 20 - Medindo a camada física do barramento

Prioridade: vital.

**Comandos necessários**

Nenhum; multímetro.

**Variações possíveis**

- Muitos módulos MCP2515 trazem um jumper (J1) que liga um resistor de 120 Ω na própria placa. Se usar os jumpers, feche-os só nas duas placas das pontas e dispense os resistores externos.

**Saídas esperadas**

- Tudo desligado, entre CAN_H e CAN_L: cerca de 60 Ω (dois resistores de 120 Ω em paralelo).
- Tudo ligado e em repouso: CAN_H e CAN_L perto de 2,5 V em relação ao GND, com diferença próxima de zero entre os dois.

**Se sair isso; faça isso**

- Se medir cerca de 120 Ω; falta uma terminação.
- Se medir 40 Ω ou menos; há terminações demais (jumpers fechados no meio do barramento).
- Se medir circuito aberto; não há terminação nenhuma, ou o cabo está interrompido.
- Se CAN_H ou CAN_L ficar em 0 V ou 5 V em repouso; um transceptor está sem alimentação ou há fios trocados. Desligue as placas uma a uma até a tensão voltar.

**Condição de aceite:** 60 Ω ± 5 Ω com tudo desligado e as duas linhas em torno de 2,5 V em repouso. Anote os valores.

**Registro:** data e hora · placa e ID: o barramento inteiro · versão: não se aplica · resultado · medido: resistência entre as duas linhas (Ω) e tensão de repouso de cada linha (V) · inesperado.

### [ ] RT 21 - Probe 00 sozinha no barramento (teste negativo)

Prioridade: recomendado. Ensina como o barramento se comporta quando ninguém confirma os quadros.

**Comandos necessários**

Só a Probe 00 energizada. No monitor dela:

```text
METRICS ON
22 20 FF 00
```

**Variações possíveis**

- `METRICS OFF` desliga a medição; `METRICS RESET` zera os acumulados.

**Saídas esperadas**

- `[GW] METRICS state=ON`.
- `[GW] Comando enviado: 22 20 ...` e nenhuma resposta de módulo.
- Linhas `[GW] BUS_STATS ...` a cada segundo, com `tx_err` crescendo: num barramento CAN, um quadro que ninguém confirma é retransmitido e conta como erro.

**Se sair isso; faça isso**

- Se `tx_err` ficar em zero com a Probe sozinha; há outra placa energizada no barramento sem você saber.
- Se aparecer `eflg=0x20`; o controlador da Probe saiu do barramento (*bus-off*). Reinicie a Probe depois deste teste.

**Condição de aceite:** nenhuma resposta e `tx_err` maior que zero. O teste passa porque falha: é a assinatura de "estou sozinho ou sem terminação", que você vai reconhecer depois.

**Registro:** data e hora · placa e ID: Probe 00 · versão · resultado · medido: valor de `tx_err` · inesperado.

### [ ] RT 22 - Probe 00 e um módulo: primeiro quadro confirmado

Prioridade: vital. Repita com cada módulo, um por vez.

**Comandos necessários**

Probe 00 e um único módulo energizados. Reinicie a Probe e, no monitor dela:

```text
METRICS ON
22 20 FF 00
```

**Variações possíveis**

- Consulta a um módulo só: `22 20 01 00` (ID em hexadecimal: módulo 10 é `0A`).

**Saídas esperadas**

- `[GW] No descoberto: NODE <n>` quando a Probe ouve o módulo pela primeira vez.
- `[GW] Atualizacao de status recebida: [STATUS] NODE <n> estado=...`.
- `[GW] BUS_STATS ... rx_err=0 tx_err=0 eflg=0x00`.

**Se sair isso; faça isso**

- Se não houver resposta e `tx_err` crescer; o módulo não está no barramento. Confira CAN_H e CAN_L (invertidos é o erro mais comum) e o GND comum.
- Se `rx_err` crescer; há quadros chegando corrompidos: cristal de 16 MHz numa das placas, terminação faltando ou cabo longo sem trança.
- Se só este módulo falhar e os outros passarem; o defeito é dele (transceptor ou solda), não do barramento.

**Condição de aceite:** cada módulo, sozinho com a Probe, responde à consulta com os contadores de erro em zero.

**Registro:** data e hora · placa e ID: Probe 00 e o módulo em teste (uma linha por módulo) · versão · resultado · medido: contadores de erro de cada módulo · inesperado.

### [ ] RT 23 - Formando a rede: eleição do líder

Prioridade: vital.

**Comandos necessários**

Probe 00 e todos os módulos energizados. No monitor da Probe:

```text
22 00 FF 01
```

**Variações possíveis**

- A Probe aceita o mesmo comando em outras grafias (`22 0 FF 1`, `0x22 00 FF 01`).
- Pela estação: Comandos (`F4`), Iniciar eleição.

**Saídas esperadas**

- `[GW] Comando enviado: 22 00 FF 01`.
- `[GW] ELEICAO recebida de NODE <n>` para cada candidato.
- `[GW] Lider anunciado: NODE <maior ID>`.
- Depois, a cada segundo: `[GW] HEARTBEAT lider=<maior ID> rodada=<n> sensores_ativos=<n> modo=3 periodo=1000 ms t_us=...`.
- No monitor de um módulo: `[ELEICAO] Iniciada` e, no vencedor, `[RECUPERACAO] Eleito lider. Reconstruindo estado` seguido de `[LIDER] Recuperacao concluida. Heartbeat iniciado`.
- LEDs: líder (GPIO 25) aceso só no vencedor; estado (GPIO 27) piscando 80 ms a cada sinal de presença; falha (GPIO 26) apagado em todos.

**Se sair isso; faça isso**

- Se o líder não for o de maior ID; aquele módulo não participou. Veja no monitor dele se a eleição chegou (`[ELEICAO] Comando recebido`) ou se ele está em falha (`[ELEICAO] Ignorada: no em falha`).
- Se aparecer `[CONFLITO] Lider com maior ID detectado`; dois líderes coexistiram por um instante e a regra do maior ID resolveu. Uma vez é normal; repetido, indica perda de quadros.
- Se nenhum líder surgir em 10 s; repita o comando e acompanhe `BUS_STATS`. Erros no barramento impedem a eleição.
- Se algum módulo reiniciar quando todos são ligados juntos (`rst:0xf (BROWNOUT_RST)` no monitor); a fonte não sustenta a partida. Resolva agora, com fonte de 5 V e 3 A ou mais: é a primeira vez que a bancada inteira liga ao mesmo tempo. O teste formal de alimentação é o RT 71.

**Condição de aceite:** em ≈ 7,5 s a rede tem um único líder, o de maior ID, e o sinal de presença sai a cada 1000 ms. Anote o tempo medido entre o comando e o primeiro `HEARTBEAT`.

**Registro:** data e hora · placa e ID: Probe 00 e todos os módulos · versão · resultado · medido: tempo do comando ao primeiro `HEARTBEAT` (s) e ID do líder eleito · inesperado.

### [ ] RT 24 - Entrada de um módulo com a rede já formada

Prioridade: vital.

**Comandos necessários**

Com a rede formada, desligue um seguidor, espere 10 s e religue-o, com o monitor dele aberto. Não envie comando nenhum.

**Variações possíveis**

- Faça também com um módulo de ID **maior** que o do líder atual (forme a rede sem ele e ligue-o depois).

**Saídas esperadas**

- No módulo: `[JOIN] Heartbeat observado. Solicitando entrada ao lider NODE <n>`, `[JOIN TX] Solicitacao enviada. Tentativa=1`, `[JOIN RX] Entrada aceita. Aguardando heartbeat do lider NODE <n>` e `[JOIN] Primeiro heartbeat recebido. No integrado como FOLLOWER`.
- Na Probe: `[GW] JOIN observado de NODE <n>`.
- Na variação: o líder **não** muda. A chegada de um módulo novo não troca o líder em exercício.

**Se sair isso; faça isso**

- Se sair `[JOIN] Sem resposta. Retornando para IDLE`; o líder não ouviu o pedido. O módulo tenta de novo no próximo sinal de presença; se nunca entrar, volte ao RT 22 com ele.
- Se o líder tiver marcado esse módulo em falha enquanto esteve fora; a falha continua registrada depois da volta. É o comportamento previsto: limpe com `22 10 <ID> 44` (RT 32).

**Condição de aceite:** o módulo entra sozinho como seguidor, sem eleição nova e sem trocar o líder.

**Registro:** data e hora · placa e ID: módulo que entrou · versão · resultado · medido: não há · inesperado.

### [ ] RT 25 - Mudando o período do sinal de presença

Prioridade: recomendado.

**Comandos necessários**

```text
22 30 FF 01
22 30 FF 05
22 30 FF 03
```

**Variações possíveis**

- Modos `01` a `05`: 2000, 1500, 1000, 750 e 500 ms.
- Teste negativo de alvo: `22 30 01 03`.
- Teste negativo de modo: `22 30 FF 09`.

**Saídas esperadas**

- O campo `periodo=` do `[GW] HEARTBEAT` acompanha: 2000, depois 500, depois 1000 ms.
- Alvo errado: `[CTRL] Frequencia do heartbeat deve ser alterada apenas em broadcast: 22 30 FF modo`.
- Modo errado: `[CTRL] Modo de heartbeat invalido`.

**Se sair isso; faça isso**

- Se o período mudar na Probe e algum módulo for marcado em falha logo depois; ele não recebeu a mudança e ficou fora de compasso. Anote: é um defeito a registrar.

**Condição de aceite:** os três períodos são aplicados, os dois testes negativos são recusados e a rede termina em 1000 ms (modo `03`). Os prazos de vigilância dos RT 32 a 34 são contados em sinais de presença: com outro período, todos os tempos mudam.

**Registro:** data e hora · placa e ID: Probe 00 e todos os módulos · versão · resultado · medido: os três períodos aplicados e o período final · inesperado.

### [ ] RT 26 - Tirando as métricas de base do barramento

Prioridade: vital. É a referência para tudo o que vem depois.

**Comandos necessários**

```text
METRICS RESET
METRICS ON
```

Deixe rodar 60 s, sem sensores.

**Variações possíveis**

- Pela estação: Métricas da rede (`F8`), `x` exporta para `exports/`.

**Saídas esperadas**

- `[GW] BUS_STATS t_ms=... win_ms=1000 frames=... load=<≈1> peak=... rx_err=0 tx_err=0 eflg=0x00 rx_peak=... ids=... xfer_ok=... xfer_err=0 own=... untracked=0`.
- A cada 5 s: `[GW] ID_STATS id=0x100 win_ms=5000 n=5 ... dt_min_us=... dt_avg_us=<≈1000000> dt_max_us=...`.

**Se sair isso; faça isso**

- Se `load` passar muito de 1 % sem sensores; há retransmissões. Olhe `rx_err` e `tx_err`.
- Se `untracked` for maior que zero; há mais de 48 identificadores ativos, o que não deveria ocorrer numa bancada pequena.
- Se `dt_max_us − dt_min_us` passar de alguns milissegundos; anote. Cerca de 1 ms é a incerteza da própria Probe; acima disso é atraso real do líder.

**Condição de aceite:** ocupação da ordem de 1 %, período médio de `0x100` em 1000 ms e todos os contadores de erro em zero por 60 s. Anote `load`, `peak` e os três `dt_` de `0x100`.

**Registro:** data e hora · placa e ID: Probe 00 e todos os módulos · versão · resultado · medido: `load`, `peak` e os três `dt_` de `0x100` · inesperado.

### [ ] RT 27 - Função local de demonstração e comandos de administração

Prioridade: recomendado.

**Comandos necessários**

```text
22 10 01 00
22 20 01 00
22 10 01 11
```

**Variações possíveis**

- Teste negativo: `22 10 FF 00` (alvo de rede inteira num comando de módulo).

**Saídas esperadas**

- Em operação normal: `[GW] SENSOR sensor=<n> rodada=<n> valor=0xAA ativo=1`, um por módulo a cada ciclo.
- Após `22 10 01 00`: `[GW] CMD_ACK node=1 ... result=APPLIED`; no módulo, `[CTRL RX] Sensor desativado manualmente`; o estado consultado passa a `DESATIVADO`.
- Após `22 10 01 11`: `[CTRL RX] Sensor ativado` e as linhas `SENSOR sensor=1` voltam.
- Negativo: `[CTRL] Comando de sensor nao pode usar alvo broadcast`.

**Se sair isso; faça isso**

- Se sair `[CTRL RX] ON ignorado: use CLEAR_FAULT para limpar falha`; o módulo está em falha. Use `22 10 01 44`.
- Se `valor` não for `0xAA`; os dados estão chegando corrompidos. Volte ao RT 26.

**Condição de aceite:** o byte `0xAA` chega de todos os módulos; desativar e reativar funciona e é confirmado com `APPLIED`.

**Registro:** data e hora · placa e ID: Probe 00 e cada módulo · versão · resultado · medido: não há · inesperado.

### [ ] RT 28 - Ajustando e consultando o relógio da rede

Prioridade: recomendado.

**Comandos necessários**

Num terminal do computador, gere a hora em milissegundos; depois, no monitor da Probe:

```bash
date +%s%3N
```

```text
TIME
TIME <milissegundos>
TIME
```

**Variações possíveis**

- O mesmo `TIME`, sem argumento, digitado no monitor de um módulo responde com a etiqueta `[RELOGIO]`.
- Teste negativo: `TIME abc`.
- A estação faz o ajuste sozinha ao conectar e a cada 10 min.

**Saídas esperadas**

- Antes do ajuste: `[GW] TIME fonte=... epoch=NA`.
- No ajuste: `[GW] TIME_SYNC node=0 fonte=TERMINAL epoch=<s.ms> ajuste_ms=NA` (o número aparece a partir do segundo ajuste).
- No módulo: `[RELOGIO] TIME fonte=TERMINAL ...` com a mesma hora.
- Negativo: `[GW] TIME_ERROR uso: TIME <milissegundos UTC desde 1970>`.

**Se sair isso; faça isso**

- Se o módulo continuar com `epoch=NA`; o quadro `0x120` não chegou. Um módulo sem hora a pede 5 s depois de ligar: procure `[GW] TIME_REQ node=<n>` na Probe.
- Se `date +%s%3N` não funcionar (macOS); use `python3 -c 'import time; print(int(time.time()*1000))'`.

**Condição de aceite:** depois do ajuste, Probe e módulos mostram a mesma hora, com fonte `TERMINAL`. Anote o `ajuste_ms` de um segundo ajuste feito minutos depois: é a deriva do relógio da placa.

**Registro:** data e hora · placa e ID: Probe 00 e todos os módulos · versão · resultado · medido: `ajuste_ms` do segundo ajuste e o intervalo entre os dois ajustes · inesperado.

## Fase 4. Módulos CAN, um por um

Agora a estação entra em cena, e cada módulo passa pelos mesmos testes. Mantenha aberto o monitor serial do módulo em teste (a estação ocupa só a porta da Probe). Os sensores continuam desligados.

### [ ] RT 29 - Conectando a estação à Probe 00

Prioridade: vital.

**Comandos necessários**

Feche o monitor serial da Probe e rode:

```bash
SECURITY_MODE=off ./Codigo/scripts/run_tui.sh <porta da Probe> --record-session
```

**Variações possíveis**

- Modo padrão (`presence`): sem `SECURITY_MODE=off`, os comandos que alteram a rede exigem a YubiKey conectada (RT 73).
- Sem informar a porta: `F3` abre o seletor.
- Detecção automática falhando: `cd Front && .venv/bin/iot-over-can-tui --port <porta> --mode probe --security-mode off --record-session`. Entre em `Front/` antes: o histórico é gravado na pasta em que a estação é aberta.
- Identificação do operador: `OPERATOR_MODE=auto` (padrão), `required` ou `off`, na mesma linha do `SECURITY_MODE` (RT 77).

**Saídas esperadas**

- Tela Início com todos os módulos, um líder e a condição geral normal.
- Na faixa de status: `Segurança desligada` e `Operador <sua conta> (sistema)`. Sem operadores cadastrados, quem responde pelos comandos é a conta do Linux.
- Rede CAN (`F5`): taxa de 500 kbit/s, contadores de erro em zero e os quadros recentes passando.
- O relógio da rede é ajustado sozinho: em Mensagens (`F6`) aparece o ajuste de hora.
- Um arquivo `Front/logs/gravacao_<data>_<sessão>.log` começa a ser escrito, e a estação avisa em Mensagens (`Gravando a sessão em ...`). Esse arquivo só existe com `--record-session`: a gravação integral é opcional porque ocupa muito disco.

**Se sair isso; faça isso**

- Se não houver arquivo `gravacao_*.log`; faltou `--record-session`. Sem ele, o comparador (`comparar_bancada.py`) e os RT 67, 81 e 89 não têm o que ler. Feche a estação e abra de novo com a opção.
- Se a estação não encontrar a porta; há um monitor serial aberto nela, ou falta o grupo `dialout`.
- Se a tela Início não mostrar líder; Comandos (`F4`), Iniciar eleição.
- Se faltar um módulo; ele não está transmitindo. Veja o monitor dele e volte ao RT 22.
- Se um recurso aparecer como indisponível; a Probe não o anunciou em `FEATURES` (versão diferente).
- Se a faixa de status mostrar `Operador sem login`; já existe um cadastro de operadores nesta conta. Tecle `o` e entre (RT 75). Sem isso, todo comando que altera a rede é recusado com `Identifique-se para alterar a rede: tecle o ou use :login.`

**Condição de aceite:** a estação mostra todos os módulos, o líder correto e o barramento sem erros. Guarde o nome do arquivo de gravação desta sessão no caderno.

**Registro:** data e hora · placa e ID: Probe 00 · versão · resultado · medido: nome do arquivo de gravação da sessão · inesperado.

### [ ] RT 30 - Conferindo os LEDs de cada módulo

Prioridade: opcional (os LEDs são opcionais na montagem).

**Comandos necessários**

Nenhum; observe cada placa nos estados abaixo.

**Variações possíveis**

- Os LEDs acendem com o pino em nível **baixo**: ligue cada LED entre 3V3 e o GPIO, com resistor.

**Saídas esperadas**

| Situação | Líder (GPIO 25) | Falha (GPIO 26) | Estado (GPIO 27) |
| --- | --- | --- | --- |
| Recém-ligado, sem rede (IDLE) | aceso | aceso | aceso |
| Seguidor | apagado | apagado | pisca 80 ms por sinal de presença |
| Líder | aceso | apagado | pisca 80 ms por sinal de presença |
| Em falha | apagado | aceso | conforme o estado |
| Probe 00 | apagado | aceso depois de pedir uma eleição por falta do líder | pisca por sinal de presença |

**Se sair isso; faça isso**

- Se os LEDs ficarem invertidos (apagados quando deveriam acender); estão ligados ao GND. O firmware assume lógica invertida.
- Se os três ficarem acesos para sempre; o módulo não saiu do IDLE: não ouviu sinal de presença nem eleição.

**Condição de aceite:** cada placa mostra o padrão da tabela nos estados que você conseguir provocar.

**Registro:** data e hora · placa e ID: cada placa · versão · resultado · medido: estados provocados e o padrão de LED visto em cada um · inesperado.

### [ ] RT 31 - Consultando o estado de um módulo

Prioridade: vital. Repita para cada módulo.

**Comandos necessários**

Na estação, lançador de comandos (`k`):

```text
22 20 01 00
22 20 FF 00
```

**Variações possíveis**

- Pela tela: Início, `Enter` no módulo, `s` pede o estado atualizado.
- Teste negativo: `22 20 01 05` (a consulta só aceita a ação `00`).

**Saídas esperadas**

- Resposta `23 21 <ID> <estado>`: `00` líder, `01` seguidor, `02` desativado, `03` em falha, `04` Probe, `05` entrando, `06` conflito de líder.
- A tela do módulo mostra papel, estado no barramento, firmware e tempo ligado.
- Negativo: `[CTRL] Requisicao de status deve usar acao 00`.

**Se sair isso; faça isso**

- Se um módulo responder `06`; ele ainda se considera líder junto com outro. Deve se resolver em um ciclo; se persistir, peça nova eleição.
- Se responder `05` por mais de alguns segundos; o pedido de entrada não foi aceito (RT 24).

**Condição de aceite:** cada módulo responde com o estado coerente com o papel dele, e exatamente um responde `00`.

**Registro:** data e hora · placa e ID: cada módulo · versão · resultado · medido: código de estado respondido por cada módulo · inesperado.

### [ ] RT 32 - Seguidor some: detecção pelo líder e limpeza da falha

Prioridade: vital.

**Comandos necessários**

1. Com o monitor do líder aberto, tire a alimentação de um seguidor e marque o tempo.
2. Depois da detecção, religue-o.
3. Limpe a falha (ID em hexadecimal):

```text
22 10 01 44
```

**Variações possíveis**

- Em vez de desligar a placa, solte só o CAN_H dela: o módulo continua vivo, mas isolado.
- Pela estação: Comandos (`F4`), alvo no módulo, Limpar falha.
- Teste negativo: `22 10 01 33` enviado pela Probe.

**Saídas esperadas**

- No líder, depois de 5 sinais de presença (≈ 5 s): `[NODE <líder>] [OMISSAO] NODE <n> ausente por mais de 5 heartbeats. Marcado como FALHA`.
- Na estação: o módulo passa a crítico e entra em Precisa de atenção.
- Depois de religar: o módulo volta, mas a falha **continua registrada**.
- Após `22 10 01 44`: no módulo, `[CTRL RX] Falha limpa e sensor reativado`; na estação, condição normal.
- Negativo: `[CTRL] Desativacao por falha so pode ser enviada pelo lider`.

**Se sair isso; faça isso**

- Se a detecção demorar muito mais que 5 períodos; confira o período em vigor (RT 25).
- Se a falha sumir sozinha quando o módulo volta; registre como defeito: ela só deveria sair com o comando do operador.
- Se seguidores saudáveis forem marcados em falha sem motivo; o líder está perdendo quadros deles. Volte ao RT 26.

**Condição de aceite:** falha detectada em ≈ 5 sinais de presença, mantida depois da volta do módulo e removida só pelo comando. Anote o tempo de detecção.

**Registro:** data e hora · placa e ID: módulo desligado e líder · versão · resultado · medido: tempo de detecção (s) · inesperado.

### [ ] RT 33 - Líder cai, com a Probe 00 presente

Prioridade: vital.

**Comandos necessários**

Tire a alimentação do líder e marque o tempo até o primeiro sinal de presença do novo líder.

**Variações possíveis**

- Repita com cada módulo no papel de líder, para provar que nenhum depende de hardware específico.

**Saídas esperadas**

- Na Probe, depois de 4 sinais perdidos: `[GW] Falha detectada no lider NODE <n>` e `[GW] Comando enviado: 22 00 FF 01`.
- `[GW] Lider anunciado: NODE <maior ID restante>` e a volta do `[GW] HEARTBEAT`.
- Na estação: uma eleição registrada em Métricas (`F8`) e o antigo líder como ausente.

**Se sair isso; faça isso**

- Se saírem duas eleições seguidas; a Probe e um seguidor pediram cada um a sua (CH-032 dizia estar corrigido). Anote os tempos e os monitores.
- Se o novo líder não for o de maior ID restante; veja se o maior estava em falha.

**Condição de aceite:** novo líder em ≈ 11,5 s (4 sinais, mais ≈ 7,5 s de eleição), um só, o de maior ID restante. Anote o tempo e compare com o `leader_failure` do RT 6.

**Registro:** data e hora · placa e ID: líder que caiu e novo líder · versão · resultado · medido: tempo até o novo líder (s) · inesperado.

### [ ] RT 34 - Líder cai, sem a Probe 00 (vigilância dos seguidores)

Prioridade: vital. É um dos itens que o repositório lista como pendente de bancada física.

**Comandos necessários**

1. Feche a estação e tire a alimentação da Probe 00.
2. Abra o monitor serial de um seguidor.
3. Tire a alimentação do líder e marque o tempo.

**Variações possíveis**

- Módulos gravados com `-D IOT_FOLLOWER_LEADER_WATCHDOG=0`: a rede deve ficar **sem** líder até alguém pedir a eleição.

**Saídas esperadas**

- No seguidor de maior ID, depois de 6 sinais perdidos: `[NODE <n>] [VIGILANCIA] Lider NODE <x> sem heartbeat ha <ms> ms. Nova eleicao solicitada`.
- Em seguida `[ELEICAO] Iniciada` e um novo líder emitindo sinal de presença.
- O seguidor de maior ID reage primeiro; os demais esperam 20 ms a mais por posição abaixo do ID 31.

**Se sair isso; faça isso**

- Se nenhuma eleição acontecer; os módulos foram compilados com a vigilância desligada, ou o seguidor também parou de ouvir o barramento.
- Se aparecer `[LACO] Parado por <ms> ms. Vigilancias rearmadas`; o laço do próprio módulo travou por mais de um período e ele rearmou as contagens em vez de acusar os outros. Anote a duração: não deveria ocorrer em operação normal.

**Condição de aceite:** novo líder em ≈ 14 s, sem a Probe. Anote o tempo: é a prova de que a Probe 00 é só um instrumento.

**Registro:** data e hora · placa e ID: líder que caiu e novo líder · versão · resultado · medido: tempo até o novo líder (s) · inesperado.

### [ ] RT 35 - Antigo líder volta à rede

Prioridade: vital.

**Comandos necessários**

Depois do RT 33 ou 34, religue o módulo que era líder, com o monitor dele aberto.

**Variações possíveis**

- O caso difícil (líder que congela e retoma achando que ainda é líder) só se provoca com o ensaio `FAULT SILENCE` (RT 69).

**Saídas esperadas**

- O módulo reinicia, ouve o sinal de presença e entra como seguidor (mesmas linhas `[JOIN]` do RT 24), ainda que tenha o maior ID.
- A rede continua com um único líder.

**Se sair isso; faça isso**

- Se o líder trocar quando ele volta; registre como defeito: a chegada de um módulo não deveria trocar o líder em exercício.
- Se aparecer `[CONFLITO] Lider com menor ID detectado. Nova eleicao solicitada`; houve dois líderes. Acompanhe até sobrar um.

**Condição de aceite:** o antigo líder volta como seguidor e a rede termina com um líder só.

**Registro:** data e hora · placa e ID: módulo que voltou · versão · resultado · medido: não há · inesperado.

### [ ] RT 36 - Comandos seriais digitados direto num módulo

Prioridade: opcional. O tratador de comandos seriais é o mesmo código na Probe e nos módulos; este RT documenta o que um módulo comum aceita pela própria USB.

**Comandos necessários**

No monitor serial de um módulo (não da Probe):

```text
TIME
22 20 FF 00
PROBE_VERSION
abc
```

**Variações possíveis**

- Experimente no líder e num seguidor: alguns comandos só o líder pode emitir.

**Saídas esperadas**

- `TIME`: `[RELOGIO] TIME fonte=... ...`.
- `22 20 FF 00`: `[CTRL TX] Comando enviado: 22 20 ...` (etiqueta `[CTRL TX]` no módulo, `[GW]` na Probe).
- `PROBE_VERSION`: não é reconhecido como introspecção num módulo; cai no leitor hexadecimal e sai `[CTRL] Formato invalido. Use: opcode subcmd alvo acao`.
- `abc`: a mesma mensagem de formato inválido.

**Se sair isso; faça isso**

- Se um módulo comum aceitar `WIRELESS BIND`, `FAULT` ou `CMD` pela USB; anote exatamente o que aconteceu. É comportamento a decidir (ver propostas de melhoria): qualquer pessoa com acesso à USB de um módulo estaria operando a rede.

**Condição de aceite:** você tem anotado quais comandos um módulo comum aceita pela USB e quais recusa.

**Registro:** data e hora · placa e ID: módulo usado · versão · resultado · medido: lista dos comandos aceitos e dos recusados · inesperado.

## Fase 5. Sensores, um por um

Cada sensor é testado sozinho, pelo console USB (115200 baud, prompt `EDGE>`), com os módulos desligados ou longe. Assim um defeito de fiação ou de aquisição aparece antes de se misturar com rádio. Comandos de leitura são livres; os que alteram o sensor exigem `AUTH UNLOCK`.

### Sensor de vibração (Pico W + MPU6050)

### [ ] RT 37 - Console, ajuda e identidade do sensor de vibração

Prioridade: vital.

**Comandos necessários**

```text
HELP
PING
VERSION
STATUS
GET
NET
xyz
```

**Variações possíveis**

- `MENU` é sinônimo de `HELP`. Os comandos não diferenciam maiúsculas de minúsculas.

**Saídas esperadas**

- `HELP`: a lista de comandos e as faixas (`RATE=4..1000 Hz, WINDOW_SIZE=128|256|512`).
- `PING`: `PONG UPTIME_MS=<n>`.
- `VERSION`: `VERSION PROTOCOL=6 NODE_UUID=0x...`.
- `STATUS`: uma linha com `NET`, `MODE`, `ACQ`, `WINDOW`, `WINDOW_SIZE`, `RATE_HZ`, `STALTA`, `GAIN`, `DTC`, `DTC_COUNT`, `MPU`, `TELEMETRY`, `PERIOD_MS`, `BATT_PCT`, `BATT_MV`, `AUTH`, `WIFI_PROVISIONED`, `BLE_BEACON`.
- `GET`: `STAGED MODE=... RATE_HZ=... WINDOW=... WINDOW_SIZE=... STALTA=... GAIN=...` e `NOTE Use APPLY to commit staged values.`
- `xyz`: `ERR Unknown command. Use HELP.`

**Se sair isso; faça isso**

- Se `BATT_PCT=255` e `BATT_MV=65535`; significa "sem medida", não bateria cheia. É o normal alimentado pela USB.
- Se `UPTIME_MS` voltar a um valor pequeno entre dois `PING`; o sensor reiniciou sozinho. Suspeite de alimentação ou do *watchdog*.
- Se sair `ERR Line too long. Buffer cleared.`; você colou texto demais de uma vez.

**Condição de aceite:** todos os comandos de leitura respondem no formato acima e o comando inválido é recusado. Copie a linha `STATUS` inteira para o caderno: é a configuração de fábrica.

**Registro:** data e hora · placa e ID: sensor de vibração (UUID) · versão · resultado · medido: linha `STATUS` inteira · inesperado.

### [ ] RT 38 - Sessão de manutenção (AUTH)

Prioridade: vital.

**Comandos necessários**

O token está em `.env.local`, na linha `EDGE_SERIAL_ADMIN_TOKEN`. Não o copie para o caderno.

```text
AUTH STATUS
SET MODE STRUCTURAL
AUTH UNLOCK 12345
AUTH UNLOCK <token>
AUTH STATUS
AUTH LOCK
```

**Variações possíveis**

- Deixe a sessão aberta e repita `AUTH STATUS` depois de 5 min: ela expira sozinha (300 000 ms).

**Saídas esperadas**

- `AUTH STATE=LOCKED PROVISIONED=YES LEASE_MS=0`.
- `SET` com a sessão fechada: `ERR AUTH_REQUIRED`.
- Token errado: `ERR AUTH_REJECTED`.
- Token certo: `OK AUTH_UNLOCKED`, e `AUTH STATE=UNLOCKED ... LEASE_MS=<perto de 300000>`.
- `AUTH LOCK`: `OK AUTH_LOCKED`.

**Se sair isso; faça isso**

- Se sair `PROVISIONED=NO` ou `ERR AUTH_NOT_PROVISIONED`; o firmware foi compilado sem token (aviso do RT 10). Nenhum comando de alteração vai funcionar: recompile e regrave.
- Se o token certo for recusado; o sensor foi gravado com um `.env.local` diferente do atual (por exemplo, antes de um `--renew`).

**Condição de aceite:** alteração recusada sem sessão, token errado recusado, token certo aceito e a sessão expirando em 5 min.

**Registro:** data e hora · placa e ID: sensor de vibração (UUID) · versão · resultado · medido: tempo até a sessão expirar (o token não vai para o caderno) · inesperado.

### [ ] RT 39 - Telemetria local e sanidade do MPU6050

Prioridade: vital.

**Comandos necessários**

Com a sessão aberta (`AUTH UNLOCK <token>`):

```text
TELEMETRY ONCE
TELEMETRY ON
TELEMETRY FAST
TELEMETRY PERIOD 500
TELEMETRY SLOW
TELEMETRY OFF
```

**Variações possíveis**

- `TEL` é sinônimo de `TELEMETRY`. Com o fluxo ligado, `!` ou `Ctrl+C` o interrompe.
- `FAST` é 250 ms, `SLOW` é 1000 ms, `PERIOD` aceita de 100 a 10000 ms.
- Teste negativo: `TELEMETRY PERIOD 50`.

**Saídas esperadas**

- `TEL MODE=... ACQ=POLLING WIN=512 AXIS=VECTOR FFT_VALID=YES RMS=... KURT=... CREST=... PEAK_HZ=... PEAK_AMP=... ENT=... PPV_MM_S=... STA_LTA=NO CLIP=NO BATT_PCT=... BATT_MV=... DTC=0x0000 DTC_COUNT=0`.
- Sensor parado sobre a mesa: `RMS` baixo e estável entre amostras.
- Batendo de leve na mesa: `RMS`, `CREST`, `KURT` e `PPV_MM_S` sobem na amostra seguinte e voltam.
- Negativo: `ERR Invalid telemetry period. Use 100..10000 ms.`

**Se sair isso; faça isso**

- Se `RMS` não reagir a batidas; o MPU6050 está respondendo no I²C, mas mal fixado ou com defeito. Confira `MPU=YES` no `STATUS`.
- Se `CLIP=YES` com o sensor parado; leitura saturada: módulo defeituoso ou alimentação ruidosa.
- Se sair `ERR I2C_POLLING_READ_FAILED CHECK_SDA_SCL_PULLUPS_AND_MPU_POWER`; a leitura I²C falhou no meio da aquisição. Fio solto ou falta de *pull-up* (a maioria dos módulos GY-521 já traz).
- Se `FFT_VALID=NO` sempre; a janela nunca enche. Veja `RATE_HZ` e `WINDOW_SIZE` no `STATUS`.

**Condição de aceite:** telemetria nos quatro ritmos, valores estáveis em repouso e reagindo a estímulo. Anote o `RMS` de repouso: é o piso de ruído deste sensor.

**Registro:** data e hora · placa e ID: sensor de vibração (UUID) · versão · resultado · medido: `RMS` de repouso · inesperado.

### [ ] RT 40 - Espectro local com uma fonte conhecida

Prioridade: recomendado.

**Comandos necessários**

Apoie o sensor sobre um alto-falante tocando um tom de 100 Hz (aplicativo gerador de tons), ou sobre um celular vibrando.

```text
FFT ONCE
TELEMETRY ONCE
```

**Variações possíveis**

- Repita com 50 Hz e 200 Hz. A 1000 Hz de amostragem, só se enxerga até 500 Hz.
- Resolução: taxa ÷ janela. Com 1000 Hz e 512 amostras, cerca de 2 Hz por faixa.

**Saídas esperadas**

- `OK FFT_ONCE` e, no próximo bloco válido, `FFT VALID=YES BINS=<n> VALUES=<v1>,<v2>,...`.
- `PEAK_HZ` da telemetria a até uma faixa de distância da frequência tocada.

**Se sair isso; faça isso**

- Se sair `FFT VALID=NO BINS=0`; o bloco ainda não tinha FFT válida. Repita.
- Se `PEAK_HZ` cair num submúltiplo ou "espelhado"; a frequência real está acima da metade da taxa (*aliasing*). Abaixe o tom ou suba a taxa.
- Se o pico for sempre perto de 0 Hz; a componente contínua (gravidade) domina. Aumente o volume do estímulo e fixe melhor o sensor.

**Condição de aceite:** o pico do espectro coincide com a frequência aplicada, dentro de uma faixa, em pelo menos duas frequências diferentes.

**Registro:** data e hora · placa e ID: sensor de vibração (UUID) · versão · resultado · medido: frequência aplicada e frequência do pico, em cada estímulo · inesperado.

### [ ] RT 41 - Configuração: SET, GET e APPLY

Prioridade: vital.

**Comandos necessários**

```text
SET MODE STRUCTURAL
SET RATE 500
SET WINDOW HAMMING
SET WINDOW_SIZE 256
SET STALTA 3.5
SET GAIN 1.25
GET
APPLY
STATUS
```

**Variações possíveis**

- Janelas: `RECT`, `HANN`, `HAMMING`, `FLATTOP`, `BLACKMAN` (ou 0 a 4). `SIZE` é sinônimo de `WINDOW_SIZE`.
- Testes negativos: `SET RATE 2000`, `SET WINDOW_SIZE 300`, `SET STALTA 0.5`, `SET GAIN 0`, `SET FOO 1`.

**Saídas esperadas**

- Cada `SET`: `OK STAGED <campo>=<valor>`. Nada muda até o `APPLY`.
- `APPLY`: `OK APPLY_QUEUED` e depois `CONFIG_APPLIED MODE=STRUCTURAL RATE_REQ=500.00 RATE_EFF=<n> WINDOW_REQ=256 WINDOW_EFF=256 ACQ=POLLING`.
- `STATUS` reflete os valores novos.
- Negativos: `ERR Invalid rate. Use 4..1000 Hz.`, `ERR Invalid window size. Use 128, 256 or 512.`, `ERR Invalid STA/LTA threshold. Use value > 1.0.`, `ERR Invalid gain. Use value > 0.0.`, `ERR Unknown SET field.`

**Se sair isso; faça isso**

- Se `RATE_EFF` diferir de `RATE_REQ`; o MPU6050 divide o relógio por inteiros e nem toda taxa existe. Use a taxa efetiva nos cálculos; é ela que vale.
- Se sair `ERR CONFIG_INVALID FIELD=<campo>` no `APPLY`; a combinação é inválida. O sensor registra o alarme `0x4006`.

**Condição de aceite:** os valores preparados só entram em vigor com `APPLY`, a configuração aplicada aparece no `STATUS` e os cinco testes negativos são recusados. Ao final, volte à configuração anotada no RT 37.

**Registro:** data e hora · placa e ID: sensor de vibração (UUID) · versão · resultado · medido: não há · inesperado.

### [ ] RT 42 - Modos de operação do sensor de vibração

Prioridade: recomendado.

**Comandos necessários**

Para cada modo, com a telemetria ligada:

```text
SET MODE ROTATING
APPLY
SET MODE SEISMIC
APPLY
SET MODE IDLE
APPLY
```

**Variações possíveis**

- Abreviações `ROT` e `STRUCT`; números 0 a 3 (0 IDLE, 1 ROTATING, 2 STRUCTURAL, 3 SEISMIC).

**Saídas esperadas**

- `ROTATING`: atenção a `PEAK_HZ`, `PEAK_AMP` e `ENT` (entropia espectral baixa quando há um tom dominante).
- `STRUCTURAL`: atenção a `RMS` e `PPV_MM_S`.
- `SEISMIC`: `STA_LTA=NO` em repouso e `STA_LTA=YES` logo depois de uma batida na mesa.
- `IDLE`: `MODE=IDLE`, sem aquisição.

**Se sair isso; faça isso**

- Se `STA_LTA` nunca virar `YES`; o limiar está alto. Abaixe com `SET STALTA 2` e `APPLY`.
- Se virar `YES` o tempo todo; o limiar está baixo para o ruído da bancada.
- Se `ACQ POLLING` responder `ERR ACQ restart rejected. Check MPU or IDLE mode.`; o sensor está em `IDLE` ou sem MPU.

**Condição de aceite:** os quatro modos entram em vigor e o detector do modo `SEISMIC` dispara com uma batida e volta ao repouso.

**Registro:** data e hora · placa e ID: sensor de vibração (UUID) · versão · resultado · medido: não há · inesperado.

### [ ] RT 43 - Alarmes (DTC) do sensor de vibração

Prioridade: recomendado.

**Comandos necessários**

1. Com a telemetria ligada, solte o fio SDA do MPU6050.
2. Observe e então religue o fio.

```text
DTC
ACQ POLLING
DTC CLEAR
DTC
```

**Variações possíveis**

- Saturação: sacuda o sensor com força (`0x2002`).
- Montagem solta: deixe o sensor rolar ou incline-o bruscamente (`0x2001`).

**Saídas esperadas**

- Ao soltar o fio: `DTC_EVENT CODE=0x1001 SYMPTOM=0x.. SEVERITY=.. TS_MS=...` e, na telemetria, `DTC=0x1001`.
- `DTC`: `DTC ACTIVE=0x1001 COUNT=<n>`.
- `ACQ POLLING` com o fio de volta: `OK ACQ=POLLING_RESTARTED`.
- `DTC CLEAR`: `OK DTC_CLEAR`, e `DTC ACTIVE=0x0000 COUNT=0`.

**Se sair isso; faça isso**

- Se aparecer `0x1002` em vez de `0x1001`; o barramento I²C travou (linha presa em nível baixo). Desligue e religue o sensor.
- Se o alarme voltar logo depois do `DTC CLEAR`; a causa continua presente.
- Se `ACQ DRDY` responder `ERR DRDY disabled. Use ACQ POLLING.`; é o previsto: a aquisição por interrupção está desativada nesta versão.

**Condição de aceite:** a falha provocada gera o código certo, a aquisição se recupera com `ACQ POLLING` e o alarme some com `DTC CLEAR`.

**Registro:** data e hora · placa e ID: sensor de vibração (UUID) · versão · resultado · medido: código do alarme gerado · inesperado.

### [ ] RT 44 - RESET em duas etapas

Prioridade: opcional.

**Comandos necessários**

```text
RESET
RESET CONFIRM
```

**Variações possíveis**

- Teste negativo: `RESET`, espere mais de 10 s, `RESET CONFIRM`.

**Saídas esperadas**

- `OK RESET_PENDING CONFIRM_WITH='RESET CONFIRM' WINDOW_MS=10000`, depois `OK RESET_CONFIRMED`, e o sensor reinicia (o banner volta).
- Negativo: `ERR RESET_CONFIRMATION_EXPIRED`.

**Se sair isso; faça isso**

- Se o console não voltar depois do reinício; a porta USB foi recriada. Feche e reabra o monitor.
- Se a configuração do RT 41 não sobreviver ao reinício; anote: saber se a configuração é volátil muda como você opera o sensor em campo.

**Condição de aceite:** o reinício só acontece com a confirmação dentro de 10 s. Anote se a configuração persiste.

**Registro:** data e hora · placa e ID: sensor de vibração (UUID) · versão · resultado · medido: se a configuração persiste depois do reinício (sim ou não) · inesperado.

### Sensor acústico (Pico W ou Pico 2 W + INMP441)

O console tem os mesmos comandos de base (`HELP`, `PING`, `VERSION`, `STATUS`, `GET`, `AUTH`, `TELEMETRY`, `FFT ONCE`, `DTC`, `NET`, `RESET`). Os RT 37, 38 e 44 valem também para ele; abaixo está só o que é próprio do microfone.

### [ ] RT 45 - Microfone vivo: níveis em silêncio, fala e palmas

Prioridade: vital.

**Comandos necessários**

```text
AUTH UNLOCK <token>
SET MODE LEVEL
APPLY
TELEMETRY ON
```

**Variações possíveis**

- Compare com um aplicativo de medidor de nível sonoro no celular, lado a lado. Não é referência metrológica, mas pega erros grosseiros.

**Saídas esperadas**

- `TEL PROFILE=ACOUSTIC MODE=LEVEL ACQ=I2S WIN=1024 RATE_EFF_HZ=16000.00 INTERVAL_MS=1000 COVERAGE_PCT=100 CPU_PCT=<n> CHANNEL=... SPECTRUM_VALID=YES OVERLOAD=NO MIC_FAULT=NO LAEQ_DB=... LCEQ_DB=... LZEQ_DB=... LZPEAK_DB=... LAF_DB=... OCT_DB=...`.
- Sala silenciosa: `LAEQ_DB` entre 33 e 45. O ruído próprio do INMP441 fica perto de 33 dB(A); abaixo disso ele não mede.
- Fala normal a 1 m: `LAEQ_DB` em torno de 55 a 65.
- Palma a 30 cm: `LZPEAK_DB` acima de 100 e `CREST` alto naquele segundo.

**Se sair isso; faça isso**

- Se `LAEQ_DB` ficar parado num valor muito baixo e não reagir a nada; o microfone entrega zeros. Veja `MIC_FAULT` e o RT 46.
- Se `OVERLOAD=YES` em ambiente normal; ganho de calibração errado (`GAIN` no `STATUS` deveria ser 1.0 de fábrica) ou microfone defeituoso.
- Se os níveis diferirem do aplicativo em mais de 5 dB; anote, mas não ajuste o ganho sem um calibrador (RT 85).
- Se as duas últimas bandas de `OCT_DB` saírem como `-`; é o previsto a 16 kHz: as bandas de 8 e 16 kHz passam da metade da taxa.

**Condição de aceite:** os níveis sobem e descem com o som do ambiente, dentro das faixas plausíveis acima, com `COVERAGE_PCT=100` e `MIC_FAULT=NO`. Anote o `LAEQ_DB` de silêncio.

**Registro:** data e hora · placa e ID: sensor acústico (UUID) · versão · resultado · medido: `LAEQ_DB` de silêncio · inesperado.

### [ ] RT 46 - Canal L/R e falha de microfone

Prioridade: recomendado. Está na lista de pendências de bancada do repositório.

**Comandos necessários**

1. Com o sensor ligado, mova o pino L/R do GND para o VDD (3V3).
2. Volte o L/R ao GND.
3. Solte o fio SD.
4. Religue o SD.

```text
STATUS
DTC
```

**Variações possíveis**

- SD ligado direto ao 3V3 simula "dado preso em nível alto" (sintoma `0x12`).

**Saídas esperadas**

- L/R em VDD: depois de 1 s de silêncio digital, `[MIC] sem sinal no canal anterior; tentando o canal direito (pino L/R em ...)` e então `[MIC] sinal presente no canal direito`. `CHANNEL` muda no `STATUS`.
- SD solto: depois de uma tentativa em cada canal, `[MIC] ...: confira alimentação, SCK=GP18, WS=GP19, SD=GP20 e o pino L/R`, `MIC_FAULT=YES` na telemetria e o alarme `0x1003`.
- SD religado: `[MIC] sinal presente no canal ...` e `MIC=OK`.

**Se sair isso; faça isso**

- Se o firmware não trocar de canal sozinho; anote o tempo esperado e o `STATUS`. A troca automática só foi verificada em simulação.
- Se o alarme `0x1003` não sair com o SD solto; uma entrada flutuante pode produzir lixo que parece sinal. Repita com o SD em GND.

**Condição de aceite:** o sensor acha o microfone nos dois canais sem intervenção e acusa `0x1003` quando não há dado válido.

**Registro:** data e hora · placa e ID: sensor acústico (UUID) · versão · resultado · medido: não há · inesperado.

### [ ] RT 47 - Modo LEVEL: integração, estatísticas e doses

Prioridade: recomendado.

**Comandos necessários**

```text
SET MODE LEVEL
APPLY
MEASURE RESET
TELEMETRY ON
```

Deixe 2 min em ambiente normal; depois toque música alta perto do microfone por 1 min.

**Variações possíveis**

- A dose só acumula com nível lento de 80 dB(A) ou mais. Para vê-la andar, o som precisa ser realmente alto.

**Saídas esperadas**

- Chaves do modo: `LEQ_T_DB`, `L10_DB`, `L50_DB`, `L90_DB`, `DOSE_NR15_PCT`, `DOSE_NHO01_PCT`, `ELAPSED_S`, `DOSE_LIMIT`.
- `MEASURE RESET`: `ELAPSED_S` volta a zero e as doses também.
- Sempre `L10_DB` ≥ `L50_DB` ≥ `L90_DB`.
- Com som acima de 85 dB(A), `DOSE_NHO01_PCT` cresce mais rápido que `DOSE_NR15_PCT` (incremento de duplicação de 3 dB contra 5 dB).

**Se sair isso; faça isso**

- Se as doses não saírem de zero; o nível não chegou a 80 dB(A). Não é defeito.
- Se `DOSE_LIMIT=YES` aparecer cedo; ou uma dose chegou a 100 %, ou o nível lento passou de 115 dB(A).
- Se `ELAPSED_S` não avançar um por segundo; há blocos de áudio perdidos (alarme `0x3001`). Veja o RT 50.

**Condição de aceite:** as estatísticas respeitam a ordem L10 ≥ L50 ≥ L90, o reinício zera a medição e `ELAPSED_S` acompanha o relógio.

**Registro:** data e hora · placa e ID: sensor acústico (UUID) · versão · resultado · medido: L10, L50 e L90 de uma medição · inesperado.

### [ ] RT 48 - Modo MACHINE: tom, tacômetro e assinatura

Prioridade: recomendado.

**Comandos necessários**

Use um ventilador de mesa, ou um aplicativo gerador tocando uma onda **dente de serra** de 200 Hz.

```text
SET MODE MACHINE
APPLY
BASELINE LEARN 10
TELEMETRY ON
```

Depois dos 10 s, mude o som (outra velocidade do ventilador, ou 300 Hz) e, por fim:

```text
BASELINE CLEAR
```

**Variações possíveis**

- Máquinas lentas (motor de 1800 rpm, 30 Hz): `SET RATE 8000`, `SET FFT 2048`, `APPLY`. A busca passa a ir de 20 a 798 Hz; a 16 kHz com 1024 pontos ela vai de 62,5 a 1594 Hz.
- Aprendizado de 5 a 3600 s.

**Saídas esperadas**

- `SET MODE MACHINE`: `OK STAGED MODE=MACHINE THRESHOLD=3.000` (o console propõe o limiar do modo).
- Chaves do modo: `TONAL_DB`, `F0_HZ`, `HARMONICS`, `ANOMALY_Z`, `BASELINE=NONE|LEARNING|READY`, `BASELINE_PCT`, `ANOMALY`.
- Durante o aprendizado, `BASELINE=LEARNING` com `BASELINE_PCT` subindo; depois, `BASELINE=READY`.
- Com o som igual ao aprendido, `ANOMALY_Z` baixo e `ANOMALY=NO`; mudando o som, `ANOMALY_Z` sobe acima de 3 e `ANOMALY=YES`.

**Se sair isso; faça isso**

- Se `F0_HZ` não aparecer com um tom **senoidal** puro; é o previsto. O tacômetro só aceita uma fundamental com pelo menos três harmônicos 6 dB acima dos vales. Use dente de serra, onda quadrada ou uma máquina real. `PEAK_HZ` continua mostrando o tom.
- Se `F0_HZ` der o dobro ou a metade do esperado; erro de oitava típico de detecção por harmônicos. Anote a configuração e a frequência.
- Se a assinatura sumir depois de mudar taxa ou FFT; é o previsto: ela só sobrevive a reconfigurações que mantêm os dois.

**Condição de aceite:** fundamental correta com som harmônico, assinatura aprendida, e anomalia acusada quando o som muda e apagada quando volta.

**Registro:** data e hora · placa e ID: sensor acústico (UUID) · versão · resultado · medido: frequência fundamental aplicada e a detectada · inesperado.

### [ ] RT 49 - Modo EVENTS: detecção e classificação

Prioridade: recomendado.

**Comandos necessários**

```text
SET MODE EVENTS
APPLY
TELEMETRY ON
```

Espere 10 s em silêncio e produza, um por vez, com 5 s de intervalo: uma batida na mesa, um estalo de dedos ou chaves, um apito de 1 s e um ruído longo (aspirador ligando).

**Variações possíveis**

- Limiar: `SET THRESHOLD 4` (mais sensível) ou `16` (menos). O padrão do modo é 8, isto é, +9 dB sobre o fundo.
- Coletar exemplos para o futuro classificador: na estação, `:rotulo bate-palma` (sem espaços; até 40 caracteres) e produza o som; cada evento detectado vira uma linha de `Front/logs/exemplos_sonoros.csv` (data, sensor, rótulo, classe do firmware, pico, duração, centroide, níveis e bandas de oitava). `:rotulo` mostra o estado e `:rotulo off` encerra. Sem rótulo ativo nada é gravado, e o classificador do firmware não muda: é só coleta.

**Saídas esperadas**

- Chaves do modo: `EVENTS`, `EVENTS_INTERVAL`, `EVENT_ACTIVE`, `STA_LTA_DB`, `BACKGROUND_DB` e, depois do primeiro evento, `EVENT_CLASS`, `EVENT_PEAK_DB`, `EVENT_SEL_DB`, `EVENT_DURATION_MS`, `EVENT_CENTROID_HZ`, `EVENT_AGE_S`.
- Batida: `IMPACT` (menos de 400 ms, centroide abaixo de 2,5 kHz).
- Estalo: `CRACK` (menos de 400 ms, centroide de 2,5 kHz ou mais).
- Apito: `TONAL` (400 ms ou mais, planura abaixo de 0,1).
- Ruído longo: `TRANSIENT`.

**Se sair isso; faça isso**

- Se nada for detectado nos primeiros 2 s; é o aquecimento do detector.
- Se um ruído contínuo virar um evento de 30 s e depois parar de disparar; é o previsto: após 30 s o ruído novo passa a ser o fundo.
- Se a classe sair errada; anote duração e centroide. A classificação é heurística e os limites (400 ms, 2,5 kHz) são fixos.

**Condição de aceite:** os quatro sons são detectados e pelo menos três recebem a classe esperada. Anote a tabela som × classe × duração × centroide. Se coletou exemplos, anote o rótulo e quantas linhas o arquivo ganhou (cada som deve render uma linha por evento detectado).

**Registro:** data e hora · placa e ID: sensor acústico (UUID) · versão · resultado · medido: tabela som × classe × duração × centroide · inesperado.

### [ ] RT 50 - Carga de processamento por taxa e tamanho de FFT

Prioridade: recomendado. É a medida que decide se 32 e 48 kHz são utilizáveis no Pico W.

**Comandos necessários**

Para cada combinação, espere 10 s e leia `CPU_PCT` e `COVERAGE_PCT` da telemetria:

```text
SET RATE 16000
SET FFT 1024
APPLY
```

**Variações possíveis**

- Taxas: 8000, 16000, 24000, 32000, 48000. FFT: 256, 512, 1024, 2048. São 20 combinações por placa.
- Repita nos três modos: `EVENTS` usa a FFT só durante eventos e carrega menos.

**Saídas esperadas**

- `CONFIG_APPLIED` com a taxa efetiva. No RP2040, 24, 32 e 48 kHz saem como 23 999,23, 32 002,05 e 48 003,07 Hz; no RP2350 são exatas.
- Pico W: folga em 16 kHz com 1024 pontos. Pico 2 W: folga em 48 kHz com 2048 pontos.

**Se sair isso; faça isso**

- Se `COVERAGE_PCT` cair abaixo de 100; parte dos quadros não passou pela FFT. Os níveis continuam exatos (vêm do domínio do tempo), mas o espectro e as ponderações A e C ficam menos representativos.
- Se aparecer o alarme `0x3001`; o DSP perdeu blocos de áudio. Essa combinação não serve para essa placa.
- Se aparecer `0x3002` ou a placa reiniciar; o núcleo de DSP ficou 2 s sem progresso e o *watchdog* (8 s) agiu. Registre a combinação como proibida.

**Condição de aceite:** tabela taxa × FFT preenchida para cada placa, com `CPU_PCT`, `COVERAGE_PCT` e os alarmes. As combinações com cobertura de 100 % e sem `0x3001` são as aprovadas.

**Registro:** data e hora · placa e ID: cada placa do sensor acústico (UUID) · versão · resultado · medido: tabela taxa × FFT, com `CPU_PCT`, `COVERAGE_PCT` e os alarmes · inesperado.

### Painel e leitor

### [ ] RT 51 - Painel sozinho: tela, toque, LED e som

Prioridade: recomendado.

**Comandos necessários**

Ligue o painel longe dos módulos, com um alto-falante pequeno no conector `SPEAK` e o monitor serial aberto.

**Variações possíveis**

- Recalibrar o toque: ligar segurando BOOT.

**Saídas esperadas**

- Na serial: `#IOC HELLO uuid=0x... profile=PANEL fw=... keys=... board=CYD`.
- Os quatro botões da base (Situação, Rede, Marcar, Ciente) respondem ao toque no lugar certo.
- Sem estação na rede, o cabeçalho não mostra hora.

**Se sair isso; faça isso**

- Se `keys=` indicar ausência de chave; o painel foi compilado sem `.env.local` e só funcionará pela USB com o Simulation Studio.
- Se o toque acertar um botão vizinho; recalibre.
- Se a tela piscar ao tocar; alimentação fraca na porta USB.

**Condição de aceite:** imagem correta, toque preciso nos quatro botões e chave presente. LED e som são conferidos com a rede, no RT 66.

**Registro:** data e hora · placa e ID: painel (UUID) · versão · resultado · medido: não há · inesperado.

### [ ] RT 52 - Leitor: navegação, sono e retomada

Prioridade: opcional.

**Comandos necessários**

Nos botões, ou pela serial (`a`, `b`, `c` tocam; `A`, `B`, `C` seguram; `s` mostra a posição).

**Variações possíveis**

- Teclado M5Stack CardKB na Port A: setas viram página e pulam seção, Enter abre, Esc volta, 1 a 0 abrem documentos.

**Saídas esperadas**

- B abre o documento; C avança a página; segurar B volta à biblioteca.
- Três toques seguidos em C avançam três páginas com um desenho só.
- `#LEITOR desenho <n> pronto em <ms> ms` a cada redesenho (15 a 30 s).
- Na bateria: `#LEITOR dormindo; um botão acorda` após 20 s sem uso; desligamento após 30 min dormindo; ao religar, volta à mesma página sem redesenhar.

**Se sair isso; faça isso**

- Se ele recomeçar pela biblioteca depois de regravar; é o previsto quando o texto mudou.
- Se `teclado=ausente` com o CardKB ligado; confira a Port A e o cabo Grove.

**Condição de aceite:** navegação completa, tempo de desenho anotado e retomada na página certa depois de desligar.

**Registro:** data e hora · placa e ID: leitor · versão · resultado · medido: tempo de desenho · inesperado.

## Fase 6. Integração: sensores na rede

Rede formada, estação conectada à Probe, monitor do módulo responsável e console do sensor abertos. Os comandos de texto abaixo vão pelo lançador da estação (`k`); as linhas brutas da Probe aparecem em Mensagens (`F6`, com `d` para depuração) e no arquivo da sessão:

```bash
tail -f Front/logs/gravacao_*.log
```

Nos comandos `CMD`, `TX=` é um identificador qualquer que você escolhe (`A1`, `A2`...) e que volta na confirmação `ACK`.

### [ ] RT 53 - Descoberta do sensor por BLE

Prioridade: vital.

**Comandos necessários**

Ligue um sensor a 1 ou 2 m dos módulos. Na estação: Sensores sem fio (`F7` ou `w`).

**Variações possíveis**

- Detalhes lista todas as observações por sinal; a primeira é uma recomendação, não uma decisão.

**Saídas esperadas**

- Uma linha por módulo que ouve o sensor: `[GW] WIRELESS_CANDIDATE reporter=<n> uuid=0x<UUID> profile=VIBRATION rssi=<dBm> protocol=6`.
- Em Disponíveis, o UUID aparece **uma vez**, com o perfil, o módulo de melhor sinal e quantos módulos o ouvem.
- O UUID é o mesmo anotado no RT 16 ou 17.

**Se sair isso; faça isso**

- Se o sensor não aparecer; no console dele, `STATUS` deve ter `BLE_BEACON=ON`. Se tiver, o problema está na escuta dos módulos: aproxime o sensor e veja se algum `reporter` surge.
- Se só um módulo o ouvir, estando todos próximos; a antena ou o rádio dos outros está prejudicado (placa encostada em metal, alimentação fraca).
- Se `protocol` não for 6; o sensor tem firmware de outra versão. Ele será descoberto e associado, mas nunca abrirá sessão.
- Se o sensor sumir de Disponíveis; só valem observações dos últimos 15 s.

**Condição de aceite:** o sensor aparece em até 15 s, com o UUID e o perfil corretos, ouvido por todos os módulos próximos. Anote o RSSI visto por cada módulo.

**Registro:** data e hora · placa e ID: sensor (UUID) e cada módulo que o ouviu · versão · resultado · medido: tempo até o sensor aparecer (s) e RSSI visto por cada módulo · inesperado.

### [ ] RT 54 - Associação do sensor a um módulo

Prioridade: vital.

**Comandos necessários**

Em Sensores sem fio, `Enter` no sensor, escolha o módulo e a política. Ou, pelo lançador:

```text
WIRELESS BIND 1 0x<UUID> AUTO
```

**Variações possíveis**

- Sem `AUTO`, a política é manual (só o operador reassocia).
- Mudar depois: `WIRELESS POLICY 1 0x<UUID> AUTO|MANUAL`, ou `f` na lista Associados.
- Teste negativo: associar a um módulo que **não** ouve o sensor (desligue o sensor, espere 30 s e mande o `BIND`).

**Saídas esperadas**

- `[GW] WIRELESS_ASSOC node=1 child=1 uuid=0x<UUID> profile=VIBRATION state=<BOUND|ONLINE> rssi=<n> protocol=6 failover=AUTO session=...`.
- O sensor passa a Associados com o identificador `01.01` (módulo.filho) e aparece na tela Início sob o módulo. A estação só o mostra depois da confirmação do módulo.
- Negativo: `state=REJECTED` (o sensor não foi ouvido recentemente, ou não há filho livre).

**Se sair isso; faça isso**

- Se não vier `WIRELESS_ASSOC`; o pedido (`0x300`/`0x301`) não chegou ao módulo. Confira o ID e o barramento.
- Se o estado ficar em `ASSOCIATING`; o módulo aceitou mas não ouve o sensor. Em 15 s ele publica `STALE`.
- Se a estação bloquear o comando; ela está no modo `presence` sem a chave física (RT 73), ou há operadores cadastrados e ninguém se identificou (RT 75).

**Condição de aceite:** vínculo confirmado pelo módulo, com `child`, política e perfil corretos; o pedido a um módulo que não ouve o sensor é rejeitado.

**Registro:** data e hora · placa e ID: sensor (UUID) e módulo responsável · versão · resultado · medido: `child`, política e perfil confirmados · inesperado.

### [ ] RT 55 - Criação do canal de dados, do vínculo à sessão

Prioridade: vital. É o ponto mais delicado do sistema e a principal pendência de bancada física.

**Comandos necessários**

Logo depois do RT 54, acompanhe ao mesmo tempo: a tela do sensor na estação (seção Criação do canal de dados), o monitor do módulo e o console do sensor. No sensor, ao final:

```text
NET
```

**Variações possíveis**

- Repita 5 vezes (desassociar e associar) para ter média e pior caso.
- Repita com o sensor associado ao **líder**. Na bancada de 08/10 a sessão com o módulo líder não abriu.

**Saídas esperadas**

- Módulo: `[WLINK] AP=ON ssid=IOC-01 channel=<1|6|11> ip=... udp=4242`, `[WLINK] OFERTA uuid=0x... canal=<n>: ...`, `[WLINK] estacao ... mac=...`, `[WLINK] estacao recebeu ip=192.168.4.x`, `[WLINK] child=1 uuid=0x... SECURE`.
- Sensor: `[NET] oferta do Node 1 conferida: entrando no Wi-Fi IOC-01`, `[NET] Wi-Fi IOC-01: conectado, ip=192.168.4.2 gateway=192.168.4.1`, `[NET] HELLO 1 enviado ao Node em 192.168.4.1:4242 (...)`, `[NET] sessao autenticada com o Node 1, filho 1`.
- Probe: `[GW] WIRELESS_EVENT node=1 child=1 event=...` para cada etapa (`AP_UP`, `OFFER`, `STATION_JOIN`, `STATION_IP`, `HELLO_OK`, `SESSION_UP`) e `[GW] WIRELESS_LINK node=1 child=1 state=SECURE ...`.
- `NET` no sensor: `NET STATE=BOUND WIFI=ON ... PARENT_NODE=1 CHILD=1 AUTH_REJECT=0 REPLAY_REJECT=0`.

**Se sair isso; faça isso**

- Se sair `event=NO_KEY`, ou canal em "módulo sem chave"; o módulo tem `LINK_KEY=MISSING` (RT 15).
- Se sair `AP_ERROR`; o ponto de acesso não subiu. O módulo tenta de novo a cada 5 s; se persistir, suspeite de alimentação.
- Se parar em `OFFER`, sem `STATION_JOIN`; o sensor não aceitou a oferta. No console dele não haverá `[NET] oferta ... conferida`: chaves diferentes entre sensor e módulo, ou sensor longe.
- Se houver `STATION_JOIN` sem `STATION_IP`; o sensor entrou no Wi-Fi mas não recebeu endereço. Veja a linha `[NET] Wi-Fi IOC-01: <estado> (<código>)` no sensor.
- Se sair `HELLO_REJECT reason=KEY`; a chave do dispositivo não confere: grave os dois com as mesmas credenciais. `reason=NOT_BOUND`: o sensor entrou no módulo errado. `reason=PROTOCOL`: versões diferentes.
- Se sair `HANDSHAKE_TIMEOUT`, ou `[WLINK] child=1 CHALLENGE sem resposta em <ms> ms`; a resposta se perdeu na disputa de rádio entre BLE e Wi-Fi. Se for frequente, o repositório indica reduzir `SCAN_WINDOW_QUIET` em `wireless_discovery.cpp`.
- Se o sensor passar 60 s sem autenticar; ele desliga o Wi-Fi e espera uma oferta nova. É o previsto; conte isso no tempo total.

**Condição de aceite:** sessão `SECURE` em todas as tentativas, com `auth_fail=0`. A bancada virtual abre em ≈ 5 s; anote o tempo de cada etapa (a tela mostra os segundos desde o vínculo) e defina com esses números o seu limite de aceite.

**Registro:** data e hora · placa e ID: sensor (UUID) e módulo responsável · versão · resultado · medido: tempo de cada etapa, do vínculo à sessão (s), e o limite de aceite que você definiu · inesperado.

### [ ] RT 56 - Telemetria e alarmes de ponta a ponta

Prioridade: vital.

**Comandos necessários**

Com a sessão aberta, observe 2 min. Depois provoque um alarme (solte o SDA do MPU6050) e limpe-o:

```text
CMD TARGET=01.01 ACTION=DTC_CLEAR TX=A1
```

**Variações possíveis**

- Pela tela do sensor: `c` abre os comandos, `s` pede o estado.

**Saídas esperadas**

- Uma linha por segundo: `TEL NODE=1 CHILD=1 SEQ=<n> GW_TS_MS=... MODE=... ACQ=POLLING WIN=512 ... RMS=... RMS_UNIT=m/s2 ...`, com `SEQ` crescendo de um em um.
- A cada 5 s: `[GW] WIRELESS_LINK node=1 child=1 state=SECURE rssi=<n> age_s=<n> rx=<n> auth_fail=0 replay=0 lost=0 stream=ON period_ms=1000 rtt_ms=<n> rtt_max_ms=<n>`.
- Os valores na tela do sensor batem com os do console USB (RT 39).
- No alarme: `DTC NODE=1 CHILD=1 CODE=0x1001 SYMPTOM=0x.. SEVERITY=.. TS_MS=...`, e o sensor entra em Precisa de atenção. Depois do comando: `ACK COMMAND=DTC_CLEAR STATE=APPLIED TX=A1 ...`.

**Se sair isso; faça isso**

- Se `SEQ` saltar; houve perda no barramento ou na remontagem. Olhe `xfer_err` em `BUS_STATS`.
- Se `lost` crescer; a perda foi no Wi-Fi. Aproxime o sensor e compare o `rssi`.
- Se `rtt_ms=-1`; ainda não há medida de ida e volta. Deve aparecer em até 3 s.
- Se `auth_fail` ou `replay` forem maiores que zero; alguém está mandando datagramas inválidos, ou há dois sensores com a mesma identidade. Anote: isso vira ocorrência de segurança na estação.

**Condição de aceite:** 120 amostras seguidas sem salto de `SEQ`, `lost=0`, `auth_fail=0`, `replay=0`; o alarme chega à estação e é limpo pelo comando. Anote `rtt_ms` e `rtt_max_ms`.

**Registro:** data e hora · placa e ID: sensor (UUID) e módulo responsável · versão · resultado · medido: `rtt_ms` e `rtt_max_ms` · inesperado.

### [ ] RT 57 - Comandos de telemetria pela rede

Prioridade: vital.

**Comandos necessários**

```text
CMD TARGET=01.01 ACTION=TELEMETRY TX=B1 STATE=OFF
CMD TARGET=01.01 ACTION=TELEMETRY TX=B2 STATE=ONCE
CMD TARGET=01.01 ACTION=TELEMETRY TX=B3 STATE=ON
CMD TARGET=01.01 ACTION=TELEMETRY_PERIOD TX=B4 PERIOD_MS=500
CMD TARGET=01.01 ACTION=TELEMETRY_PERIOD TX=B5 PERIOD_MS=1000
CMD TARGET=01.01 ACTION=STATUS TX=B6
```

**Variações possíveis**

- `STATE=FAST` e `STATE=SLOW`. Período de 200 a 10000 ms.
- Teste negativo: `CMD TARGET=01.03 ACTION=STATUS TX=B7` (filho que não existe).

**Saídas esperadas**

- Uma confirmação por comando: `ACK COMMAND=<ação> STATE=APPLIED TX=<id> NODE=1 CHILD=1 DETAIL=<n> RTT_MS=<n>`.
- `OFF` para as linhas `TEL`; `ONCE` traz uma; `ON` retoma; o período muda o ritmo e o campo `period_ms` de `WIRELESS_LINK`.
- `STATUS` faz o módulo republicar `WIRELESS_LINK` e `WIRELESS_CONFIG`.
- Negativo: `STATE=NO_LINK` ou `UNKNOWN`.

**Se sair isso; faça isso**

- Se não vier `ACK`; o comando (`0x304`) não chegou ao módulo, ou a confirmação se perdeu. A estação mostra o comando como sem resposta.
- Se `RTT_MS` passar de centenas de milissegundos; o módulo está com a fila de transmissão cheia. Veja a ocupação.

**Condição de aceite:** todos os comandos confirmados com `APPLIED` e efeito visível; o comando ao filho inexistente é recusado. Anote o `RTT_MS` típico.

**Registro:** data e hora · placa e ID: sensor (UUID) e módulo responsável · versão · resultado · medido: `RTT_MS` típico · inesperado.

### [ ] RT 58 - Espectro com 64 e com 256 faixas

Prioridade: vital. O espectro de 256 faixas está na lista de pendências de bancada.

**Comandos necessários**

```text
CMD TARGET=01.01 ACTION=FFT TX=C1 BINS=64
CMD TARGET=01.01 ACTION=FFT TX=C2 BINS=256
```

**Variações possíveis**

- Pela tela do sensor: `f` abre o espectro, `n` pede um novo.
- Repita 10 vezes o de 256 faixas, para pegar perdas ocasionais.

**Saídas esperadas**

- 64 faixas: uma linha `FRAG NODE=1 CHILD=1 TYPE=FFT TRANSFER=<n> INDEX=0 COUNT=1 FORMAT=U16_SCALED SCALE=... RATE_HZ=... FFT_SIZE=... WINDOW=... DATA=<hex>`.
- 256 faixas: quatro blocos, `INDEX=0` a `INDEX=3`, com `COUNT=4`.
- A tela Espectro mostra o pico na mesma frequência do RT 40, com o mesmo estímulo.

**Se sair isso; faça isso**

- Se faltarem blocos no de 256; o repositório indica aumentar a pausa entre blocos em `wireless_link.cpp`. Anote quantos de 10 pedidos vieram completos e o `xfer_err`.
- Se o pedido voltar `REJECTED REASON=SENSOR_TIMEOUT`; o sensor não entregou o espectro em 3 s.
- Se a telemetria falhar durante o espectro; o limite de 4 quadros pendentes no controlador não segurou a rajada. Registre com o `BUS_STATS` do instante.

**Condição de aceite:** 10 de 10 espectros de 64 faixas e 10 de 10 de 256 faixas completos, sem salto de `SEQ` na telemetria durante as transferências.

**Registro:** data e hora · placa e ID: sensor (UUID) e módulo responsável · versão · resultado · medido: espectros completos de 64 faixas (de 10) e de 256 faixas (de 10) · inesperado.

### [ ] RT 59 - Configuração remota e recusas

Prioridade: vital.

**Comandos necessários**

```text
CMD TARGET=01.01 ACTION=CONFIG TX=D1 MODE=STRUCTURAL RATE_HZ=500 WINDOW=HAMMING WINDOW_SIZE=256 STALTA=3.5 GAIN=1.25
```

**Variações possíveis**

- Pela tela do sensor: `c`, configuração guiada.
- Negativo de perfil: `MODE=LEVEL` enviado ao sensor de vibração.
- Negativo de faixa: `RATE_HZ=5000`.
- Negativo de tempo: desligue o sensor e mande a configuração em seguida.
- Negativo de caminho: ações que só existem no console USB (`ACQ`, `WIFI`, `DTC_LIST`).

**Saídas esperadas**

- `ACK COMMAND=CONFIG STATE=ACCEPTED ...` e depois `STATE=APPLIED`.
- `[GW] WIRELESS_CONFIG node=1 child=1 status=APPLIED mode=STRUCTURAL rate_req_hz=500.00 rate_eff_hz=... window=HAMMING window_size=256 stalta=3.500 gain=1.2500 profile=VIBRATION`.
- Na estação, "Aplicada pelo sensor"; no console do sensor, `CONFIG_APPLIED ...`.
- Negativos: `REJECTED REASON=PROFILE_MISMATCH`; `REJECTED` com o alarme `0x4006`; `REJECTED REASON=SENSOR_TIMEOUT` depois de 3 s; `REASON=UNSUPPORTED_VIA_NODE`.

**Se sair isso; faça isso**

- Se sair `REJECTED REASON=CONFIG_UNKNOWN`; o módulo ainda não conhece a configuração atual do sensor (sessão recém-aberta). Mande `ACTION=STATUS` e tente de novo.
- Se ficar em `ACCEPTED` para sempre; a resposta do sensor se perdeu. Em 3 s deveria virar `SENSOR_TIMEOUT`.

**Condição de aceite:** configuração válida aplicada e confirmada nos três lugares (Probe, estação, console do sensor); os quatro negativos recusados com o motivo certo. Restaure a configuração original.

**Registro:** data e hora · placa e ID: sensor (UUID) e módulo responsável · versão · resultado · medido: não há · inesperado.

### [ ] RT 60 - Perda e retorno do sensor; desassociação

Prioridade: vital.

**Comandos necessários**

1. Com a sessão aberta, tire a alimentação do sensor e marque o tempo.
2. Espere 60 s e religue.
3. Depois de a sessão voltar, desassocie:

```text
WIRELESS UNBIND 1 0x<UUID>
```

**Variações possíveis**

- Pela tela: `d` na lista Associados, com confirmação.

**Saídas esperadas**

- Em ≈ 10 s sem tráfego: `[WLINK] child=1 sessao ...` no módulo, `event=SESSION_DOWN` e o canal de volta a `OFFERING`.
- Vínculo: `ONLINE`, depois `STALE` (15 s sem anúncio) e `LOST` (mais 45 s).
- Ao religar: o sensor volta a `SECURE` sozinho, com o mesmo `01.01`, sem nova associação.
- `UNBIND`: `[GW] WIRELESS_ASSOC ... state=UNBOUND`; no módulo, `[WLINK] AP=OFF` (se era o último sensor); no sensor, `[NET] Wi-Fi desligado` e `NET STATE=DISABLED`.

**Se sair isso; faça isso**

- Se o sensor não voltar sozinho; veja as linhas `[NET]`. Ele precisa receber uma oferta nova; o módulo tem de estar em `OFFERING`.
- Se o sensor continuar aparecendo na estação depois do `UNBIND`; a confirmação `UNBOUND` não chegou. A estação só remove com a confirmação do módulo.
- Se o Wi-Fi do sensor continuar ligado depois de desassociado; registre: a desassociação deveria apagar o nome e a senha da rede.

**Condição de aceite:** sessão encerrada em ≈ 10 s, estados `STALE` e `LOST` nos prazos, retorno automático, e desassociação levando o sensor a `DISABLED`. Anote os três tempos.

**Registro:** data e hora · placa e ID: sensor (UUID) e módulo responsável · versão · resultado · medido: os três tempos (s): até a sessão encerrar, até `STALE` e até `LOST` · inesperado.

### [ ] RT 61 - Reassociação automática: o módulo responsável cai

Prioridade: vital.

**Comandos necessários**

1. Associe o sensor a um seguidor com `AUTO`, num ponto em que outro módulo também o ouça.
2. Com a telemetria fluindo, tire a alimentação do módulo responsável e marque o tempo.
3. Depois de tudo estabilizar, religue o módulo.

**Variações possíveis**

- O responsável é também o líder: soma-se o tempo da eleição.
- Sem a Probe não há como observar; use o monitor de um módulo.

**Saídas esperadas**

- O líder marca o módulo em falha (5 sinais de presença) e decide em ≈ 5 s: `[GW] WIRELESS_FAILOVER uuid=0x<UUID> from=1 to=2 reason=OWNER_LOST rssi=<n>`.
- O sensor desiste da sessão em 10 s e só aceita a oferta de outro módulo depois de 15 s.
- A telemetria volta pelo novo módulo em ≈ 25 s, como `TEL NODE=2 CHILD=<n>`.
- Ao religar o módulo antigo: ele escuta por 12 s, vê que outro assumiu o sensor e descarta o vínculo gravado. Não disputa.

**Se sair isso; faça isso**

- Se não houver `WIRELESS_FAILOVER`; confira a política (`failover=AUTO`) e se o destino ouviu o sensor nos últimos 12 s com pelo menos −90 dBm.
- Se o destino publicar `STALE` em 15 s; ele aceitou mas não ouve o sensor. O líder escolhe outro, respeitando 30 s entre decisões.
- Se o sensor ficar alternando entre módulos; registre os instantes. As esperas de 15 s e 30 s existem para impedir isso.
- Se os dois módulos mantiverem o sensor depois da volta; a regra é: cede quem não tem sessão.

**Condição de aceite:** o sensor troca de módulo sem intervenção, a telemetria volta em ≈ 25 s e o módulo antigo retorna sem disputar. Anote: falha → decisão, decisão → sessão, e o maior intervalo sem telemetria. Compare com o `owner_failover` do RT 6.

**Registro:** data e hora · placa e ID: sensor (UUID), módulo que caiu e módulo que assumiu · versão · resultado · medido: falha → decisão, decisão → sessão e o maior intervalo sem telemetria (s) · inesperado.

### [ ] RT 62 - Reassociação por perda de alcance e política manual

Prioridade: recomendado.

**Comandos necessários**

1. Sensor em `AUTO` no módulo 1. Leve o sensor (num power bank) para longe do módulo 1 e para perto do módulo 2, até a sessão cair.
2. Depois, mude a política e repita o RT 61:

```text
WIRELESS POLICY 2 0x<UUID> MANUAL
```

**Variações possíveis**

- Se a bancada é pequena demais para separar os alcances, embrulhe o módulo 1 em papel-alumínio aterrado para atenuar o rádio dele.

**Saídas esperadas**

- Perda de alcance: `[GW] WIRELESS_FAILOVER ... reason=SENSOR_LOST`, depois de o destino ouvir o sensor por 6 s seguidos.
- Política manual: com o responsável desligado, **ninguém** assume. O vínculo fica como estava e o sensor entra em Precisa de atenção até o operador associá-lo de novo.

**Se sair isso; faça isso**

- Se não houver decisão por `SENSOR_LOST`; o responsável ainda ouve o sensor de vez em quando. Nada é decidido enquanto ele o ouvir depois do último estado publicado.
- Se, na política manual, alguém assumir o sensor; defeito: a reassociação só pode ocorrer com autorização.

**Condição de aceite:** troca por `SENSOR_LOST` observada pelo menos uma vez, e nenhuma troca com a política manual.

**Registro:** data e hora · placa e ID: sensor (UUID) e os módulos envolvidos · versão · resultado · medido: trocas observadas com cada política · inesperado.

### [ ] RT 63 - Reinício do módulo: vínculos gravados

Prioridade: recomendado. Implementado e ainda não validado em bancada física.

**Comandos necessários**

Com um sensor associado em política **manual** e a sessão aberta, pressione o botão EN do módulo responsável.

**Variações possíveis**

- Durante os 12 s de espera, desassocie o sensor pela estação: o vínculo gravado deve ser descartado.

**Saídas esperadas**

- O módulo reinicia, entra na rede e escuta por 12 s antes de retomar a tabela.
- O vínculo volta com o mesmo `NN.CC`, o módulo volta a oferecer o canal e o sensor se autentica de novo, sem ação do operador.
- Nesses 12 s a estação mostra o sensor sem sessão.

**Se sair isso; faça isso**

- Se o vínculo não voltar; a tabela na memória não volátil foi rejeitada (CRC) ou não foi gravada. Associe de novo e repita uma vez para confirmar.
- Se o filho voltar com outro número; registre: o número deveria ser preservado.

**Condição de aceite:** depois do reinício, o mesmo sensor volta com o mesmo identificador e a telemetria retoma sozinha. Anote o tempo do reinício à primeira leitura.

**Registro:** data e hora · placa e ID: módulo reiniciado e sensor (UUID) · versão · resultado · medido: tempo do reinício à primeira leitura (s) · inesperado.

### [ ] RT 64 - Sensor acústico de ponta a ponta

Prioridade: vital se houver sensor acústico.

**Comandos necessários**

Associe o sensor acústico (suponha `02.01`) e envie:

```text
CMD TARGET=02.01 ACTION=CONFIG TX=E1 MODE=MACHINE
CMD TARGET=02.01 ACTION=BASELINE_LEARN SECONDS=10 TX=E2
CMD TARGET=02.01 ACTION=BASELINE_CLEAR TX=E3
CMD TARGET=02.01 ACTION=CONFIG TX=E4 MODE=LEVEL
CMD TARGET=02.01 ACTION=MEASURE_RESET TX=E5
CMD TARGET=02.01 ACTION=FFT TX=E6 BINS=64
```

**Variações possíveis**

- Pela tela do sensor: `z` reinicia a medição, `a` aprende a assinatura (modo de máquina), `f` mostra o espectro em dB SPL.
- `THRESHOLD` e `FFT_SIZE` são sinônimos de `STALTA` e `WINDOW_SIZE`.
- Negativo: `MODE=ROTATING` enviado ao sensor acústico; `ACTION=MEASURE_RESET` enviado ao sensor de vibração.

**Saídas esperadas**

- Descoberta com `profile=ACOUSTIC`.
- `TEL NODE=2 CHILD=1 SEQ=<n> ... PROFILE=ACOUSTIC MODE=LEVEL ACQ=I2S WIN=1024 RATE_EFF_HZ=16000 INTERVAL_MS=1000 COVERAGE_PCT=100 CPU_PCT=<n> ... LAEQ_DB=... OCT_DB=...`, uma por segundo.
- `WIRELESS_CONFIG ... profile=ACOUSTIC`.
- As ações acústicas confirmam `ACCEPTED` e depois o resultado, com `DETAIL` igual ao número da ação (1, 2 ou 3).
- Negativos: `REJECTED REASON=PROFILE_MISMATCH`.
- A tela do sensor mostra níveis, bandas de oitava, o painel do modo e a leitura do ambiente.

**Se sair isso; faça isso**

- Se a ocupação do barramento subir mais que o esperado; um relatório acústico ocupa 19 quadros por segundo, contra 9 do de vibração. Compare com o RT 26.
- Se frequência dominante, centroide e planura sumirem da tela; `SPECTRUM_VALID=NO` naquele segundo (nenhum quadro passou pela FFT).

**Condição de aceite:** relatório por segundo sem salto de `SEQ`, as seis ações confirmadas e os dois negativos recusados por perfil.

**Registro:** data e hora · placa e ID: sensor acústico (UUID) e módulo responsável · versão · resultado · medido: não há · inesperado.

### [ ] RT 65 - Dois sensores no mesmo módulo e limite de quatro

Prioridade: recomendado.

**Comandos necessários**

Associe dois sensores ao mesmo módulo, um logo após o outro, e acompanhe a Criação do canal de dados de cada um.

**Variações possíveis**

- Com 5 dispositivos (sensores e painel contam): tente associar o quinto ao mesmo módulo.
- Dois sensores em módulos diferentes e queda de um módulo (cenário `two_sensors`).

**Saídas esperadas**

- Filhos `01.01` e `01.02`; as ofertas se revezam a cada 400 ms enquanto os dois esperam.
- As duas sessões abrem; na bancada virtual, cada uma em ≈ 5 s.
- Duas linhas `TEL` por segundo, com `SEQ` independentes por filho.
- Quinto dispositivo: `state=REJECTED` (não há filho livre).

**Se sair isso; faça isso**

- Se o segundo sensor demorar minutos ou nunca abrir; é exatamente o sintoma antigo que a correção do nonce e da escuta BLE em 25 % deveria ter eliminado. Guarde os três registros (módulo, dois sensores).
- Se a sessão do primeiro cair quando o segundo entra; anote `WIRELESS_EVENT` dos dois.

**Condição de aceite:** dois sensores com sessão no mesmo módulo, telemetria contínua dos dois, e o quinto dispositivo recusado. Anote o tempo de abertura de cada sessão.

**Registro:** data e hora · placa e ID: os dois sensores (UUID) e o módulo · versão · resultado · medido: tempo de abertura de cada sessão (s) · inesperado.

### [ ] RT 66 - Painel de operação de ponta a ponta

Prioridade: recomendado. Todo o painel CYD está pendente de bancada física.

**Comandos necessários**

1. Ligue o painel; ele aparece em Sensores sem fio como os sensores. Associe-o a um módulo.
2. Toque em Rede, depois em Situação, depois em Marcar.
3. Tire a alimentação de um seguidor e observe; toque em Ciente.

**Variações possíveis**

- Ligue o painel com a estação **desconectada** desde o boot da rede: sem hora, só tempos relativos ("há 3 min").

**Saídas esperadas**

- `PANEL NODE=<n> CHILD=<c> TYPE=STATUS STATE=AWAKE SUPPLY=USB SLEEP_S=0 LEVEL=0 ... CLOCK=YES ... ALERT=...`.
- Cabeçalho com o nível, o módulo e a hora; página Rede com líder, módulos, ocupação e cada sensor com a última leitura.
- Marcar: `PANEL ... TYPE=MARK SEQ=<n> KIND=BUTTON ...`, e a marca na linha do tempo do Histórico (`F11`, teclas `m` e `M`).
- Com o seguidor fora: o nível sobe, o LED e os bipes acompanham (azul pulsando e 1 bipe quando a rede trata sozinha; amarelo piscando e 2 bipes em atenção; vermelho e 3 bipes em "requer você").
- Ciente: os bipes param, sai `KIND=ACK`, e a estação registra "Ciente em `<n>` s: ...".

**Se sair isso; faça isso**

- Se o painel não mostrar hora; nenhuma estação se conectou à rede desde que ele ligou. Conecte: a hora chega em segundos.
- Se a tela ficar atrasada em relação à estação; o painel redesenha no máximo uma vez por segundo e recebe o tráfego em lotes de 200 ms. Até 1 a 2 s é o previsto.
- Se os bipes não saírem; confira o alto-falante no conector `SPEAK`.

**Condição de aceite:** painel associado, páginas coerentes com a estação, marca registrada no histórico, alerta sinalizado por LED e som e reconhecido com Ciente. Anote o tempo de redesenho e o atraso da hora.

**Registro:** data e hora · placa e ID: painel (UUID) e módulo responsável · versão · resultado · medido: tempo de redesenho e atraso da hora · inesperado.

### [ ] RT 67 - Estação: limites de alarme, exportação, histórico e reprodução

Prioridade: recomendado.

**Comandos necessários**

1. Na tela do sensor, `l`: defina um limite de valor eficaz pouco acima do piso do RT 39 e bata na mesa.
2. Métricas (`F8`), `x`.
3. Histórico (`F11`): escolha a sessão atual.
4. Feche a estação e reproduza a sessão:

```bash
cd Front
.venv/bin/python -m pico_tui --replay logs/gravacao_<data>_<sessão>.log --replay-speed 4
grep -c '\*\*\*' logs/gravacao_<data>_<sessão>.log
```

**Variações possíveis**

- Supervisório (`F12`): até cinco painéis; `a` acrescenta, `x` remove.

**Saídas esperadas**

- O alarme acende ao passar do limite e apaga ao voltar abaixo do limite menos a folga (5 % para valor eficaz e PPV, 1 dB para níveis).
- Arquivos JSON e CSV em `Front/exports/`.
- Histórico com os eventos da sessão e capturas em `Front/logs/capturas/<data>/<id>/`.
- No Histórico, a faixa `AUDIT` (símbolo ◈) traz cada alteração feita pela estação, com o autor. A mudança de limites do passo 1 está lá como `ALARM_LIMITS`, com o valor anterior e o novo.
- A reprodução mostra a mesma sequência de eventos, sem enviar comando nenhum.
- Tokens gravados como `***`: o token real não aparece no arquivo.

**Se sair isso; faça isso**

- Se o token aparecer em claro na gravação; defeito de segurança. Apague o arquivo e registre.
- Se a reprodução divergir do que você viu ao vivo; guarde o arquivo: é um caso de teste pronto para a suíte da estação.
- Se a mudança de limites for recusada; há operadores cadastrados e ninguém se identificou, ou quem entrou tem o perfil de observador. A tentativa também fica registrada na faixa `AUDIT`.

**Condição de aceite:** alarme por limite funcionando, exportação gerada, sessão reproduzível e sem segredos no arquivo. Guarde os arquivos de `exports/` junto do registro da sessão.

**Registro:** data e hora · placa e ID: Probe 00 e sensor (UUID) · versão · resultado · medido: limite de alarme usado e nomes dos arquivos de `exports/` · inesperado.

## Fase 7. Robustez: falhas e alimentação

Os ensaios `FAULT` provocam, por comando, as mesmas falhas que você provocou à mão nas fases 4 e 6, e a estação mede a reação sozinha. Faça-os pela tela Ensaios (`F9` ou `e`), que pede o módulo e a duração, mostra o comando e pede confirmação. Duração de 1 a 120 s; um ensaio por vez em cada módulo. Depois de cada um, *Limpar falha* no módulo e exporte com `x`.

### [ ] RT 68 - Ensaio de queda: líder e módulo com sensor

Prioridade: vital.

**Comandos necessários**

```text
FAULT POWER_CYCLE <ID do líder> 20
FAULT POWER_CYCLE <ID do módulo com sensor em AUTO> 30
```

**Variações possíveis**

- Encerrar antes da hora: `FAULT CANCEL <ID>`, ou `s` na tela.
- Teste negativo: um segundo `FAULT` no mesmo módulo durante o primeiro.

**Saídas esperadas**

- `[GW] FAULT_TX kind=POWER_CYCLE node=<n> duration_ms=20000 seq=<n>`.
- `[GW] FAULT_EVENT node=<n> kind=POWER_CYCLE state=STARTED duration_ms=20000 seq=<n>`.
- O módulo sai do barramento (só escuta), desliga os rádios e reinicia ao final.
- Queda do líder: nova eleição, como no RT 33. Queda do módulo com sensor: `WIRELESS_FAILOVER reason=OWNER_LOST`, como no RT 61.
- A tela mostra o tempo até a falha ser percebida, até a rede voltar e o maior intervalo sem telemetria. Medida não obtida aparece como "não observado", nunca como zero.
- Negativo: `state=REJECTED` (estado 3).

**Se sair isso; faça isso**

- Se o módulo ficar mudo depois do ensaio (vivo no monitor, ausente na rede); a volta do modo somente escuta do MCP2515 falhou. O firmware repete o pedido a cada 100 ms (CH-046, pendente de bancada). Reinicie a placa e registre.
- Se não vier `FAULT_EVENT`; o módulo foi compilado com `IOT_FAULT_INJECTION=0` e ignora o comando (RT 73).
- Se os tempos diferirem muito dos medidos à mão nos RT 33 e 61; desconfie da medição manual primeiro. A do ensaio vem dos carimbos da Probe.

**Condição de aceite:** os dois ensaios começam, terminam e a rede volta ao estado normal sem intervenção além de *Limpar falha*. Anote os tempos e compare com os RT 33 e 61.

**Registro:** data e hora · placa e ID: módulo de cada ensaio · versão · resultado · medido: tempos dos dois ensaios, pelos carimbos da Probe · inesperado.

### [ ] RT 69 - Ensaios de módulo congelado, ponto de acesso e sessões

Prioridade: recomendado.

**Comandos necessários**

```text
FAULT SILENCE <ID do líder> 20
FAULT AP_OUTAGE <ID do módulo com sensor> 15
FAULT DROP_SESSION <ID do módulo com sensor>
```

**Variações possíveis**

- `FAULT SILENCE <líder> 45` com um sensor em `AUTO` noutro módulo: o líder que retoma não pode reassociar um sensor que continuou funcionando (cenário `silence_leader_with_sensor`).

**Saídas esperadas**

- `SILENCE`: o módulo sai do barramento e para o programa; ao final retoma **com o estado que tinha**, achando que ainda é líder. A rede resolve pela regra do maior ID e termina com um líder só e sem falhas indevidas. No módulo: `[LACO] Parado por <ms> ms. Vigilancias rearmadas`.
- `AP_OUTAGE`: sessões encerradas e Wi-Fi desligado (`[WLINK] AP=OFF`, `event=AP_DOWN reason=TEST`); ao final o ponto de acesso volta e o sensor se autentica de novo.
- `DROP_SESSION`: `[WLINK] child=<n> sessao descartada (ensaio)`; o sensor percebe e refaz a autenticação.

**Se sair isso; faça isso**

- Se, depois do `SILENCE`, seguidores forem marcados em falha; o líder contou o próprio congelamento como silêncio dos outros (CH-031 dizia estar corrigido).
- Se, depois do `SILENCE`, ficarem dois líderes por mais de alguns ciclos; registre os monitores dos dois.
- Se o ponto de acesso não voltar depois do `AP_OUTAGE`; o firmware tenta religá-lo a cada 5 s enquanto houver vínculo (CH-047, pendente de bancada). Anote quanto tempo levou.
- Se o sensor não se autenticar de novo depois do `DROP_SESSION`; veja as linhas `[NET]`: ele leva até 10 s para desistir da sessão antiga.

**Condição de aceite:** nos três ensaios a rede e o canal de dados voltam sozinhos, com um único líder e nenhum sensor reassociado sem motivo. Anote o tempo sem telemetria em cada um.

**Registro:** data e hora · placa e ID: módulo de cada ensaio · versão · resultado · medido: tempo sem telemetria em cada um dos três ensaios (s) · inesperado.

### [ ] RT 70 - Ensaio de carga no barramento

Prioridade: recomendado.

**Comandos necessários**

Com a telemetria de pelo menos um sensor fluindo e `METRICS ON`:

```text
FAULT LOAD 40 15 HIGH
FAULT LOAD 40 15 LOW
FAULT LOAD 80 15 MIX
```

**Variações possíveis**

- Carga de 1 a 90 %. Prioridades: `HIGH` (`0x010`), `MID` (`0x1F0`), `LOW` (`0x7EF`), `MIX` alterna as três. Um quarto argumento fixa a semente.
- Suba a carga em passos (20, 40, 60, 80, 90) para achar o máximo que a Probe sustenta.

**Saídas esperadas**

- Ao final: `[GW] FAULT_EVENT node=0 kind=BUS_LOAD state=ENDED duration_ms=15000 seq=<n> load_pct=40 priority=HIGH frames=<n> achieved_pct=<≈40>`.
- Durante: `BUS_STATS load=` perto de 41 %.
- Carga `HIGH`: o sinal de presença (`0x100`) perde a arbitragem para `0x010` e o `dt_max_us` dele cresce; os dados dos sensores (`0x380+N`) atrasam mais.
- Carga `LOW`: nada do projeto deveria atrasar; `0x7EF` perde para tudo.
- Nenhuma eleição, nenhum módulo em falha.

**Se sair isso; faça isso**

- Se `achieved_pct` ficar bem abaixo do pedido; é o limite do que a Probe consegue gerar pelo MCP2515. Esse número é uma das pendências de bancada: anote-o para cada carga.
- Se um módulo for marcado em falha com 40 % `HIGH`; o sinal de presença atrasou além de 5 períodos. Registre a carga em que isso começa.
- Se `rx_peak` chegar perto de 128; a fila de recepção da Probe está no limite e ela vai perder quadros.
- Se `SEQ` da telemetria saltar sob carga; anote a carga e a prioridade. É perda de remontagem (1,5 s sem completar descarta a transferência).

**Condição de aceite:** com 40 % de carga alta por 15 s a rede mantém o líder, não declara falhas e a telemetria continua. Monte a tabela carga pedida × `achieved_pct` × `dt_max_us` de `0x100` × saltos de `SEQ`.

**Registro:** data e hora · placa e ID: Probe 00 e todos os módulos · versão · resultado · medido: tabela carga pedida × `achieved_pct` × `dt_max_us` de `0x100` × saltos de `SEQ` · inesperado.

### [ ] RT 71 - Alimentação e barramento degradados

Prioridade: recomendado.

**Comandos necessários**

1. Ligue todos os módulos ao mesmo tempo (chave geral do hub), com o monitor de um deles aberto. Repita 5 vezes.
2. Com a rede e a telemetria normais, retire **um** resistor de terminação por 60 s e recoloque.
3. Com um sensor associado ao módulo 1, solte o CAN_H só do módulo 1, mantendo-o alimentado, por 60 s.

**Variações possíveis**

- Passo 1 com um power bank, só para ver a falha: os módulos reiniciam juntos, em laço.
- Meça o 5 V no pino de um ESP32 durante a partida, se tiver osciloscópio.

**Saídas esperadas**

- Passo 1: nenhum `rst:0xf (BROWNOUT_RST)` no boot; todos chegam a `[INIT] Pronto`.
- Passo 2: `rx_err` e `tx_err` sobem, podem aparecer `xfer_err` e saltos de `SEQ`; ao recolocar o resistor, os contadores param de subir e a rede segue, sem reiniciar nada.
- Passo 3: o líder declara o módulo 1 em falha. O módulo isolado percebe o próprio isolamento: depois de 6 s sem quadro e com as transmissões sem confirmação ele escreve `[ISOLADO] sem quadros do barramento ha ... ms e transmissoes sem confirmacao (TEC=... EFLG=0x...)`, libera o sensor (`CMD_RELEASE`), desliga o ponto de acesso e não pede eleição; o líder entrega o sensor a outro módulo e a telemetria volta por ele cerca de 8 s depois de o cabo ser solto (bancada virtual; confirme aqui). `ISOLAMENTO` na serial mostra o estado. **Ao recolocar o CAN_H** o módulo escreve `[ISOLADO] barramento de volta apos ... ms` depois de 3 s de quadros sem interrupção, e **não** disputa o sensor nem a liderança. Quadros que ficaram na fila do controlador durante o isolamento podem sair na volta do cabo (limite do MCP2515; aparecem como telemetria antiga com `SEQ` fora de ordem).

**Se sair isso; faça isso**

- Se sair `BROWNOUT_RST`; a fonte não sustenta o pico. Use fonte de 5 V com 3 A ou mais, cabos curtos, e um capacitor de 470 a 1000 µF entre 5 V e GND perto de cada placa.
- Se a Probe mostrar `eflg=0x20`; ela saiu do barramento (*bus-off*). Anote se volta sozinha ou se precisa reiniciar.
- Se, no passo 3 com política `AUTO`, o sensor **não** migrar; é consequência do limite: o sensor tem sessão ativa com o módulo isolado e ignora outras ofertas. Anote o que aconteceu; é o dado que justifica a melhoria prevista (detecção de isolamento).

**Condição de aceite:** 5 partidas simultâneas sem reinicio; a rede sobrevive a 60 s sem uma terminação e se recupera sozinha; o comportamento do módulo isolado fica documentado com tempos.

**Registro:** data e hora · placa e ID: a bancada inteira e a fonte usada · versão · resultado · medido: partidas sem reinício (de 5), tempo de recuperação sem uma terminação e tempos do módulo isolado · inesperado.

## Fase 8. Segurança, operador e auditoria

Três proteções são testadas aqui, da placa à pessoa: a chave de cada sensor, a autorização da estação para alterar a rede e a identificação de quem opera, com o registro do que cada um fez. A fase vem depois da robustez de propósito: o RT 72 regrava sensores, o RT 73 regrava um módulo e o RT 74 muda o comportamento da estação dali em diante.

### Chaves do sensor e autorização na estação

### [ ] RT 72 - Chaves próprias do sensor e sensor com chave alheia

Prioridade: recomendado (vital antes de uso permanente).

**Comandos necessários**

```bash
./Codigo/scripts/provision_sensor_security.sh --sensor <UUID do sensor A>
./Codigo/scripts/build_pico.sh --sensor <UUID do sensor A>
```

Grave o `.uf2` no sensor A e, no console dele, `NET`. Depois grave **o mesmo** `.uf2` num sensor B e repita.

**Variações possíveis**

- Sensor acústico: acrescente `--audio`. Painel: `build_paper.sh --sensor <UUID>`.
- Sensor gravado sem chave nenhuma.

**Saídas esperadas**

- Build: `Enlace: chaves derivadas para o sensor 0x<UUID>`.
- Sensor A: `LINK_KEYS=DEVICE`, e o canal de dados abre como no RT 55.
- Sensor B com as chaves do A: `LINK_KEYS=UUID_MISMATCH`. Ele ignora as ofertas, nunca liga o Wi-Fi e não abre sessão, mesmo associado.
- Sem chave: `LINK_KEYS=NONE`, mesmo comportamento.

**Se sair isso; faça isso**

- Se o sensor B abrir sessão; falha de segurança grave. Pare e registre tudo.
- Se o sensor A não abrir sessão com `LINK_KEYS=DEVICE`; a chave mestra do `.env.local` mudou entre gravar os módulos e gerar as chaves do sensor.

**Condição de aceite:** o sensor com chaves próprias funciona; o sensor com chave de outro UUID e o sensor sem chave são associáveis mas nunca chegam a `SECURE`, e nenhum dado deles vai ao barramento. Ao final, regrave o sensor B com as chaves do próprio UUID (ou com o firmware de bancada), para que ele volte a abrir sessão antes do teste de longa duração.

**Registro:** data e hora · placa e ID: sensores A e B (UUID) · versão: o commit e o modo de chave gravado em cada sensor, inclusive o que ficou no sensor B ao final · resultado · medido: estado final do canal de cada sensor (as chaves não vão para o caderno) · inesperado.

### [ ] RT 73 - Autorização na estação e firmware sem ensaios

Prioridade: recomendado (vital antes de qualquer uso fora da bancada).

**Comandos necessários**

Abra a estação no modo padrão, **sem** a YubiKey:

```bash
./Codigo/scripts/run_tui.sh <porta da Probe>
```

Pelo lançador (`k`):

```text
22 20 FF 00
22 00 FF 01
22 0 FF 1
0x22 00 FF 01
WIRELESS UNBIND 1 0x<UUID>
FAULT LOAD 10 5 LOW
METRICS RESET
```

Depois, grave um módulo sem os ensaios e mande um ensaio para ele:

```bash
PLATFORMIO_BUILD_FLAGS="-D IOT_FAULT_INJECTION=0" ./Codigo/scripts/upload_esp32_can_node.sh 2 <porta>
```

```text
FAULT POWER_CYCLE 2 10
```

**Variações possíveis**

- Com a YubiKey conectada, os comandos bloqueados passam.
- `--security-mode otp`: bloqueia tudo o que altera a rede (a verificação OTP/FIDO2 ainda não existe).
- Faça este RT antes de cadastrar operadores (RT 74). Com o cadastro feito, as duas proteções se somam: em `presence`, o comando precisa da chave conectada **e** de um operador identificado, e a recusa pode vir por qualquer um dos dois motivos.

**Saídas esperadas**

- Livres: `22 20 FF 00` e `METRICS RESET` (também `TIME`).
- Bloqueados sem a chave física: a eleição nas **três grafias**, o `UNBIND` e o `FAULT`.
- Módulo sem ensaios: `FAULT_TX` sai da Probe, mas não vem `FAULT_EVENT` do módulo 2 e ele continua funcionando.

**Se sair isso; faça isso**

- Se `22 0 FF 1` ou `0x22 00 FF 01` passar sem autorização; a classificação pela forma canônica (CH-049) regrediu. Defeito de segurança.
- Se o módulo sem ensaios obedecer ao `FAULT`; a opção de compilação não entrou. Confira a linha de build.
- Lembre: desligar a autorização na estação não protege o barramento. Qualquer equipamento ligado ao CAN pode emitir `0x080`, `0x304` e `0x305`; é um limite documentado. Com `--auth-can` (RT 88), só `0x304` e `0x305` passam a exigir a etiqueta; `0x080` continua aberto.

**Condição de aceite:** os comandos de leitura passam, os que alteram a rede são bloqueados em todas as grafias, e o módulo compilado sem ensaios ignora o comando de falha. Regrave o módulo 2 com o firmware de bancada ao final.

**Registro:** data e hora · placa e ID: módulo 2 · versão: o commit e o firmware do módulo 2 (sem ensaios durante o teste, de bancada ao final) · resultado · medido: não há · inesperado.

### Identificação do operador e auditoria

Acrescentada em 12/11, com o commit `b8fcbab`. A estação passa a saber quem a usa e grava, em cada alteração da rede, o nome de quem a fez. Tudo isso acontece no computador: nenhuma placa muda.

Três cuidados antes de começar:

- **A senha nunca vai para o caderno**, nem para a linha de comando. A ferramenta a pede no terminal, sem mostrar. Ela tem de 8 a 256 caracteres; uma frase longa vale mais que uma palavra com símbolos.
- **Cadastrar um operador muda o comportamento da estação** dali em diante: toda alteração na rede passa a exigir login. Para voltar ao comportamento anterior, remova todos os operadores (`./Codigo/scripts/operadores.sh remove <nome>`).
- **`operadores.sh` roda dentro de `Front/`**, a mesma pasta em que `run_tui.sh` grava o histórico. É por isso que o `audit` encontra o banco sem você informar o caminho.

### [ ] RT 74 - Cadastrando operadores

Prioridade: recomendado (vital se mais de uma pessoa usa a estação). Não precisa de hardware.

**Comandos necessários**

```bash
./Codigo/scripts/operadores.sh check
./Codigo/scripts/operadores.sh add <nome> --display "Seu Nome"
./Codigo/scripts/operadores.sh add visita --role observer
./Codigo/scripts/operadores.sh list
./Codigo/scripts/operadores.sh check
ls -l ~/.config/iot-over-can/operators.json
```

**Variações possíveis**

- Trocar a senha: `passwd <nome>`. Desativar sem apagar: `disable <nome>` e `enable <nome>`. Trocar o perfil: `role <nome> operator` ou `observer`. Remover: `remove <nome>` (pede confirmação; `--yes` dispensa).
- Em scripts, a senha entra pela entrada padrão com `--password-stdin`. Nunca como argumento.
- Outro arquivo de cadastro: `./Codigo/scripts/operadores.sh --file <arquivo> list` (a opção vem antes do subcomando).
- Testes negativos: senha com menos de 8 caracteres; nome com maiúscula ou espaço (`"Ana Maria"`); nome repetido; senhas diferentes na confirmação.

**Saídas esperadas**

- Primeiro `check`: `Sem cadastro em /home/<conta>/.config/iot-over-can/operators.json: a TUI identifica o usuário do sistema.`
- `add`: pergunta `Senha de <nome>:` e `Repita a senha:`, sem mostrar o que você digita, e responde `Operador <nome> cadastrado (operador). Cadastro: .../operators.json`. No segundo: `Operador visita cadastrado (observador).`
- `list`: uma tabela com as colunas `NOME`, `PERFIL`, `ESTADO`, `CRIADO` e `NOME COMPLETO`; os dois operadores como `ativo`.
- Segundo `check`: `Cadastro válido: 2 operador(es), 2 ativo(s). Arquivo: .../operators.json`.
- `ls -l`: permissão `-rw-------`.
- Negativos, todos com código de saída 1: `Erro: A senha precisa de pelo menos 8 caracteres.`; `Erro: Nome inválido: use de 2 a 32 caracteres, começando por letra minúscula, com letras, números, ponto, hífen ou sublinhado.`; `Erro: Operador já cadastrado: <nome>`; `Erro: As senhas não conferem. Nada foi alterado.`

**Se sair isso; faça isso**

- Se sair `PROBLEMA: operators.json deve usar permissões 0600`; `chmod 600 ~/.config/iot-over-can/operators.json`. Com a permissão errada, a estação bloqueia as alterações em vez de liberá-las (RT 77).
- Se sair `Erro: A senha não pode ser igual ao nome do operador.`; escolha outra.
- Se sair `AVISO: nenhum operador ativo; a TUI vai bloquear os comandos que alteram a rede.`; todos estão desativados: reative um com `enable`. Com o cadastro **vazio**, o mesmo aviso aparece, mas no modo padrão a estação não bloqueia: volta a responder pela conta do Linux.
- Se `iot-over-can-operators` não for encontrado; use `./Codigo/scripts/operadores.sh`, que não depende do `PATH`, ou refaça o RT 3.
- Se esquecer a senha; `passwd <nome>` grava uma nova sem pedir a antiga. É proposital: quem controla a conta do Linux controla o cadastro.

**Condição de aceite:** um operador e um observador cadastrados, arquivo com permissão 600, e os quatro testes negativos recusados sem alterar o cadastro. Anote no caderno os nomes de acesso, nunca as senhas.

**Registro:** data e hora · placa e ID: não se aplica (o teste é da estação) · versão · resultado · medido: nomes de acesso e perfis cadastrados (as senhas não vão para o caderno) · inesperado.

### [ ] RT 75 - Entrando e saindo da sessão do operador

Prioridade: recomendado. Basta a estação aberta; pode ser com `--demo`.

**Comandos necessários**

```bash
SECURITY_MODE=off ./Codigo/scripts/run_tui.sh <porta da Probe>
```

Na estação:

1. Tecle `o`, informe o nome e uma senha **errada**.
2. Informe a senha certa.
3. Tecle `o` de novo e leia a janela.
4. Escolha **Sair da sessão**.

**Variações possíveis**

- Pelo campo de comando manual (`F4`, depois `a`): `:login`, `:logout` e `:operador`.
- Cinco senhas erradas seguidas, e depois a senha certa.
- Sem hardware: `cd Front && .venv/bin/iot-over-can-tui --demo --security-mode off`.

**Saídas esperadas**

- Antes de entrar, na faixa de status: `Operador sem login`.
- A janela **Operador**: "Identifique-se para alterar a rede. A sessão expira por inatividade.", os campos Operador e Senha (a senha não aparece) e os botões Entrar e Cancelar.
- Senha errada: `Nome ou senha incorretos.` A mensagem é a mesma para nome inexistente.
- Senha certa: aviso `<Seu Nome> identificado.` e a faixa passa a `Operador <Seu Nome>`.
- `o` com a sessão aberta: `Operador: <Seu Nome>  perfil: operador` e `A sessão expira por inatividade em 15 min.`, com os botões Sair da sessão e Fechar.
- Ao sair: aviso `<Seu Nome> saiu.` e a faixa volta a `Operador sem login`.
- Cinco erros seguidos: `Muitas tentativas. Aguarde 30 s.` Durante a espera, nem a senha certa entra. O tempo dobra a cada novo bloqueio.

**Se sair isso; faça isso**

- Se a faixa mostrar `Operador <conta> (sistema)` em vez de `Operador sem login`; a estação não achou cadastro: você cadastrou com outra conta do Linux, ou com `--file` noutro arquivo. A janela do `o` confirma: `Responsável pelos comandos: <conta>@<máquina> (usuário do sistema, não verificado).`
- Se sair `Cadastro de operadores inválido: ...`; rode o `check` do RT 74.
- Se sair `Nenhum operador ativo no cadastro. Use iot-over-can-operators add NOME.`; todos foram desativados.
- Se o bloqueio de tentativas incomodar na bancada; fechar e abrir a estação zera a contagem. É um limite documentado, não um defeito.
- Se `:logout` responder `Não há operador identificado por senha.`; ninguém estava com sessão aberta.

**Condição de aceite:** senha errada recusada, senha certa aceita com o nome na faixa de status, saída da sessão funcionando, e bloqueio de 30 s depois de cinco erros.

**Registro:** data e hora · placa e ID: não se aplica (o teste é da estação) · versão · resultado · medido: tempo de bloqueio depois de cinco erros (s) · inesperado.

### [ ] RT 76 - Sem login e como observador: ver sim, alterar não

Prioridade: recomendado. Precisa da rede formada e de um sensor disponível.

**Comandos necessários**

Com a estação conectada à Probe e **ninguém identificado**, pelo lançador (`k`):

```text
22 20 FF 00
22 00 FF 01
22 0 FF 1
WIRELESS BIND 1 0x<UUID>
```

Depois entre como `visita` (o observador) e repita os mesmos comandos.

**Variações possíveis**

- Como observador, tente mudar um limite de alarme na tela de um sensor (`l`).
- Como observador, tente um ensaio (`F9`).

**Saídas esperadas**

- `22 20 FF 00` passa nos dois casos: consultar é livre.
- Sem login, os outros três são recusados com o aviso `Identifique-se para alterar a rede: tecle o ou use :login.`, e em Mensagens (`F6`) fica `Bloqueado: <comando> — <motivo>`. A grafia alternativa da eleição (`22 0 FF 1`) é recusada pelo mesmo critério.
- Nada chega à Probe: nenhuma linha `[GW] Comando enviado` nem `WIRELESS_ASSOC` nova.
- Como observador: faixa `Operador visita (observador)` e recusa com `visita tem o perfil de observador e não altera a rede.`
- Cada recusa vira um registro `COMMAND_DENIED` na faixa `AUDIT`: sem autor quando ninguém estava identificado, com `visita` quando foi o observador.

**Se sair isso; faça isso**

- Se um comando que altera a rede passar sem login; a estação está com `OPERATOR_MODE=off`, ou não achou o cadastro (a faixa mostraria `(sistema)`). Se nenhum dos dois, é defeito de segurança: registre o comando exato.
- Se a consulta `22 20 FF 00` for recusada; registre: leitura não deveria exigir identificação.
- Se as recusas não aparecerem no `audit` (RT 79); a estação está com `OPERATOR_MODE=off`, o único modo que não registra ações.

**Condição de aceite:** sem login e como observador, a consulta passa e as três alterações são recusadas sem chegar à Probe; cada tentativa fica registrada.

**Registro:** data e hora · placa e ID: não se aplica (o teste é da estação) · versão · resultado · medido: não há · inesperado.

### [ ] RT 77 - Modos de identificação e cadastro inválido

Prioridade: opcional (recomendado antes de uso fora da bancada).

**Comandos necessários**

Um de cada vez, tentando em cada um a consulta `22 20 FF 00` e a eleição `22 00 FF 01`:

```bash
OPERATOR_MODE=off SECURITY_MODE=off ./Codigo/scripts/run_tui.sh <porta>
OPERATOR_MODE=required SECURITY_MODE=off ./Codigo/scripts/run_tui.sh <porta>

chmod 644 ~/.config/iot-over-can/operators.json
SECURITY_MODE=off ./Codigo/scripts/run_tui.sh <porta>
chmod 600 ~/.config/iot-over-can/operators.json
```

**Variações possíveis**

- Modo `required` sem nenhum cadastro, apontando para um arquivo que não existe: `cd Front && .venv/bin/iot-over-can-tui --port <porta> --mode auto --security-mode off --operator-mode required --operators-file /tmp/sem-cadastro.json`.
- Modo `auto` sem cadastro: o mesmo comando, com `--operator-mode auto`.

**Saídas esperadas**

| Situação | Faixa de status | Eleição `22 00 FF 01` | Registro na auditoria |
| --- | --- | --- | --- |
| `off` | sem o trecho do operador | passa | nenhum |
| `auto`, com cadastro, sem login | `Operador sem login` | recusada | `COMMAND_DENIED`, sem autor |
| `auto`, com cadastro, com login | `Operador <Seu Nome>` | passa | `NETWORK_COMMAND`, com o seu nome |
| `auto`, sem cadastro | `Operador <conta> (sistema)` | passa | `NETWORK_COMMAND`, autor `sys:<conta>` |
| `required`, sem cadastro | `Operador sem login` | recusada: `Nenhum operador cadastrado. Use iot-over-can-operators add NOME.` | `COMMAND_DENIED` |
| cadastro com permissão 644 | `Operador sem login` | recusada: `Cadastro de operadores inválido: operators.json deve usar permissões 0600` | `COMMAND_DENIED` |

A consulta `22 20 FF 00` passa em todas as linhas.

**Se sair isso; faça isso**

- Se, com o cadastro em 644, a eleição passar; defeito de segurança. Um cadastro estragado tem de bloquear, não liberar: senão bastaria danificar o arquivo para desligar o login.
- Se, no modo `off`, aparecerem registros novos no `audit`; registre: esse modo existe para ensaios automatizados e não deveria gravar ações.
- Se esquecer o `chmod 600` no fim; todos os testes seguintes vão falhar com `Cadastro de operadores inválido`.

**Condição de aceite:** cada linha da tabela se comporta como descrito, e o cadastro com permissão errada bloqueia as alterações. Termine com a permissão de volta em 600.

**Registro:** data e hora · placa e ID: não se aplica (o teste é da estação) · versão · resultado · medido: comportamento visto em cada linha da tabela · inesperado.

### [ ] RT 78 - Expiração por inatividade e revogação da sessão

Prioridade: opcional.

**Comandos necessários**

Abra a estação com expiração de 1 minuto. O `run_tui.sh` repassa `OPERATOR_IDLE` (minutos) e, depois da porta, qualquer outra opção da estação:

```bash
OPERATOR_IDLE=1 SECURITY_MODE=off ./Codigo/scripts/run_tui.sh <porta>
```

1. Entre com `o` e não toque no teclado nem no mouse por 1 minuto. Tente a eleição.
2. Entre de novo. Noutro terminal, troque a sua senha:

```bash
./Codigo/scripts/operadores.sh passwd <nome>
```

**Variações possíveis**

- Em vez de `passwd`: `disable <nome>` (e depois `enable`).
- `--operator-idle 0` desliga a expiração. O padrão é 15 minutos.

**Saídas esperadas**

- Passado 1 minuto: aviso `Sessão de <Seu Nome> encerrada por inatividade`, a faixa volta a `Operador sem login`, a eleição é recusada e a auditoria ganha um `SESSION_EXPIRED`.
- Após o `passwd`: o terminal responde `Senha de <nome> trocada. Sessões abertas dele na TUI serão encerradas.` Na estação, em até 5 s ou no próximo comando: aviso `Sessão de <Seu Nome> encerrada: acesso alterado no cadastro` e um registro `SESSION_REVOKED`.
- Após o `disable`: `Operador <nome> desativado. A sessão dele na TUI, se houver, será encerrada.`

**Se sair isso; faça isso**

- Se a sessão não expirar; algo está contando como atividade: teclado, mouse e comandos contam; a telemetria chegando não. Afaste o mouse da janela do terminal.
- Se a sessão sobreviver à troca de senha; defeito: a sessão antiga deveria cair. Anote quanto tempo esperou.
- Se o histórico desta sessão não aparecer depois no `audit`; a estação foi aberta fora de `Front/`, e o banco ficou em `logs/` da pasta em que você estava.

**Condição de aceite:** a sessão cai sozinha no tempo configurado e cai em até 5 s quando a senha é trocada ou o operador é desativado; os dois encerramentos ficam registrados. Anote os tempos medidos.

**Registro:** data e hora · placa e ID: não se aplica (o teste é da estação) · versão · resultado · medido: tempo até a sessão cair por inatividade, depois da troca de senha e depois da desativação (s) · inesperado.

### [ ] RT 79 - Trilha de auditoria: quem fez o quê

Prioridade: recomendado. É o resultado que a mudança entrega; precisa da rede formada e de um sensor disponível.

**Comandos necessários**

Identificado como operador, faça pela estação, nesta ordem: associar o sensor com reassociação automática (`F7`); pedir uma eleição (`F4`); mudar a configuração do sensor (tela do sensor, `c`); rodar um ensaio curto (`F9`); desassociar o sensor. Depois abra o Histórico (`F11` ou `h`) e, num terminal:

```bash
./Codigo/scripts/operadores.sh audit --since 24h
./Codigo/scripts/operadores.sh audit --operator <nome> --since 24h
./Codigo/scripts/operadores.sh audit --operator <nome> --json | tail -2
grep -c AUDITORIA Front/logs/session_*.jsonl
```

**Variações possíveis**

- Ações feitas sem cadastro aparecem com `--operator sys:<conta>`.
- `--since` aceita `24h`, `7d` ou uma data (`2026-11-12`); `--limit` muda o máximo de 200 registros.
- Com a estação ligada direto ao console de um sensor, envie `AUTH UNLOCK <token>` e confira como ele fica gravado.

**Saídas esperadas**

- Uma linha por ação, com instante, autor, tipo e título. Este é o formato real, de uma sessão simulada:

```text
2026-11-12 13:59:09  sys:root         SESSION_START      Estação iniciada por root@vm
2026-11-12 13:59:10  marcelo          LOGIN              Entrada de Marcelo
2026-11-12 13:59:10  marcelo          SENSOR_BIND        Entrada do sensor 0x1122334455667788 autorizada no Node 02 (reassociação automática)
2026-11-12 13:59:10  marcelo          NETWORK_COMMAND    Iniciar eleição de líder
2026-11-12 13:59:10  marcelo          SENSOR_CONFIG      Configuração alterada (sensor 02.01)
2026-11-12 13:59:10  marcelo          FAULT_COMMAND      Ensaio de falha solicitado
2026-11-12 13:59:11  visita           COMMAND_DENIED     Recusado: Sensor 0x1122334455667788 desassociado do Node 02 [DENIED]
```

- No Histórico: uma faixa `AUDIT` (símbolo ◈), cada linha começando pelo autor. Nos detalhes de uma ação: `Responsável: <Seu Nome> (<nome>), operador, identificado por senha`. Para a conta do sistema: `usuário do sistema, não verificado`.
- Nos eventos do sensor, a resposta do firmware à associação (`BOUND`, `ONLINE`) traz `Autorizado por: <nome>`. O que a rede faz sozinha (`STALE`, uma reassociação automática) fica sem autor.
- Nenhum segredo: o comando do token aparece como `AUTH UNLOCK ***`.
- O `grep` devolve um número maior que zero: as mesmas ações estão no arquivo da sessão, uma segunda cópia fora do banco.

**Se sair isso; faça isso**

- Se `Autorizado por` não aparecer na resposta `BOUND`; ela chegou mais de 2 minutos depois do comando, ou o identificador foi digitado à mão numa forma diferente da que a Probe imprime (`0x` e 16 dígitos). Pela tela `F7` a forma é sempre a certa. O registro `SENSOR_BIND` com o seu nome existe nos dois casos.
- Se a Probe responder `[GW] WIRELESS_ERROR UUID invalido`; o identificador foi digitado sem o `0x`.
- Se sair `Nenhum histórico em logs/historico.sqlite3. Rode a TUI nesta pasta ou use --db.`; a estação foi aberta de outra pasta. Informe o banco: `audit --db <caminho>/logs/historico.sqlite3`.
- Se sair `Nenhuma ação registrada com esses filtros.`; o filtro usa o nome de acesso (`marcelo`), não o nome completo (`Marcelo`).
- Se aparecer em Mensagens `Falha ao registrar a ação do operador`; o banco não pôde ser escrito (disco cheio, permissão). O comando saiu, mas ficou sem registro: anote.

**Condição de aceite:** as cinco ações aparecem na ordem em que foram feitas, com o seu nome, tanto no Histórico quanto no `audit`; a resposta do firmware à associação está creditada a você; nenhum token ou senha aparece em lugar nenhum.

**Registro:** data e hora · placa e ID: não se aplica (o teste é da estação) · versão · resultado · medido: não há · inesperado.

### [ ] RT 80 - Integridade da trilha de auditoria

Prioridade: opcional. Trabalhe sempre numa **cópia** do banco.

**Comandos necessários**

```bash
./Codigo/scripts/operadores.sh audit --verify

cp Front/logs/historico.sqlite3 /tmp/copia.sqlite3
sqlite3 /tmp/copia.sqlite3 "UPDATE events SET actor='visita' WHERE kind='SENSOR_BIND';"
./Codigo/scripts/operadores.sh audit --db /tmp/copia.sqlite3 --verify; echo "saída=$?"
```

**Variações possíveis**

- Apagar um registro do meio: `DELETE FROM events WHERE kind='NETWORK_COMMAND';`
- Apagar os **últimos** registros de auditoria, numa segunda cópia (`DELETE FROM events WHERE id > <n>;`; confira o nome da tabela com `.tables` se o comando recusar).
- Sem o programa `sqlite3`: `sudo apt install sqlite3`.

**Saídas esperadas**

- Banco original: `Cadeia de auditoria íntegra: <n> registro(s) conferido(s).`, código de saída 0.
- Cópia adulterada: `ADULTERAÇÃO: o registro <id> não confere com a cadeia (<n> anteriores íntegros).`, código de saída 2. O `<id>` é o do registro que você alterou.
- Registro do meio apagado: a adulteração é apontada no registro seguinte.
- Últimos registros apagados: a cadeia sozinha continua `íntegra`, com menos registros, mas o hash final anotado ao encerrar a sessão (em `logs/session_*.jsonl`, uma linha `audit_head`) deixa de estar na cadeia, e `audit --verify` escreve `CORTE OU SUBSTITUIÇÃO: <arquivo da sessão>: o hash final anotado (<n> registro(s), <hash>…) não está na cadeia do banco.` e sai com código 2. Sem nenhum arquivo de sessão ao lado do banco, a saída é `Sem âncoras de sessão em <pasta>: o corte dos últimos registros não pode ser verificado.`

**Se sair isso; faça isso**

- Se o banco **original** acusar adulteração; não mexa nele. Copie a pasta `Front/logs/` inteira e compare com o arquivo da sessão antes de qualquer outra coisa.
- Se um banco de sessões antigas, anterior a este commit, responder `0 registro(s) conferido(s)`; está certo: ele é migrado ao abrir e ainda não tem ações registradas.
- Se a cópia adulterada passar como íntegra; confira se o `UPDATE` alterou alguma linha (o banco precisa ter um `SENSOR_BIND`, feito no RT 79).

**Condição de aceite:** banco original íntegro e `Âncoras de sessão: <n> arquivo(s) conferido(s)`; edição e remoção no meio detectadas, com o registro apontado; o corte do fim detectado pela âncora (`CORTE OU SUBSTITUIÇÃO`). Para o corte do fim, apague os últimos registros numa **cópia** do banco e use `--db <cópia>` com `--anchors <pasta de logs>`. Anote o número de registros conferidos junto do registro da sessão.

**Registro:** data e hora · placa e ID: não se aplica (o teste é da estação) · versão · resultado · medido: número de registros conferidos pelo `audit --verify` e o registro apontado em cada adulteração · inesperado.

## Fase 9. Longa duração

É o último teste obrigatório: o sistema inteiro ligado e ninguém tocando em nada. Ele fica para o fim porque só diz alguma coisa depois que cada parte passou sozinha. Antes de começar, confira que todos os sensores estão com sessão aberta (o sensor B do RT 72 já regravado com as chaves dele) e identifique-se na estação: ao final, a auditoria mostra que ninguém mexeu na rede.

### [ ] RT 81 - Longa duração

Prioridade: recomendado. Uma hora no mínimo; uma noite (8 h) é o ideal.

**Comandos necessários**

Rede completa, todos os sensores e o painel associados, estação conectada, `METRICS ON`. Não toque em nada. Ao final, exporte (`F8`, `x`) e conte no arquivo da sessão:

```bash
cd Front/logs
grep -c 'WIRELESS_FAILOVER' gravacao_<data>_<sessão>.log
grep -c 'Lider anunciado' gravacao_<data>_<sessão>.log
grep -c 'SESSION_DOWN' gravacao_<data>_<sessão>.log
grep -c 'HANDSHAKE_TIMEOUT' gravacao_<data>_<sessão>.log
grep 'BUS_STATS' gravacao_<data>_<sessão>.log | grep -vc 'rx_err=0 tx_err=0'
cd ../.. && ./Codigo/scripts/operadores.sh audit --since 24h
```

**Variações possíveis**

- Com o roteador Wi-Fi da casa e um micro-ondas por perto, para um ambiente de 2,4 GHz mais hostil.

**Saídas esperadas**

- Zero reassociações, zero eleições depois da primeira, zero sessões de dados encerradas.
- `auth_fail=0` e `replay=0` em todas as linhas `WIRELESS_LINK`.
- `UPTIME` de cada placa igual à duração do teste (nenhum reinício): `PROBE_STATUS` na Probe e `PING` nos sensores.
- Ocupação estável, sem tendência de subida.
- Na saída do `audit`, nenhuma ação entre o início e o fim do teste: é a prova de que ninguém mexeu na rede enquanto ela rodava.
- Se havia um operador identificado, a sessão **dele** expira 15 minutos depois do último toque e deixa um registro `SESSION_EXPIRED`. É o previsto e não é um evento da rede.

**Se sair isso; faça isso**

- Se houver sessões encerradas; veja o `reason` de cada `SESSION_DOWN` e o `rssi` anterior. Poucas quedas com retorno automático são esperadas num rádio compartilhado; conte quantas por hora.
- Se alguma placa reiniciou; no monitor dela, o motivo está na linha `rst:` do boot. O firmware ainda não registra motivos de reinício (melhoria proposta).
- Se o `ajuste_ms` do relógio for grande a cada 10 min; é a deriva do cristal do ESP32. Anote o valor típico.

**Condição de aceite:** nenhum reinício, nenhuma eleição ou reassociação espontânea, contadores de erro e de segurança em zero. Guarde o arquivo da sessão e o exportado: são o registro da validação.

**Registro:** data e hora · placa e ID: a bancada inteira · versão · resultado · medido: duração do teste, contadores de erro e de segurança ao final, e nomes do arquivo da sessão e do exportado · inesperado.

## Fase 10. Opcionais, para fazer por último

Nada aqui é necessário para dar o sistema por validado. São testes que ensinam bastante sobre ele e geram dados para o trabalho.

### [ ] RT 82 - Sensor real dentro do laboratório virtual

Prioridade: opcional. Útil para depurar um sensor sem montar a bancada CAN.

**Comandos necessários**

```bash
./Codigo/scripts/build_network_sim.sh /tmp/ioc_sim_network
./Simulacao/iniciar_laboratorio.py
```

Ou peça por peça, em três terminais:

```bash
python3 Simulacao/studio/server.py --binary /tmp/ioc_sim_network --browser
Front/.venv/bin/python Simulacao/usb_sensor_bridge.py --port /dev/ttyACM0 --node 1 --rssi -55
python3 Simulacao/sim_ctl.py PROBE WIRELESS BIND 1 0x<UUID> AUTO
```

**Variações possíveis**

- `--passive` se o sensor já envia telemetria; `--token-file <arquivo>` com o token de manutenção.
- Painel real: `Simulacao/usb_panel_bridge.py`.
- Topologia em funcionamento: `sim_ctl.py STATUS`, `PAUSE`, `RESUME`, `NODE 4 ADD`.
- Opções do lançador: `status`, `stop`, `--rebuild`, `--no-usb`, `--no-browser`, `--dry-run`, `--device /dev/serial/by-id/...`.

**Saídas esperadas**

- O Studio abre em `http://127.0.0.1:8765` e informa a porta virtual (`PORT=/dev/pts/N`).
- O sensor físico aparece como filho do módulo virtual 1, com telemetria real; o espectro do sensor acústico também passa pela ponte.

**Se sair isso; faça isso**

- Se a ponte não conseguir comandar o sensor; ela não contorna a autorização do firmware. Use `--token-file` ou `--passive`.
- Se a FFT nunca chegar pela ponte; foi um defeito já corrigido (CH-070: o eco do console era tomado por resposta). Recompile o simulador.
- Não exponha o servidor do Studio na rede: ele só aceita conexões da própria máquina e usa token de sessão.

**Condição de aceite:** o sensor físico entrega telemetria numa rede virtual e responde a um comando enviado pela estação.

**Registro:** data e hora · placa e ID: sensor físico usado (UUID) · versão · resultado · medido: não há · inesperado.

### [ ] RT 83 - Instrumentação de pesquisa: CONFIG, BENCH e ECHO

Prioridade: opcional. Gera os números de desempenho da rede física.

**Comandos necessários**

Na Probe (lançador `k`) e no console de um sensor:

```text
CONFIG
BENCH CRYPTO 100
ECHO 1.1 40 500 LOCAL
ECHO 1.1 40 500 SENSOR
```

E a campanha automatizada:

```bash
cd Pesquisa
python3 -m iocpesquisa planejar desenhos/bancada_fisica.json
../Front/.venv/bin/python -m iocpesquisa coletar desenhos/bancada_fisica.json --porta <porta da Probe>
python3 -m iocpesquisa bench --porta <porta da Probe> --placa probe --saida resultados/bancada_fisica
python3 -m iocpesquisa bench --porta /dev/ttyACM0 --placa sensor --saida resultados/bancada_fisica
python3 -m iocpesquisa analisar resultados/bancada_fisica
```

**Variações possíveis**

- `ECHO <módulo>.<filho> <n> <período_ms> [LOCAL|SENSOR] [semente]`: `LOCAL` mede até o módulo; `SENSOR` vai até o sensor e volta.
- `--limite N` faz no máximo N execuções por chamada; a campanha continua de onde parou.
- Na estação, a tela `/` (medições de pesquisa) mostra os resultados.

**Saídas esperadas**

- `[GW] CONFIG node=0 bitrate=500000 hb_period_ms=... assoc_stale_ms=... assoc_lost_ms=...`: os parâmetros com que o firmware foi compilado.
- `[GW] BENCH_DONE ops=<n> cpu_mhz=240 t_us=...` na Probe; `BENCH_DONE ops=<n> cpu_mhz=125` no Pico W (150 no Pico 2 W).
- Resultados do eco e `relatorio/relatorio.md` gerado pela análise.

**Se sair isso; faça isso**

- Se o `BENCH CRYPTO` no sensor pausar a telemetria; é o previsto: ele pausa a aquisição enquanto mede.
- Se a coleta parar no meio; chame `coletar` de novo. Os sensores precisam estar associados antes.

**Condição de aceite:** linha `CONFIG` copiada para o caderno (é a prova de quais parâmetros estavam em vigor na bancada), custo da criptografia medido em cada placa e pelo menos um trem de eco `LOCAL` e um `SENSOR`.

**Registro:** data e hora · placa e ID: Probe 00 e cada placa medida · versão · resultado · medido: linha `CONFIG` inteira, custo da criptografia em cada placa e resultado dos trens de eco `LOCAL` e `SENSOR` · inesperado.

### [ ] RT 84 - Alcance e convivência de rádio

Prioridade: opcional. Quantifica o que a bancada virtual não reproduz.

**Comandos necessários**

Com um sensor num power bank e a telemetria fluindo, afaste-o do módulo responsável (política manual, para não migrar) e anote, a cada distância, o `rssi`, o `lost` e o `rtt_ms` de `WIRELESS_LINK`.

**Variações possíveis**

- Distâncias: 1, 3, 5, 10 m, e atrás de uma parede.
- Canal Wi-Fi: o ponto de acesso do módulo N usa o canal `{1, 6, 11}[N mod 3]`: módulos 3, 6, 9 no canal 1; módulos 1, 4, 7 no canal 6; módulos 2, 5, 8 no canal 11. Compare dois sensores em módulos de **mesmo** canal (1 e 4) com dois em canais diferentes (1 e 2).
- Repita o RT 55 a 5 m: tempo de abertura da sessão com sinal fraco.
- Roteador doméstico no mesmo canal, transmitindo um vídeo.

**Saídas esperadas**

- RSSI caindo com a distância; `lost` e `rtt_max_ms` subindo antes de a sessão cair.
- A reassociação automática só escolhe um destino que ouve o sensor com −90 dBm ou mais: abaixo disso o sensor fica órfão mesmo em `AUTO`.

**Se sair isso; faça isso**

- Se a sessão cair a poucos metros; a antena do ESP32 ou do Pico W está obstruída (protoboard metálica, fios por cima da antena).
- Se dois módulos no mesmo canal tiverem desempenho pior; foi uma das suspeitas da bancada de 08/10 (módulos 1 e 4 no canal 6). Anote a diferença.

**Condição de aceite:** tabela distância × RSSI × perdas × ida e volta preenchida, e a distância máxima com sessão estável conhecida para o seu ambiente.

**Registro:** data e hora · placa e ID: sensor (UUID) e módulo responsável · versão · resultado · medido: tabela distância × RSSI × perdas × ida e volta, e a distância máxima com sessão estável · inesperado.

### [ ] RT 85 - Calibração acústica e comparação com referência

Prioridade: opcional. Exige um calibrador acústico de 94 dB a 1 kHz; sem ele, não altere o ganho.

**Comandos necessários**

Com o calibrador encaixado no microfone por pelo menos 3 s:

```text
AUTH UNLOCK <token>
CAL 94
```

**Variações possíveis**

- Pela rede: ação "Calibrar com calibrador acústico" na tela do sensor; a estação calcula o ganho com as leituras recebidas e o envia numa configuração comum.
- Nas duas placas (Pico W e Pico 2 W).
- Com um medidor de nível sonoro de referência: compare `LAEQ_DB` e as doses em três níveis.
- Tacômetro acústico contra um tacômetro óptico num motor.

**Saídas esperadas**

- `OK CAL MEASURED_DB=<n> REFERENCE_DB=94.00 OFFSET_DB=<±n> GAIN=<n>` e depois `CONFIG_APPLIED ... GAIN=<n>`.
- Com o calibrador, depois do ajuste: `LZEQ_DB`, `LAEQ_DB` e `LCEQ_DB` em 94,0 (a 1 kHz as três ponderações coincidem).
- O deslocamento esperado é de até ±1 dB (tolerância do INMP441).

**Se sair isso; faça isso**

- Se sair `ERR CAL_UNSTABLE SPREAD_DB=<n>`; as três últimas leituras variaram mais de 0,5 dB. O calibrador está mal encaixado ou há ruído externo.
- Se o deslocamento passar de 3 dB; não é tolerância do microfone: confira o encaixe, o furo do microfone e se o ganho já não estava alterado.
- Se os níveis A e C não coincidirem a 1 kHz; a FFT não está cobrindo os quadros (`COVERAGE_PCT`).

**Condição de aceite:** leitura de 94,0 ± 0,3 dB com o calibrador depois do ajuste, e o ganho final anotado por sensor. As leituras continuam indicativas: o INMP441 não é um medidor classe 1 ou 2.

**Registro:** data e hora · placa e ID: cada sensor acústico (UUID) · versão · resultado · medido: leitura com o calibrador depois do ajuste (dB) e ganho final · inesperado.

## Fase 11. Recursos acrescentados depois do commit `b8fcbab`

Os RT desta fase cobrem o que o repositório ganhou depois do primeiro roteiro. A bancada virtual e os testes do computador conferem a lógica de cada recurso; o que falta é vê-los numa placa, e cada RT diz o limite que só o hardware mostra. Nenhum deles é condição para dar as fases 0 a 9 por validadas. O RT 71 (isolamento) e o RT 49 (coleta de exemplos sonoros) já trazem as novidades no próprio texto.

### [ ] RT 86 - Autoteste da placa (`SELFTEST`)

Prioridade: recomendado. Repita para cada placa ESP32, módulos e Probe 00, **com a placa fora da rede**.

**Comandos necessários**

Deixe a placa sem quadros no barramento (as outras placas desligadas ou o CAN_H dela solto), reinicie-a e, no monitor serial, logo depois de `[INIT] Pronto`:

```text
SELFTEST
SELFTEST CAN
SELFTEST LEDS
```

Com a porta livre, o mesmo pelo script de fumaça: `python3 Codigo/scripts/smoke_boot.py modulo --porta <porta> --no <n> --selftest` (`probe` no lugar de `modulo` para a Probe 00).

**Variações possíveis**

- `SELFTEST` sozinho roda as duas partes; `SELFTEST CAN` só o controlador; `SELFTEST LEDS` só a sequência dos LEDs.
- Com a placa já dentro da rede o comando é recusado de propósito (`RECUSADO ESTADO=...`): o teste tira o MCP2515 do barramento por instantes, e um módulo ativo não pode ficar mudo. Reinicie com o cabo solto e repita.

**Saídas esperadas**

- `[NODE <n>] [SELFTEST] INICIO PARTES=CAN,LEDS ESTADO=IDLE`.
- `CHECK=CAN_ERRORS RESULT=PASS REASON=OK REC=0 TEC=0 EFLG=0x00`: nenhum erro acumulado.
- `CHECK=CAN_LOOPBACK RESULT=PASS REASON=OK FRAMES=4/4`: quatro quadros saíram pelo MCP2515, voltaram pelo laço interno e chegaram iguais.
- `CHECK=LEDS RESULT=VISUAL SEQUENCE=LEADER,STATUS,FAULT,ALL`, com os LEDs acendendo um por vez (líder, estado, falha) e depois os três juntos.
- `[NODE <n>] [SELFTEST] RESULT=PASS CHECKS=2 WARN=0 FAIL=0 DURATION_MS=<n>`. Os LEDs só se confirmam a olho e não entram na contagem.

**Se sair isso; faça isso**

- Se sair `REASON=MODO_LACO`; o controlador não entrou no modo de laço interno: confira os 5 V e o cristal do módulo MCP2515. É o defeito que o RT 14 mostra como `Falha ao iniciar CAN`.
- Se sair `REASON=SEM_QUADRO`; o controlador respondeu pelo SPI mas nenhum quadro voltou: confira o fio INT.
- Se sair `REASON=ID`, `TAMANHO` ou `DADOS`; o quadro voltou diferente do enviado: confira SCK, MOSI e MISO (fios curtos, contato, alimentação do módulo).
- Se sair `CHECK=CAN_RESTORE RESULT=FAIL`; a placa ficou fora do modo normal: reinicie antes de usar o barramento.
- Se `CAN_ERRORS` sair `WARN` ou `FAIL` (`CONTADOR_ALTO`, `ERRO_PASSIVO`, `BUS_OFF`); um nó sozinho não recebe confirmação dos quadros que transmite, e os contadores sobem. Reinicie e rode o `SELFTEST` na hora, antes de a placa transmitir. Se persistir com o cabo solto, o defeito é da placa; só com o cabo ligado, é do barramento: volte ao RT 20.
- Se um LED não acender na sequência; confira o resistor e a polaridade (nível baixo acende) e o RT 30.

**Condição de aceite:** `CAN_ERRORS` e `CAN_LOOPBACK` em `PASS` (`FRAMES=4/4`) e os três LEDs na sequência, em todas as placas. Uma placa que reprova aqui tem defeito de placa ou de ligação ao MCP2515, não de barramento.

**Registro:** data e hora · placa e ID: cada placa · versão · resultado · medido: `FRAMES=` e `DURATION_MS=` de cada placa · inesperado.

### [ ] RT 87 - Alimentação do módulo: reinícios, tensão de entrada e corrente (`ENERGIA`)

Prioridade: recomendado se houve reinício inesperado (RT 15, RT 71) ou se a bancada usa fonte e cabos longos; opcional nos demais casos. A medição só informa: não age sobre nada.

**Comandos necessários**

1. O motivo e a contagem dos reinícios existem em todo módulo, sem montagem nenhuma:

```text
ENERGIA
ENERGIA LIMPAR
```

2. Para medir a tensão de entrada, ligue um divisor resistivo (R1 da entrada ao pino, R2 do pino ao GND, 100 kΩ cada) a um pino do ADC1 (GPIO 32 a 39; o ADC2 não serve com o Wi-Fi ligado). Para a corrente, um INA219 em série com a alimentação, no I²C (SDA no GPIO 21, SCL no GPIO 22). A ligação está no [Guia de uso](../guia-de-uso.md). Grave com as opções:

```bash
PLATFORMIO_BUILD_FLAGS="-D IOT_VIN_ADC_PIN=34 -D IOT_INA219_ADDR=64" ./Codigo/scripts/upload_esp32_can_node.sh 1 <porta>
```

3. Repita `ENERGIA` com o módulo em repouso, com o ponto de acesso ligado e com um sensor transmitindo, e compare a tensão com a de um multímetro na entrada da placa.

**Variações possíveis**

- Outro divisor: `-D IOT_VIN_R1_OHM=<Ω> -D IOT_VIN_R2_OHM=<Ω>`. Outros limites: `-D IOT_VIN_WARN_MV=<mV>` e `-D IOT_VIN_CRIT_MV=<mV>` (padrões 4750 e 4500).
- Resistor de derivação do INA219 diferente de 0,1 Ω: `-D IOT_INA219_SHUNT_MOHM=<mΩ>`.
- Potência do Wi-Fi: o padrão é 11 dBm (`IOT_WIFI_TX_POWER_QDBM=44`); `-D IOT_WIFI_TX_POWER_QDBM=78` dá o alcance máximo.

**Saídas esperadas**

- `[NODE <n>] [ENERGIA] reinicio=<motivo> quedas_de_tensao=<n> watchdog=<n> panico=<n> wifi_tx_max_dbm=11.00`. O motivo é `ENERGIA_LIGADA` depois de ligar a placa, `RESET_EXTERNO` depois de apertar EN, e `QUEDA_DE_TENSAO`, `WATCHDOG...` ou `FALHA_DO_PROGRAMA` nos reinícios anormais, que também são contados e gravados.
- Com o divisor: `vin_mv=<mV> vin_min_mv=<mV> vin_max_mv=<mV> vin=OK` (ou `BAIXA`, `CRITICA`; `vin=SEM_LEITURA` nos primeiros 160 ms).
- Com o INA219: `corrente_ma=<mA> corrente_max_ma=<mA>` (ou `corrente=SEM_RESPOSTA` se o chip não respondeu no I²C, `corrente=SEM_LEITURA` antes da primeira amostra).
- Tensão abaixo do limite de aviso: a linha `[ENERGIA] ATENCAO: tensao de entrada BAIXA, <mV> mV ...`, no máximo a cada 10 s.

**Se sair isso; faça isso**

- Se `reinicio=QUEDA_DE_TENSAO` e `quedas_de_tensao` subir; a tensão caiu abaixo do limite do ESP32: troque a fonte e os cabos, encurte a alimentação, ponha um capacitor perto da placa. Anote `vin_min_mv` quando a medição estiver ligada.
- Se a leitura de tensão divergir do multímetro por mais de cerca de 100 mV; recalcule o divisor (R1 e R2 reais) ou use o valor do multímetro como referência: o conversor do ESP32 não é preciso.
- Se `corrente=SEM_RESPOSTA`; confira SDA, SCL, VCC, GND, o endereço do INA219 (`0x40` a `0x4F`; os pinos A0 e A1 mudam o endereço) e se o resistor de derivação está em série com a alimentação.
- Se `vin_min_mv` ficar bem abaixo de `vin_mv`; houve afundamento. Mas a medição é uma média de 16 amostras a cada 10 ms: um pico de microssegundos **não aparece**; só o patamar. Para o pico use um osciloscópio.

**Condição de aceite:** `ENERGIA` responde em todos os módulos e o motivo bate com o que você fez (ligar, apertar EN). Com a medição ligada, a tensão dentro de cerca de 100 mV do multímetro, e a corrente em repouso e em carga anotadas. Nenhum `QUEDA_DE_TENSAO` ao longo da bancada.

**Registro:** data e hora · placa e ID: cada módulo medido · versão e opções de compilação · resultado · medido: `vin_mv`, `vin_min_mv`, `corrente_ma` e `corrente_max_ma` em repouso e em carga, mais a contagem de reinícios · inesperado.

### [ ] RT 88 - Autenticação de origem dos comandos CAN (opcional)

Prioridade: opcional. Só vale a pena se a bancada conviver com equipamentos que você não controla. Cobre os comandos `0x304` (comando a um sensor) e `0x305` (comando de ensaio) da Probe 00; o resto do barramento continua sem autenticação.

**Comandos necessários**

1. Grave a Probe 00 e **todos** os módulos com a opção, do mesmo commit, e só então ligue a rede (misturar placas com e sem a opção não funciona):

```bash
./Codigo/scripts/upload_esp32_can_node.sh 0 <porta> --auth-can
./Codigo/scripts/upload_esp32_can_node.sh 1 <porta> --auth-can
./Codigo/scripts/upload_esp32_can_node.sh 2 <porta> --auth-can
```

2. Com um sensor associado (RT 55), pela estação ou pela serial da Probe:

```text
CMD TARGET=01.01 ACTION=TELEMETRY TX=B2 STATE=ONCE
FAULT DROP_SESSION <ID do módulo com sensor>
```

3. Veja o estado, na serial da Probe e na de cada módulo:

```text
CANAUTH
```

4. Controle negativo: grave uma placa extra como Probe (ID 0) **sem** `--auth-can`, com a mesma chave, ligue-a no lugar da Probe autêntica e repita o `FAULT DROP_SESSION`.

**Variações possíveis**

- Trocar a Probe por outra placa (o contador dela recomeça do zero): `CANAUTH SYNC 0` na serial de cada módulo.
- Sem a opção, nada muda: todos os RT anteriores valem como estão.

**Saídas esperadas**

- Na partida: `[NODE 0] [CANAUTH] LIGADO papel=PROBE atualidade=<n>` e `[NODE <n>] [CANAUTH] LIGADO papel=MODULO atualidade=<n>`.
- Os comandos do passo 2 funcionam como no RT 57 e no RT 69. `CANAUTH` na Probe: `estado=LIGADO papel=PROBE ... etiquetas=<n> perdidas=0`; no módulo: `estado=LIGADO papel=MODULO ... aceitos=<n> invalidos=0 sem_etiqueta=0 orfas=0`, com `aceitos` subindo a cada comando.
- Controle negativo: o módulo escreve `[NODE <n>] [CANAUTH] descartado id=0x305 motivo=SEM_ETIQUETA ...`, o `sem_etiqueta` sobe e a sessão **não** cai.

**Se sair isso; faça isso**

- Se sair `[CANAUTH] ERRO: chave do enlace invalida`; a placa foi gravada sem a chave mestra: RT 4, e grave de novo (o script já recusa `--auth-can` sem chave).
- Se os comandos legítimos forem descartados (`SEM_ETIQUETA`) com tudo autêntico; alguma placa foi gravada sem `--auth-can`. Regrave todas com a mesma opção e o mesmo commit.
- Se `perdidas` na Probe for maior que zero; a fila do MCP2515 estava cheia na hora de enviar a etiqueta, e os módulos descartaram o comando. Repita fora do ensaio de carga (RT 70).
- Se, depois de trocar a Probe, os comandos forem descartados com `ETIQUETA_INVALIDA`; faça `CANAUTH SYNC 0` em cada módulo.
- Se um módulo recém-ligado recusar tudo depois de a Probe ter mandado mais de 65 535 comandos; `CANAUTH SYNC <valor de atualidade mostrado pela Probe>`.

**Condição de aceite:** os comandos `0x304` e `0x305` legítimos funcionam e são contados em `aceitos`; o comando sem etiqueta é descartado e contado em `sem_etiqueta`, sem efeito. Anote os limites que continuam valendo: a eleição, a presença, a hora, a associação e os dados do sensor **não** são autenticados, e a chave é de grupo (prova que o emissor tem a chave, não qual módulo é). Detalhes em [Segurança](../seguranca.md).

**Registro:** data e hora · placa e ID: Probe, cada módulo e a placa do controle negativo · versão (com `--auth-can`) · resultado · medido: `atualidade`, `aceitos` e `sem_etiqueta` ao fim · inesperado.

### [ ] RT 89 - Medindo as esperas do sensor para decidir se mudam

Prioridade: recomendado, mas só para quem quer encurtar a reassociação. Sem esta medição, **mantenha os padrões** (10 s para largar a sessão, 15 s para seguir a oferta de outro módulo): eles são os que foram validados.

**Comandos necessários**

1. Monte o cenário do RT 61: dois módulos que ouvem o sensor, o sensor com sessão `SECURE` no módulo 1, política `AUTO`, a estação ligada na Probe gravando a sessão (`--record-session`, no `logs/gravacao_*.log`).
2. Tire a alimentação do módulo 1 e deixe a rede reassociar o sensor. Religue o módulo 1 depois que a telemetria voltar.
3. Compare a gravação com a bancada virtual:

```bash
python3 Simulacao/comparar_bancada.py logs/gravacao_<data>_<sessão>.log --cenario owner_failover
```

4. Repita pelo menos 5 vezes e anote a mediana e o pior caso de `reassociacao_ms` (da decisão `WIRELESS_FAILOVER` até o sensor voltar a ter sessão `SECURE`).
5. Só se a mediana ficar dentro da tolerância do comparador (1500 ms ou 25 % do valor da bancada, o que for maior) o modelo descreve a sua bancada, e a tabela abaixo vale como projeção. Então, se ainda quiser encurtar, compile **todos** os sensores com os mesmos valores e repita os passos 1 a 4:

```bash
./Codigo/scripts/build_pico.sh pico_w --espera-sessao 7000 --espera-oferta 5000
```

**Variações possíveis**

- O sensor acústico aceita as mesmas opções: `build_pico.sh pico_w --audio --espera-sessao ... --espera-oferta ...`. A saída fica numa pasta própria (`build/pico_w-sess7000-oferta5000/`), sem apagar o `.uf2` padrão.
- As esperas valem de 7000 a 60000 ms (sessão; precisa passar de dois `CMD_PING` perdidos, um a cada 3 s) e de 1000 a 120000 ms (oferta). Valores fora disso fazem a compilação falhar.

**Saídas esperadas**

Projeção da bancada virtual (cenário `owner_failover`; da falha do módulo até o sensor voltar a ter sessão, somando os ≈ 5,2 s que o líder leva para decidir):

| Sessão / oferta | decisão → sessão (`reassociacao_ms`) | falha → sessão |
| --- | --- | --- |
| 10 s / 15 s (padrão) | 19,1 s | 24,3 s |
| 10 s / 5 s | 9,2 s | 14,4 s |
| 7 s / 15 s | 16,1 s | 21,3 s |
| 7 s / 5 s | 6,2 s | 11,4 s |
| 7 s / 3 s | 4,1 s | 9,3 s |
| 7 s / 1 s | 2,3 s | 7,5 s |

A bancada virtual só prevê o ganho; o que a medição física decide é se o ganho vale o risco. A espera da oferta existe para limitar o efeito de uma oferta antiga repetida: com ela curta demais, o sensor pode alternar entre módulos.

**Se sair isso; faça isso**

- Se a mediana física ficar fora da tolerância; o modelo da bancada virtual não descreve a sua montagem. Não mude as esperas: registre os tempos medidos (falha → decisão, decisão → sessão) e abra um `CH-` em [Desafios](../desafios.md).
- Se, com as esperas novas, o sensor alternar entre módulos sem falha, ou largar a sessão com a rede saudável (observe o RT 56 por 10 minutos); volte aos padrões.
- Se o ganho medido for muito menor que o da tabela; algo além das esperas domina (a associação ao ponto de acesso, o rádio): anote a etapa mais lenta e deixe os padrões.

**Condição de aceite:** a tabela das medidas (cada repetição, a mediana e o pior caso) anotada no registro da sessão, com a decisão: manter os padrões ou adotar valores novos, e quais. Mudar o padrão no repositório exige essa tabela, não só a projeção.

**Registro:** data e hora · placa e ID: sensor (UUID), módulos e o `.uf2` usado (pasta de saída) · versão · resultado · medido: `reassociacao_ms` de cada repetição, mediana e pior caso (s) · inesperado.

## Referência rápida

Tabelas para consultar durante os testes. Todas as seriais operam a 115200 baud.

### Comandos de texto da Probe 00

| Comando | Efeito | Autorização na estação |
| --- | --- | --- |
| `PROBE_VERSION` ou `VERSION` | versão, protocolo e `FEATURES` | livre |
| `PROBE_STATUS` ou `STATUS` | estado do CAN e tempo ligado | livre |
| `TIME` | mostra a hora da rede | livre |
| `TIME <ms UTC desde 1970>` | ajusta a hora (fonte terminal) e divulga | livre |
| `METRICS ON\|OFF\|RESET` | medição do barramento | livre |
| `WIRELESS BIND <módulo> <uuid> [AUTO]` | associa um sensor | exige |
| `WIRELESS UNBIND <módulo> <uuid>` | desassocia | exige |
| `WIRELESS POLICY <módulo> <uuid> AUTO\|MANUAL` | política de reassociação | exige |
| `CMD TARGET=NN.CC ACTION=... TX=<id>` | comando a um sensor | exige |
| `FAULT POWER_CYCLE\|SILENCE\|AP_OUTAGE <módulo> <s>` | ensaios de falha | exige |
| `FAULT DROP_SESSION <módulo>` | descarta as sessões | exige |
| `FAULT LOAD <pct> <s> [HIGH\|MID\|LOW\|MIX] [semente]` | carga gerada pela Probe | exige |
| `FAULT CANCEL [módulo]` | encerra o ensaio | exige |
| `ECHO <módulo>.<filho> <n> <período_ms> [LOCAL\|SENSOR] [semente]` | ida e volta pelo canal de dados | confira na estação |
| `CONFIG` | parâmetros da compilação | confira na estação |
| `BENCH CRYPTO [n]` | custo da criptografia na placa | confira na estação |

### Comandos do monitor serial de um módulo CAN (e da Probe 00, quando dito)

| Comando | Efeito |
| --- | --- |
| `VERSION` | versão, commit, protocolo e `FEATURES` do módulo |
| `SELFTEST [CAN\|LEDS]` | autoteste do controlador e dos LEDs; só fora da rede (também na Probe 00; RT 86) |
| `ENERGIA` / `ENERGIA LIMPAR` | motivo do último reinício, contagem dos anormais e, se ligadas, tensão e corrente; zera a contagem (RT 87) |
| `ISOLAMENTO` | estado do detector de isolamento do barramento (RT 71) |
| `CANAUTH` / `CANAUTH SYNC <n>` | estado da autenticação dos comandos `0x304` e `0x305`; acerta o contador de um módulo (só com `--auth-can`; RT 88) |

### Comandos CAN de quatro bytes (`opcode subcomando alvo ação`, em hexadecimal)

| Comando | Efeito |
| --- | --- |
| `22 00 FF 01` | iniciar eleição |
| `22 10 ID 00` | desativar a função local do módulo |
| `22 10 ID 11` | reativar a função local |
| `22 10 ID 33` | confirmar falha (só o líder pode emitir) |
| `22 10 ID 44` | limpar falha |
| `22 20 FF 00` | consultar o estado de todos |
| `22 20 ID 00` | consultar o estado de um módulo |
| `22 30 FF 01` a `05` | período do sinal de presença: 2000, 1500, 1000, 750, 500 ms |

Estados na resposta `23 21 ID estado`: `00` líder, `01` seguidor, `02` desativado, `03` em falha, `04` Probe, `05` entrando, `06` conflito de líder.

### Ações de `CMD` para um sensor

| Ação | Parâmetros | Perfil |
| --- | --- | --- |
| `TELEMETRY` | `STATE=ON\|OFF\|ONCE\|FAST\|SLOW` | todos |
| `TELEMETRY_PERIOD` | `PERIOD_MS=200..10000` | todos |
| `FFT` | `BINS=16..256` | todos |
| `CONFIG` | `MODE= RATE_HZ= WINDOW= WINDOW_SIZE= STALTA= GAIN=` | todos |
| `STATUS` | nenhum | todos |
| `DTC_CLEAR` | nenhum | todos |
| `MEASURE_RESET` | nenhum | acústico |
| `BASELINE_LEARN` | `SECONDS=5..3600` | acústico |
| `BASELINE_CLEAR` | nenhum | acústico |

Confirmações: `ACCEPTED`, `APPLIED`, `REJECTED`, `NO_LINK`, `UNKNOWN`. Motivos de recusa: `SENSOR_TIMEOUT`, `CONFIG_UNKNOWN`, `PROFILE_MISMATCH`, `UNSUPPORTED_VIA_NODE`.

### Console USB dos sensores (prompt `EDGE>`)

| Comando | Vibração | Acústico | Pede `AUTH UNLOCK` |
| --- | --- | --- | --- |
| `HELP`, `MENU`, `PING`, `VERSION`, `STATUS`, `GET` | sim | sim | não |
| `AUTH STATUS\|UNLOCK <token>\|LOCK` | sim | sim | não |
| `NET`, `NET WIFI STATUS` | sim | sim | não |
| `DTC` | sim | sim | não |
| `BENCH CRYPTO [n]` | sim | confira no `HELP` | não |
| `SET MODE` | `IDLE\|ROTATING\|STRUCTURAL\|SEISMIC` | `IDLE\|LEVEL\|MACHINE\|EVENTS` | sim |
| `SET RATE` | 4 a 1000 Hz | 8000, 16000, 24000, 32000, 48000 | sim |
| `SET WINDOW` | `RECT\|HANN\|HAMMING\|FLATTOP\|BLACKMAN` | os mesmos | sim |
| `SET WINDOW_SIZE` | 128, 256, 512 | 256, 512, 1024, 2048 (`SET FFT`) | sim |
| `SET STALTA` | maior que 1 | `SET THRESHOLD`, de 1 a 1000 | sim |
| `SET GAIN` | maior que 0 | de 0,01 a 100 | sim |
| `APPLY` | sim | sim | sim |
| `TELEMETRY ON\|OFF\|ONCE\|FAST\|SLOW\|PERIOD <ms>` | sim | sim | sim |
| `FFT ONCE` | sim | sim | sim |
| `DTC CLEAR` | sim | sim | sim |
| `ACQ POLLING` | sim | não | sim |
| `CAL <dB>` | não | sim | sim |
| `MEASURE RESET` | não | sim | sim |
| `BASELINE LEARN <5..3600>`, `BASELINE CLEAR` | não | sim | sim |
| `NET WIFI ON\|OFF\|PROVISION <ssid> <senha>\|CLEAR` | sim | sim | sim |
| `RESET` e `RESET CONFIRM` (em 10 s) | sim | sim | sim |

Campos de `NET`: `STATE=DISABLED|DISCOVERY|BOUND`, `LINK_KEYS=DEVICE|MASTER|NONE|UUID_MISMATCH`, `PARENT_NODE`, `CHILD`, `AUTH_REJECT`, `REPLAY_REJECT`.

### Operadores e auditoria (estação)

Todos os comandos abaixo começam por `./Codigo/scripts/operadores.sh` (ou `iot-over-can-operators`, dentro de `Front/`).

| Subcomando | Efeito |
| --- | --- |
| `add <nome> [--display "Nome"] [--role operator\|observer]` | cadastra; a senha é pedida no terminal, sem eco |
| `list` | lista os operadores, o perfil e o estado |
| `passwd <nome>` | troca a senha e encerra as sessões abertas dele |
| `disable <nome>`, `enable <nome>` | desativa sem apagar; reativa |
| `role <nome> operator\|observer` | troca o perfil |
| `remove <nome> [--yes]` | remove do cadastro; o histórico dele permanece |
| `check` | confere formato e permissões do cadastro |
| `audit [--operator N] [--since 24h\|7d\|data] [--limit N] [--json] [--db ARQ]` | lista as ações registradas |
| `audit --verify` | confere a cadeia de hashes; código de saída 2 se houver adulteração |

| Na estação | Efeito |
| --- | --- |
| tecla `o` | janela do operador: entrar, sair da sessão ou ver quem está identificado |
| `:login`, `:logout`, `:operador` | o mesmo, pelo campo de comando manual (`F4`, `a`) |
| `OPERATOR_MODE=auto\|required\|off` antes de `run_tui.sh` | modo de identificação (`--operator-mode` no programa) |
| `OPERATOR_IDLE=<minutos>` antes de `run_tui.sh` | inatividade até a sessão expirar: padrão 15, `0` desliga (`--operator-idle` no programa) |
| `OPERATORS_FILE=<arquivo>` antes de `run_tui.sh` | outro arquivo de cadastro (`--operators-file` no programa) |

| Faixa de status | Significa |
| --- | --- |
| `Operador <conta> (sistema)` | sem cadastro: responde a conta do Linux, sem verificação |
| `Operador sem login` | há cadastro e ninguém entrou; alterações são recusadas |
| `Operador <Nome>` | operador identificado por senha |
| `Operador <nome> (observador)` | identificado, mas só acompanha |

| Registro na faixa `AUDIT` | Quando |
| --- | --- |
| `SESSION_START` | a estação é aberta |
| `LOGIN`, `LOGIN_FAILED`, `LOGOUT` | entrada, tentativa recusada e saída |
| `SESSION_EXPIRED`, `SESSION_REVOKED` | inatividade; senha trocada ou operador desativado |
| `SENSOR_BIND`, `SENSOR_UNBIND`, `SENSOR_POLICY` | entrada, remoção e política de reassociação de um sensor |
| `SENSOR_CONFIG`, `SENSOR_COMMAND`, `NETWORK_COMMAND`, `FAULT_COMMAND`, `DEVICE_UNLOCK`, `WIFI_PROVISION` | os demais comandos que alteram a rede ou um sensor |
| `ALARM_LIMITS` | mudança dos limites de alarme de um sensor |
| `COMMAND_DENIED`, `COMMAND_FAILED` | comando recusado, ou que não chegou a sair |

Arquivos: `~/.config/iot-over-can/operators.json` (cadastro, permissão 600, só o hash das senhas); `Front/logs/historico.sqlite3` (histórico, com a faixa `AUDIT`); `Front/logs/session_<id>.jsonl` (a segunda cópia, fonte `AUDITORIA`).

Prazos: sessão expira em 15 min sem teclado, mouse ou comando; 5 senhas erradas bloqueiam por 30 s, dobrando a cada bloqueio até 15 min; a resposta do firmware a uma associação é creditada a quem pediu por até 2 min; senha trocada ou operador desativado encerra a sessão em até 5 s.

### Etiquetas das linhas, para filtrar o registro

| Etiqueta | Onde aparece | Assunto |
| --- | --- | --- |
| `[GW]` | Probe 00 | fatos da rede observados pela Probe |
| `TEL`, `FRAG`, `DTC`, `ACK`, `PANEL` | Probe 00 | dados remontados do barramento |
| `[NODE n] [WLINK]` | monitor do módulo | canal de dados: ponto de acesso, oferta, estação, HELLO, sessão |
| `[ELEICAO]`, `[LIDER]`, `[JOIN]`, `[CONFLITO]` | monitor do módulo | formação da rede |
| `[OMISSAO]`, `[VIGILANCIA]`, `[LACO]` | monitor do módulo | vigilâncias |
| `[CTRL]`, `[CTRL RX]`, `[CTRL TX]` | Probe e módulos | comandos de controle |
| `[RELOGIO]` | monitor do módulo | relógio da rede |
| `[BOOT]` | monitor do módulo e da Probe | versão e commit gravados |
| `[SELFTEST]`, `[ENERGIA]`, `[ISOLADO]`, `[CANAUTH]` | monitor do módulo | autoteste, alimentação, isolamento do barramento, autenticação dos comandos |
| `[NET]` | console do sensor | passos até a sessão |
| `[MIC]` | console do sensor acústico | canal e falha do microfone |
| `#IOC` | serial do painel | estado do painel |
| `#LEITOR` | serial do leitor | estado do leitor |

### Identificadores CAN (menor número, maior prioridade)

| Identificador | Quem transmite | Conteúdo |
| --- | --- | --- |
| `0x010` | Probe 00 | carga de ensaio, prioridade alta |
| `0x050` | candidatos | eleição |
| `0x060` | líder eleito | anúncio de líder |
| `0x080` | Probe e módulos | comandos de controle e estado |
| `0x100` | líder | sinal de presença |
| `0x120 + N` | Probe e módulo N | hora e pedido de hora |
| `0x1F0` | Probe 00 | carga de ensaio, prioridade intermediária |
| `0x200` | módulos | função local de demonstração |
| `0x280 + 2N`, `+1` | módulo N | sensor ouvido por BLE |
| `0x300`, `0x301` | Probe 00 | pedido de associação |
| `0x302`, `0x303` | líder | pedido de reassociação |
| `0x304` | Probe 00 | comando a um sensor |
| `0x305`, `0x306` | Probe 00, módulos | comando e estado de ensaio |
| `0x307` | Probe 00 | etiqueta de autenticação do comando `0x304` ou `0x305` anterior (só com `--auth-can`) |
| `0x320 + 2N`, `+1` | módulo N | estado de um vínculo |
| `0x380 + N` | módulo N | dados dos sensores, segmentados |
| `0x7EF` | Probe 00 | carga de ensaio, prioridade baixa |

### Pinagem

| Placa | Sinal | Pino |
| --- | --- | --- |
| ESP32 (Probe e módulos) | MCP2515 SCK, SO, SI, CS, INT | GPIO 13, 19, 23, 5, 4 |
| ESP32 (Probe e módulos) | LED líder, falha, estado (acesos em nível baixo) | GPIO 25, 26, 27 |
| Pico W (vibração) | MPU6050 SDA, SCL | GP0, GP1 (VCC em 3V3(OUT), endereço `0x68`) |
| Pico W ou Pico 2 W (acústico) | INMP441 SCK, WS, SD | GP18, GP19, GP20 (L/R em GND, VDD em 3V3(OUT)) |

### Prazos do firmware, para conferir os tempos medidos

| Evento | Prazo |
| --- | --- |
| Eleição completa | ≈ 7,5 s (5 s de candidaturas, 1,5 s de recuperação, 1 s até o primeiro sinal) |
| Seguidor declarado ausente | 5 sinais de presença |
| Probe pede eleição | 4 sinais sem líder |
| Seguidor pede eleição | 6 sinais, mais 20 ms por posição abaixo do ID 31 |
| Vínculo `ONLINE` para `STALE`, e para `LOST` | 15 s sem anúncio, mais 45 s |
| Republicação dos vínculos | a cada 10 s |
| Verificação da sessão pelo módulo | a cada 3 s |
| Sessão encerrada por silêncio | 10 s sem tráfego (parâmetro de compilação, RT 89) |
| Sensor desiste de autenticar | 60 s |
| Sensor aceita oferta de outro módulo | 15 s depois de perder a sessão (parâmetro de compilação, RT 89) |
| Primeiro canal de dados protegido da reassociação automática | 120 s |
| Módulo declara o próprio isolamento | 6 s sem quadro e transmissões sem confirmação; volta depois de 3 s de quadros contínuos |
| Desafio sem resposta | 5 s |
| Intervalo mínimo entre reassociações do mesmo sensor | 30 s |
| Módulo reiniciado retoma vínculos | depois de 12 s escutando |
| Confirmação de comando pelo sensor | 3 s, senão `SENSOR_TIMEOUT` |
| Transferência segmentada incompleta | descartada em 1,5 s |
| Sessão de manutenção no console | 300 s |
| Ajuste de hora pela estação | ao conectar e a cada 10 min |

### Alarmes (DTC)

| Código | Significado |
| --- | --- |
| `0x1001` | MPU6050 sem comunicação no I²C |
| `0x1002` | barramento I²C travado |
| `0x1003` | microfone INMP441 sem dados válidos |
| `0x2001` | vetor de gravidade fora da faixa (sensor solto, impacto) |
| `0x2002` | saturação do acelerômetro |
| `0x2003` | sobrecarga acústica (≈ 120 dB SPL) |
| `0x3001` | processamento atrasado; no acústico, blocos de áudio perdidos |
| `0x3002` | núcleo de processamento sem progresso (acústico) |
| `0x4001` | tensão de alimentação baixa |
| `0x4002` | falhas repetidas de envio pelo Wi-Fi |
| `0x4003` | mensagem recusada por chave de autenticação |
| `0x4004` | versão de protocolo incompatível |
| `0x4005` | mensagem repetida ou antiga |
| `0x4006` | configuração recusada por campo inválido |

## Bom saber

### Pendências de bancada do repositório e onde este roteiro as cobre

O documento de validação do repositório lista o que só foi verificado na bancada virtual. Estes são os RT que fecham cada item:

| Pendência listada | RT |
| --- | --- |
| abertura do canal de dados, com um e com dois sensores | 55, 65 |
| rádio do módulo com a escuta BLE em 25 % | 55, 84 |
| espectro de 256 faixas | 58 |
| medição: ocupação, período do sinal de presença, contadores | 26, 70 |
| vigilância do líder sem a Probe (≈ 14 s) | 34 |
| tempos de reassociação | 61, 62, 68 |
| ensaios de falha: modo somente escuta, volta do ponto de acesso, carga sustentada | 68, 69, 70 |
| sensor acústico: calibração, ocupação do processamento, níveis | 45 a 50, 64, 85 |
| painel CYD: cores, toque, LED, som, redesenho, hora | 18, 51, 66 |
| leitor da documentação | 19, 52 |
| relógio da rede | 28, 66 |
| vínculos gravados após reinício | 63 |

### Detalhes que economizam tempo

- **Dois sistemas de numeração.** Nos comandos de quatro bytes o ID do módulo é hexadecimal (módulo 10 é `0A`). Em `WIRELESS BIND`, `FAULT` e `ECHO` é decimal (confira o ECHO na sua bancada; os dois primeiros foram lidos no código). Com menos de 10 módulos você não percebe; com mais, erra o alvo.
- **Probe e módulo são a mesma placa.** Só o ID gravado muda. Uma Probe com defeito se substitui regravando qualquer módulo com o ID 0.
- **`WIRELESS ...` só existe na Probe.** Um módulo comum ignora esses comandos pela própria USB; os de quatro bytes e `TIME` ele aceita.
- **`sensores_ativos` no sinal de presença conta módulos**, não sensores sem fio. O nome vem da função local de demonstração.
- **Toda placa diz o que tem gravado.** `[BOOT] FIRMWARE=... COMMIT=...` na partida dos ESP32 e `VERSION` nos módulos e nos sensores. Um `COMMIT=...-dirty` significa que a placa foi gravada com alterações locais; `COMMIT=unknown`, que o build não estava num repositório git.
- **`build_pico.sh` guarda uma pasta por variante.** `build/pico_w/`, `build/pico2_w/`, `build/pico_w-<UUID>/`: compilar para outra placa ou outro sensor não apaga o `.uf2` anterior.
- **Portas estáveis.** `/dev/ttyUSB0` muda conforme a ordem de conexão. Use os caminhos de `/dev/serial/by-id/`, que identificam cada placa.
- **`logs/` e `exports/` ficam em `Front/`**, a pasta em que os scripts abrem a estação.
- **A sessão não é gravada na memória do módulo; o vínculo é.** Depois de qualquer reinício o sensor se autentica de novo.
- **Os dados são autenticados, não cifrados.** Quem captura o Wi-Fi do módulo e conhece a senha lê a telemetria; não consegue forjá-la.
- **A chave `000102...1f` dos testes é pública.** Nunca a grave em hardware.
- **Três módulos, três canais.** Com os IDs 1, 2 e 3 cada ponto de acesso fica num canal (6, 11 e 1). O quarto módulo repete o canal do primeiro.
- **A demonstração não reage aos ensaios de falha.** Para ensaiar sem hardware, use uma gravação ou o laboratório virtual.
- **Nível lógico do MCP2515 (conhecimento geral, não está no repositório).** O transceptor TJA1050 exige 5 V. Num módulo alimentado em 5 V, a saída SO chega ao GPIO 19 do ESP32 em 5 V, acima dos 3,3 V que ele especifica. Costuma funcionar, mas um divisor resistivo ou conversor de nível nessa linha é a prática segura para uma montagem que vai ficar ligada.

### Sobre a identificação do operador

- **Ela separa pessoas, não resiste a um administrador.** Quem controla a conta do Linux pode trocar senhas, editar o cadastro ou apagar o banco. O objetivo é saber quem fez o quê entre pessoas que dividem a bancada.
- **Só entra na auditoria o que passa pela estação.** Os comandos da Fase 3, digitados no monitor serial da Probe, e os do console USB de um sensor (Fase 5) não ficam registrados. As marcas feitas no painel físico continuam sem autor.
- **Sem cadastro, o nome é o da conta.** `sys:<conta>` identifica a conta do Linux, não a pessoa. Numa conta compartilhada, isso não distingue ninguém.
- **O banco antigo é aproveitado.** Um `historico.sqlite3` de sessões anteriores ao commit é migrado ao abrir; as sessões antigas ficam, sem ações de auditoria.
- **A pasta importa.** O histórico fica em `logs/` da pasta em que a estação foi aberta. Pelos scripts é sempre `Front/`; chamando o programa direto, entre em `Front/` antes.
- **A faixa de status aparece em qualquer captura de tela.** Sem cadastro, ela mostra o nome da sua conta do Linux. Se for gravar ou fotografar a estação, cadastre um operador com o nome que você quer exibir.

### Como registrar a sessão

Cada sessão de bancada vira uma entrada em `Documentacao/historico/` (`bancada-fisica-AAAA-MM-DD.md`), escrita a partir do [modelo de sessão](modelo-de-sessao.md): montagem, o que funcionou, o que falhou, o que ficou em aberto, com os tempos medidos. Cada problema novo entra em [Desafios](../desafios.md) com um número `CH-`. Junte à entrada o arquivo `gravacao_*.log` (`python -m pico_tui --record-session`), os arquivos de `exports/`, a linha `CONFIG` do RT 83, a saída de `operadores.sh audit --since 24h` com o número de registros que o `audit --verify` conferiu e a saída de `Simulacao/comparar_bancada.py` para o cenário correspondente.

## Situação das propostas de melhoria

Estas 21 propostas saíram da primeira versão deste roteiro. A coluna «Situação» diz o que o repositório fez com cada uma; nada foi validado em hardware, e o que depende dele está dito.

| # | Proposta | Situação |
| --- | --- | --- |
| 1 | Imprimir versão e commit no boot de todos os firmwares e em `VERSION` dos sensores | feita: `[BOOT]`, `VERSION` e `BOOT` dos sensores (RT 14 a 17) |
| 2 | Uma única fonte para a versão | feita: arquivo `VERSION` e `versao.py --check` (RT 2) |
| 3 | Corrigir o guia de uso (`MIC=OK`) ou o firmware (`MIC=YES`) | feita no firmware: `MIC=OK`, `NO_HARDWARE`, `SILENT` ou `STUCK_HIGH` |
| 4 | Script de fumaça pós-gravação | feita: `smoke_boot.py`; testado com capturas, não com portas reais |
| 5 | Build que para, em vez de só avisar, quando falta a chave | feita nos scripts do módulo e do sensor, com `--sem-chave` explícito (CH-056) |
| 6 | Comando `SELFTEST` no módulo | feita (RT 86); o laço interno do MCP2515 só foi exercitado na bancada virtual |
| 7 | `build_pico.sh` com pasta de saída por placa e por sensor e repasse dos pinos do microfone | feita (`--mic-pins`); a compilação do Pico só roda na integração contínua |
| 8 | Versionar este roteiro com um modelo de registro de sessão | feita: este arquivo e `modelo-de-sessao.md` |
| 9 | Script que compara os tempos da gravação física com os da bancada virtual | feita: `Simulacao/comparar_bancada.py`, com tolerância e código de saída |
| 10 | Bancada mínima em integração contínua | parcial: fluxo `bancada-fisica.yml` (`workflow_dispatch`, runner próprio) que confere a Probe, os módulos e os sensores e compara uma gravação com a bancada virtual; nunca foi executado, porque exige um runner com as placas; ver «Bancada física na integração contínua» em [Validação](../validacao.md) |
| 11 | Motivo dos reinícios, tensão de entrada e corrente | motivo e contagem já existiam; tensão (divisor resistivo no ADC1) e corrente (INA219) feitas como medição, sem ação sobre elas (RT 87); não validadas em hardware |
| 12 | O módulo detectar o próprio isolamento do barramento | feita (`ISOLAMENTO`, `[ISOLADO]`); só na bancada virtual, e o RT 71 confirma |
| 13 | Ajustar as esperas do sensor com medição física | parcial: as esperas viraram parâmetros de compilação, com os padrões de sempre; o valor novo depende da medição da bancada física (RT 89) |
| 14 | Autenticação de origem nos comandos CAN | parcial: `0x304` e `0x305` (opção `--auth-can`, desligada por padrão; RT 88); o `0x080` não, por ter vários emissores |
| 15 | Comissionamento sem compilação | adiada |
| 16 | Verificação FIDO2/OTP da chave física | adiada para quando o projeto estiver mais evoluído |
| 17 | Classificador de eventos sonoros treinado com sons da instalação | só a coleta de exemplos rotulados (`:rotulo`); o classificador não foi treinado |
| 18 | Hash final da cadeia de auditoria anotado fora do banco | feita (RT 80) |
| 19 | Comparar o identificador do sensor pelo valor | feita (RT 79) |
| 20 | `run_tui.sh` repassar o tempo de inatividade e o arquivo de cadastro | feita (RT 78) |
| 21 | Corrigir o aviso do `check` com cadastro vazio e o título das associações recusadas | feita (RT 77 e 79) |

## Fontes

O roteiro foi escrito a partir do repositório e acompanha-o. Documentos citados:

- [README](../../README.md)
- [Guia de uso](../guia-de-uso.md)
- [Validação](../validacao.md)
- [Arquitetura](../arquitetura.md)
- [Protocolo](../protocolo.md)
- [Sensor acústico](../sensor-acustico.md)
- [Segurança](../seguranca.md)
- [TUI](../tui.md)
- [Desafios](../desafios.md)
- [Bancada física de 08/10/2026](../historico/bancada-fisica-2026-10-08.md)
- [Ferramenta de pesquisa](../../Pesquisa/README.md)
- Código lido para as mensagens exatas: `Codigo/node-can/src/main.ino`, `comandos.cpp`, `wireless_link.cpp`, `relogio.cpp`, `fault_injection.cpp`; `Codigo/node-wifi/serial_console.c`, `edge_network_driver.c`; `Codigo/node-audio/audio_console.c`; `Codigo/scripts/`.

Para a identificação do operador (RT 74 a 80): a seção 5 de [Segurança](../seguranca.md), a [TUI](../tui.md) e `Front/pico_tui/operators.py`, `operators_cli.py`, `audit.py`, `dialogs.py`, `history/`.
