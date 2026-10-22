# IoT-over-CAN — Etapa 6, FFT fisica no USB-HIL

## Objetivo

Integra o comando `FFT` da TUI conectada a Probe 00 virtual ao **Pico acustico fisico** na USB, sem transformar o resultado em espectro sintetico. A Probe recebe o comando, o simulador mapeia o endereco logico (Node.Child) ao sensor USB (UUID64), a ponte solicita `FFT ONCE` ao Pico, valida a linha `FFT VALID=YES ... VALUES=...`, e injeta as magnitudes reais no `Payload_AcousticStream`. O firmware Node CAN compilado no host converte as faixas ao protocolo CAN e a Probe entrega a TUI. A TUI existente nao precisa ser alterada.

Nao substitui o enlace BLE/Wi-Fi fisico, que segue **emulado**. Nao altera o firmware do Pico nem seus requisitos de autenticacao. Apenas a FFT sob demanda do perfil `ACOUSTIC` e suportada nesta entrega; pedidos de configuracao de DSP e outros comandos continuam virtuais.

## Aplicar a partir da raiz do repo (Etapas 1–5 instaladas)

Antes de instalar, encerre **Studio**, **TUI** e **ponte USB**, e trabalhe em branch isolada com copias dos arquivos locais modificados. O instalador verifica as ancoras existentes, os hashes dos arquivos esperados e recusa sobrescrever modificacoes inesperadas. Execute:

```bash
unzip ~/Downloads/iot-over-can-usb-fft-etapa6.zip -d /tmp
bash /tmp/ioc_usb_fft_stage6/install.sh .
python3 -m unittest discover -s Simulacao/tests -v
bash Simulacao/verify.sh
./Codigo/scripts/build_network_sim.sh /tmp/ioc_sim_network
```

A primeira verificacao de instalacao pode ser feita sem aplicar nada:

```bash
python3 /tmp/ioc_usb_fft_stage6/patch.py --check .
```

## Execucao

Terminal 1 na raiz:

```bash
python3 Simulacao/studio/server.py --binary /tmp/ioc_sim_network --browser
```

Terminal 2: usar a porta PTY publicada como `PORT=/dev/pts/N` no terminal do Studio (nao iniciar outra bancada com `--sim`):

```bash
cd Front
.venv/bin/python -m pico_tui --port /dev/pts/N --mode probe --security-mode off
```

Terminal 3, raiz do repo (substitua o caminho se a porta by-id for diferente):

```bash
Front/.venv/bin/python Simulacao/usb_sensor_bridge.py \
  --port /dev/serial/by-id/usb-Raspberry_Pi_Pico_E6616408430D5239-if00 \
  --node 1 --rssi -55 --period-ms 1000 \
  --token-file ~/.config/ioc-usb-admin-token
```

Se necessario, associe o UUID do Pico na TUI (F7). Aguarde estado `SECURE` e chegada de `TEL`. Depois acesse o sensor 01.01 -> **Espectro** e pressione **n** (novo espectro), ou use `:fft 01.01 bins=64` na interface de comandos da TUI.

## Resultado esperado

Na TUI, depois do pedido, sao esperadas as transicoes `COMMAND ACCEPTED`, `COMMAND APPLIED` e uma linha semelhante a `FFT recebida de 01.01: 64 bins`. Na ponte USB, procure:

```
[usb] pedido FFT da Probe encaminhado ao Pico
[usb] FFT fisica entregue: 256 bins, 16000 Hz em 0.62 s
```

O tempo indicado vai do pedido detectado pela ponte ate a entrega ao simulador. O Pico so imprime a FFT no relatorio seguinte do DSP, que fecha uma vez por segundo; tempos entre 0 e ~1,2 s sao normais. Na Probe, `ACK COMMAND=FFT STATE=APPLIED` costuma chegar entre 1,5 e 2 s depois do pedido, pois o Node ainda reduz e fragmenta o espectro no CAN.

Os 256 bins da USB sao reduzidos pelo firmware do Node virtual para a resolucao solicitada (32, 64 ou 128 bins). O protocolo de fragmentacao CAN e o decodificador da TUI permanecem os originais. O nome do `TX` e as latencias sao determinados na execucao real.

Se aparecer `AUTH_REQUIRED`, mantenha o arquivo de token valido em permissao 0600. A ponte renova a autorizacao se necessario sem gravar o token nos logs. Se o Pico responder de fato `FFT VALID=NO`, a ponte pede o relatorio seguinte (no maximo 3 tentativas e somente enquanto ele couber no prazo); ela nao fabrica amplitudes, e sem espectro a requisicao termina por timeout no Node. Se `FFT QUERY` nao mudar de contador, a associacao 01.01/UUID nao esta completa ou a TUI esta ligada a outra simulacao.

### Eco do console e falha corrigida (22/10/2026)

O console do sensor acustico ecoa cada caractere recebido. Para cada pedido, a USB traz, nesta ordem:

```
EDGE> FFT ONCE            <- eco do comando digitado pela ponte
OK FFT_ONCE               <- confirmacao imediata
EDGE>
TEL ... SPECTRUM_VALID=YES ...
FFT VALID=YES BINS=256 RATE_HZ=16000.00 FFT_SIZE=512 UNIT=PA VALUES=...   <- proximo relatorio do DSP
```

