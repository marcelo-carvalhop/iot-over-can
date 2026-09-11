# Build e teste

Todos os scripts operacionais ficam em `Codigo/scripts/` e podem ser chamados a partir da raiz do repositório.

## TUI

Instalação ou atualização do ambiente virtual:

```bash
./Codigo/scripts/setup_tui.sh
```

O script cria `Front/.venv` e instala o pacote com as dependências de desenvolvimento declaradas em `Front/pyproject.toml`.

Execução sem porta pré-selecionada:

```bash
./Codigo/scripts/run_tui.sh
```

Execução conectando diretamente à Probe 00:

```bash
./Codigo/scripts/run_tui.sh /dev/ttyUSB0
```

Testes:

```bash
./Codigo/scripts/test_tui.sh
```

## Node CAN — ESP32 + MCP2515

O ID deve ser informado explicitamente. Para build do Node 1:

```bash
./Codigo/scripts/build_esp32_can_node.sh 1
```

Para upload do Node 2:

```bash
./Codigo/scripts/upload_esp32_can_node.sh 2 /dev/ttyUSB0
```

O script define `IOT_NODE_ID` para o processo PlatformIO. Não é necessário editar `main.ino` para alterar o ID. O ID 0 é reservado à Probe 00; os Nodes funcionais devem usar IDs distintos.

Monitor serial direto:

```bash
cd Codigo/node-can
pio device monitor -b 115200
```

## Sensor wireless — Raspberry Pi Pico W

Antes do primeiro build destinado a operar comandos mutáveis, provisione os arquivos de segurança conforme `modelo_de_seguranca.md`.

Build padrão:

```bash
./Codigo/scripts/build_pico.sh
```

O alvo padrão é `pico_w`. Um alvo diferente só deve ser informado quando o hardware correspondente estiver realmente em uso:

```bash
./Codigo/scripts/build_pico.sh pico2_w
```

O artefato esperado é:

```text
Codigo/node-wifi/build/edge_node_firmware.uf2
```

## Testes nativos do firmware

```bash
./Codigo/scripts/test_native_firmware.sh
```

Esse teste compila e executa a validação nativa de configuração sem depender do hardware.

## Validação de bancada

Com o Pico W ligado, cada Node funcional deve iniciar o scanner BLE com:

```text
[NODE N] BLE_SCAN=ACTIVE
```

Ao detectar o sensor, a serial do Node deve apresentar UUID, perfil, protocolo e RSSI. A Probe 00 deve recompor a observação e emitir `WIRELESS_CANDIDATE`. Na TUI, o mesmo UUID deve aparecer observado por todos os Nodes que estiverem ao alcance, com RSSI independente.

O valor local `0xAA` deve atualizar o painel de estado do Node sem produzir uma nova linha de EventLog a cada rodada.

## Integração contínua

Os workflows em `.github/workflows/` executam testes da TUI, validação C nativa e builds de firmware. Os scripts usados pela CI são os mesmos de `Codigo/scripts/`, reduzindo divergência entre desenvolvimento local e GitHub Actions.

### Comportamento da CI no GitHub

A integração contínua é definida apenas em `.github/workflows/ci.yml`. Pushes que alteram somente documentação Markdown não iniciam builds de firmware nem testes da TUI. Alterações de código, scripts, configuração ou workflow executam três verificações independentes: testes da TUI e validação C nativa, build do Pico W e build do Node CAN ESP32.

Os scripts são invocados explicitamente com `bash` dentro da CI. Ainda assim, os arquivos de `Codigo/scripts/` devem ser versionados com permissão executável (`100755`), pois no uso local eles são chamados diretamente com `./Codigo/scripts/...`.

Para conferir as permissões armazenadas pelo Git antes de um push:

```bash
git ls-files -s Codigo/scripts/*.sh
```

A primeira coluna deve ser `100755` para todos os scripts. Se algum aparecer como `100644`, corrija o índice com:

```bash
git update-index --chmod=+x Codigo/scripts/*.sh
```

Para reproduzir localmente a parte de testes da CI:

```bash
./Codigo/scripts/setup_tui.sh
./Codigo/scripts/test_tui.sh
./Codigo/scripts/test_native_firmware.sh
```
