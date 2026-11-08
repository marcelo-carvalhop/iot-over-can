# IoT-over-CAN — ponte de sensores reais USB / Simulation Studio (etapa 5)

Este pacote incremental integra **Pico W / Pico 2 W reais conectados por USB CDC** à topologia da bancada virtual das etapas 1–4. O firmware do Pico executa a aquisição física e fornece mensagens `TEL` pelo console USB. A ponte lê essas mensagens e alimenta a representação do sensor na simulação, que continua a realizar descoberta e associação lógica, sessão emulada com o Node CAN, transmissão CAN, Probe 00 e TUI.

**Precisão de interpretação:** a aquisição USB é real; o enlace BLE e a conexão Wi-Fi com o Node virtual **não** são físicos. Não há handshake HMAC entre o Pico físico e o Node virtual; o firmware da instância virtual executa o handshake com o sensor virtual correspondente. A telemetria de origem USB é marcada como `source=USB` no Studio. Comandos de configuração enviados pela TUI ao Node virtual **não** alteram automaticamente o Pico físico. A ponte implementa o caminho de dados físico→virtual, não a equivalência bidirecional de todo o protocolo wireless.

## 1. Instalação (raiz do checkout do projeto)

Pare Studio, TUI e o monitor PlatformIO. Certifique-se de que não há modificações locais não salvas nos arquivos envolvidos e trabalhe em uma branch dedicada. Extraia este pacote fora do repositório e, da raiz do checkout, execute:

```bash
bash /caminho/ioc_usb_stage5/install.sh .
python3 -m unittest discover -s Simulacao/tests -v
bash Simulacao/verify.sh
./Codigo/scripts/build_network_sim.sh /tmp/ioc_sim_network
```

O instalador verifica as âncoras do código das etapas anteriores, é idempotente e não modifica firmware Pico ou ESP32. Caso as âncoras não coincidam, ele interrompe sem editar os arquivos existentes. Verifique `git diff --check` e `git status -sb` antes de integrar ao Git.

## 2. Iniciar simulação e TUI

Terminal 1, na raiz do checkout:

```bash
python3 Simulacao/studio/server.py --binary /tmp/ioc_sim_network --browser
```

No navegador: `http://127.0.0.1:8765`. Copie a porta `PORT=/dev/pts/N` que aparece no terminal do servidor.

Terminal 2, na raiz do checkout:

```bash
./Codigo/scripts/setup_tui.sh  # somente na primeira instalacao
cd Front
.venv/bin/python -m pico_tui --port /dev/pts/N --mode probe --security-mode off
cd ..
```

Não use `--sim` quando o Studio já gerencia a instância, pois criaria outra bancada.

## 3. Conectar um Pico real USB

Conecte um Pico W/Pico 2 W com firmware do projeto ao USB. Identifique a porta:

```bash
python3 -m serial.tools.list_ports -v
# ou: ls -l /dev/serial/by-id/ /dev/ttyACM* 2>/dev/null
```

Use a porta persistente `/dev/serial/by-id/...` se disponível. Feche `pio device monitor`, `minicom`, `picocom` e outras aplicações que abram a mesma porta. Apenas a ponte pode possuir a USB durante a execução.

Terminal 3, na raiz do checkout:

```bash
Front/.venv/bin/python Simulacao/usb_sensor_bridge.py \
  --port /dev/ttyACM0 \
  --node 1 \
  --rssi -55
```

O programa solicita `VERSION` e `STATUS` ao Pico, identifica automaticamente `NODE_UUID` e o perfil (vibração ou acústico), cria/reutiliza o sensor virtual, configura seu alcance BLE em relação ao Node 01, marca `source=USB` e passa a enviar as amostras reais `TEL` por um socket local ao simulador. Com a TUI aberta, entre em **F7 / Sensores sem fio**; depois de o candidato aparecer, associe pela TUI ou pelo CLI:

```bash
python3 Simulacao/sim_ctl.py PROBE WIRELESS BIND 1 0x<UUID_REAL> AUTO
```

