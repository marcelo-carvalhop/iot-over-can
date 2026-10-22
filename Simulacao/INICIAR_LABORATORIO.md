# Iniciar o laboratório IoT-over-CAN com um comando

Script Linux Mint para executar Studio, TUI (Probe 00 virtual) e ponte USB-HIL do Pico W em **três terminais gráficos independentes**, com identificação automática da PTY e do socket de controle gerados pelo Studio. Não altera firmware nem armazena o token administrativo: somente encaminha seu caminho (`--token-file`).

## Instalação

Após atualizar a branch, na raiz do repositório:

```bash
chmod +x Simulacao/iniciar_laboratorio.py
```

Para iniciar **Studio + TUI + Pico USB** com os parâmetros da bancada já configurados:

```bash
./Simulacao/iniciar_laboratorio.py
```

O lançador usa a porta física por ID `usb-Raspberry_Pi_Pico_E6616408430D5239-if00`, Node 1, RSSI virtual -55 dBm, período de 1000 ms, arquivo de token `~/.config/ioc-usb-admin-token`, navegador no Studio e TUI em modo Probe. O modo de segurança da TUI é `off` apenas para esta bancada de testes; o firmware continua exigindo autenticação para comandos protegidos.

O script abre três terminais: **Studio**, **TUI** e **USB-HIL**. Lê automaticamente `PORT=/dev/pts/N` e `CONTROL=.../control.sock` produzidos pelo Studio, repassando ambos aos respectivos componentes. Não depende de um número fixo de `/dev/pts` nem assume que só haja um simulador no sistema. O binário `/tmp/ioc_sim_network` é compilado automaticamente se estiver ausente ou se o código-fonte for mais recente. Use `--rebuild` para forçar recompilação.

Na TUI, abra `F7` e associe o Pico identificado como `0xE6616408430D5239` ao Node 01. Em seguida, abra o sensor `01.01` e solicite a FFT na tela Espectro. A associação é manual para manter o controle da operação sobre o Node responsável.

## Outros comandos

```bash
./Simulacao/iniciar_laboratorio.py status
./Simulacao/iniciar_laboratorio.py stop
./Simulacao/iniciar_laboratorio.py --rebuild
./Simulacao/iniciar_laboratorio.py --no-usb
./Simulacao/iniciar_laboratorio.py --no-browser
./Simulacao/iniciar_laboratorio.py --dry-run
```

`--no-usb` inicia apenas Studio e TUI, mesmo se o Pico físico estiver desconectado. `--dry-run` mostra as ações sem iniciar processos. Se outro dispositivo for conectado, ajuste `--device /dev/serial/by-id/...`; para outra porta HTTP use `--studio-port 8766`.

Se a TUI ainda não estiver instalada, execute previamente `./Codigo/scripts/setup_tui.sh`. O lançador procura `gnome-terminal`, `mate-terminal`, `xfce4-terminal`, `konsole` ou `xterm`. É possível forçar um com `--terminal mate-terminal`. O Studio usa a porta HTTP padrão 8765; se estiver ocupada, o script recusa sobrescrever uma sessão existente.

O arquivo `studio.log`, o status e os metadados desta bancada ficam em `~/.local/state/iot-over-can/laboratorio-<identificador>/`. O identificador é derivado do caminho absoluto do repositório para não misturar clones diferentes. `stop` encerra somente os processos criados pelo lançador. O console USB físico não pode estar aberto no miniterm, PlatformIO monitor ou outra TUI direta ao iniciar a ponte.

## Testes

```bash
python3 -m unittest Simulacao.tests.test_lab_launcher -v
```

Os testes usam um Studio, uma TUI e uma ponte USB emulados: não precisam de Pico físico, interface gráfica ou compilador real. A validação da inicialização e do encerramento em hardware real ainda deve ser feita no Linux Mint do projeto. Em uma máquina Linux, o teste exercita o marcador de sessão em `/proc` e uma PTY real, mas **não garante** compatibilidade gráfica de todos os emuladores de terminal.

O script foi criado de forma independente da correção da FFT: executa a versão da ponte que estiver na branch atual. A branch `integration/usb-bridge-acoustic-fft-consolidated` inclui a correção do eco serial (PR #4).