Ate esta data a ponte tratava qualquer linha iniciada por `FFT ` como resposta. O eco `FFT ONCE` era lido como `FFT VALID=NO`; cada nova tentativa gerava outro eco, e a ponte desistia em milissegundos, antes de a linha verdadeira sair. O sintoma era este, sempre com `SPECTRUM_VALID=YES` no ultimo `TEL`:

```
[usb] FFT VALID=NO; nova tentativa 2/3
[usb] FFT VALID=NO; nova tentativa 3/3
[usb] FFT indisponivel apos 3 tentativa(s); MODE=MACHINE CPU_PCT=55 COVERAGE_PCT=100 SPECTRUM_VALID=YES ...
```

e na Probe, `ACK COMMAND=FFT STATE=REJECTED ... REASON=SENSOR_TIMEOUT RTT_MS=5001`. Agora so `FFT VALID=YES|NO` conta como resposta; o eco e o `OK FFT_ONCE` sao reconhecidos e nao consomem tentativas. Uma linha FFT truncada na USB ou uma recusa do simulador gera aviso, sem encerrar a ponte. O teste `Simulacao/tests/test_usb_fft_console_echo.py` emula o console byte a byte (eco, prompt e relatorio de 1 s).

### Mensagens da ponte

| Mensagem | Significado |
|---|---|
| `FFT fisica entregue: N bins, F Hz em T s` | espectro real injetado no sensor virtual |
| `FFT VALID=NO; nova tentativa k/3` | o Pico respondeu sem espectro naquele segundo; a ponte pede o proximo |
| `FFT do Pico rejeitada: ...; nova tentativa k/3` | a linha FFT chegou truncada ou incoerente (bins, taxa, magnitudes) |
| `FFT indisponivel apos k tentativa(s) (...)` | desistencia dentro do prazo; inclui os indicadores do ultimo `TEL` e `FFT_ONCE_ACK` |
| `FFT fisica nao chegou em 4.5 s; ... FFT_ONCE_ACK=NO` | o Pico nem confirmou o pedido: confira a porta, a autorizacao e se outro programa usa a USB |
| `FFT fisica nao chegou em 4.5 s; ... FFT_ONCE_ACK=YES` | o Pico aceitou mas nao produziu relatorio a tempo: confira `CPU_PCT`, `COVERAGE_PCT` e o modo |
| `simulador recusou o espectro: ...` | o Studio nao aceitou a injecao (pedido ja atendido, sensor desligado ou desassociado) |

### Indicadores espectrais na TUI

O Pico informa `SPECTRUM_VALID=YES` em todo relatorio com FFT calculada. A ponte repassa esse indicador ao simulador (campo `FFT_VALID` do `SENSOR SAMPLE`), e o sensor virtual mantem `WIN` igual a janela do DSP mesmo quando leva o vetor de espectro, como faz o firmware real. Antes, a TUI recebia `SPECTRUM_VALID=NO` em quase todos os segundos e ocultava frequencia dominante, centroide e planura; e `WIN` alternava entre 1024 e 512 a cada FFT. Essa parte exige recompilar o simulador (`./Codigo/scripts/build_network_sim.sh /tmp/ioc_sim_network`); com um binario anterior a ponte continua funcionando e o campo e simplesmente ignorado.

## Diagnostico

```bash
python3 Simulacao/sim_ctl.py STATUS
python3 Simulacao/sim_ctl.py SENSOR FFT_QUERY 0xE6616408430D5239
grep -E 'FFT|SPECTRUM|ACK COMMAND' Simulacao/results/studio-live.log | tail -n 40
```

O campo `request=` aumenta quando a Probe recebe um pedido FFT para o sensor USB associado. O retorno de `SPECTRUM` confirma apenas a injeção do vetor USB, nao garante entrega ao Node, à Probe ou à TUI: verifique `APPLIED` e `FFT recebida`.

## Limitacoes e validacao

Os testes locais validam parser de 256 bins, rejeicao de dados corrompidos, eco do console, novas tentativas, compilacao de stubs C++ e instalacao idempotente. Em 22/10/2026 o caminho completo foi exercitado com o simulador compilado do firmware real, a Probe na PTY e um Pico acustico emulado numa pseudo-serial (eco, autorizacao, `TEL` e `FFT` a cada segundo): dois pedidos `ACTION=FFT BINS=64` seguidos terminaram em `APPLIED` com `FRAG TYPE=FFT`, e a versao anterior da ponte reproduziu o `SENSOR_TIMEOUT`. **O teste com o Pico fisico ainda depende da bancada do usuario.** A transferencia pode atrasar ou falhar sob carga CAN elevada; 5 s e o prazo de timeout do Node para esse pedido. Os resultados nao representam latencia do BLE ou Wi-Fi fisicos.

Seguranca: nunca compartilhe o token `AUTH UNLOCK` no chat, em logs publicados ou em commits. Se o token foi exposto, planeje rotaciona-lo e regravar o firmware correspondente.