Substitua `0x<UUID_REAL>` pelo UUID completo informado pela ponte. Confirme a associação e os relatórios `WIRELESS_LINK`/`TEL` na TUI e no log `Simulacao/results/studio-live.log`. No Studio, o inspetor do sensor exibe `Origem da aquisição = Sensor físico (USB CDC)`.

## 4. Controle de autorização do firmware Pico

O firmware real requer autenticação para `TELEMETRY ON` quando o console USB está protegido. A ponte não contorna esta regra. Quando o firmware já produz telemetria USB, use `--passive` para não enviar comandos de ativação. Caso contrário, forneça um arquivo local restrito que contenha o token administrativo provisionado *no firmware real*:

```bash
umask 077
read -rsp 'Token administrativo do Pico: ' IOC_TOKEN; printf '\n'
printf '%s\n' "$IOC_TOKEN" > "$HOME/.config/ioc-usb-admin-token"
unset IOC_TOKEN
chmod 600 "$HOME/.config/ioc-usb-admin-token"

Front/.venv/bin/python Simulacao/usb_sensor_bridge.py \
  --port /dev/ttyACM0 --node 1 --rssi -55 \
  --token-file "$HOME/.config/ioc-usb-admin-token"
```

O exemplo pressupõe que `~/.config` existe. Use `mkdir -p ~/.config` se necessário. Não armazene esse arquivo no repositório e não passe o token literal na linha de comando. Se o firmware foi compilado sem um token administrativo provisionado, será necessário provisioná-lo adequadamente; a ponte não desativa a segurança.

## 5. Múltiplos sensores e desligamento

Execute um processo da ponte por porta física, cada um com um UUID próprio. É possível apontar sensores diferentes para Nodes diferentes com `--node` e `--rssi`, inclusive enquanto o Studio e a TUI estão abertos. Ao encerrar com Ctrl+C, a ponte desliga sua representação virtual (`SENSOR POWER ... OFF`), permitindo observar perda de presença. Para religar, reinicie o mesmo comando; o sensor virtual desligado é reutilizado. O objeto não é desassociado à força pela ponte: o firmware simulado decide as transições de estado.

## 6. Comandos e diagnóstico

```bash
python3 Simulacao/sim_ctl.py STATUS
python3 Simulacao/sim_ctl.py HELP
python3 Simulacao/sim_ctl.py SENSOR SOURCE 0x<UUID_REAL> USB
python3 Simulacao/sim_ctl.py SENSOR POWER 0x<UUID_REAL> OFF
```

`SENSOR SAMPLE` é um comando interno da ponte; não se deve enviar amostras fabricadas a uma entidade marcada como de origem real. Se a TUI não descobrir o sensor, confirme `STATUS` (`source=USB`, `links=1`), `running=1`, Node ligado, e verifique eventos `WIRELESS_CANDIDATE`. Se o Studio apresentar `usb_age_ms` crescente, confira a serial ou a autorização do streaming. Se a porta não abrir, veja permissões do grupo `dialout`; alterações de grupo exigem uma nova sessão de login.

## 7. Limites desta versão

- A aquisição física da vibração e do áudio vem do firmware conectado por USB. A origem não é ocultada.
- O canal de envio USB não equivale ao plano de dados HMAC-SHA256 real por Wi-Fi, e as métricas de rádio permanecem simuladas. Não use esses ensaios para concluir a latência física do rádio.
- Os relatórios `TEL` não transportam vetores FFT completos. Nesta versão as faixas FFT do Pico real não são encaminhadas ao CAN. Marcadores como `SPECTRUM_VALID` não são fabricados em cima de espectros inexistentes.
- A ponte depende do firmware fornecer `VERSION` com UUID64 e `STATUS` com indicação de perfil. O sensor vibração é reconhecido na ausência de `PROFILE=ACOUSTIC`.
- A TUI opera comandos CAN virtuais; a configuração real do sensor via TUI não foi redirecionada para a USB. Isso poderá ser desenvolvido como segunda fase de HIL bidirecional, com controle de concorrência e confirmação de configuração.
- A desconexão USB interrompe o envio de amostras; a representação virtual é desligada pela ponte ao encerrar, sem produzir amostras sintéticas substitutivas.

Nenhum arquivo é transmitido ao GitHub remoto. Valide a compilação completa no checkout local antes de fazer um commit.
