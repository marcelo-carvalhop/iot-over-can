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
[usb] FFT fisica entregue: 256 bins, 16000 Hz
```

Os 256 bins da USB sao reduzidos pelo firmware do Node virtual para a resolucao solicitada (32, 64 ou 128 bins). O protocolo de fragmentacao CAN e o decodificador da TUI permanecem os originais. O nome do `TX` e as latencias sao determinados na execucao real.

Se aparecer `AUTH_REQUIRED`, mantenha o arquivo de token valido em permissao 0600. A ponte renova a autorizacao se necessario sem gravar o token nos logs. Se aparecer `FFT VALID=NO`, ela nao fabrica amplitudes e a requisição termina por timeout no Node. Se `FFT QUERY` nao mudar de contador, a associacao 01.01/UUID nao esta completa ou a TUI esta ligada a outra simulacao.

## Diagnostico

```bash
python3 Simulacao/sim_ctl.py STATUS
python3 Simulacao/sim_ctl.py SENSOR FFT_QUERY 0xE6616408430D5239
grep -E 'FFT|SPECTRUM|ACK COMMAND' Simulacao/results/studio-live.log | tail -n 40
```

O campo `request=` aumenta quando a Probe recebe um pedido FFT para o sensor USB associado. O retorno de `SPECTRUM` confirma apenas a injeção do vetor USB, nao garante entrega ao Node, à Probe ou à TUI: verifique `APPLIED` e `FFT recebida`.

## Limitacoes e validacao

Os testes locais do pacote validam parser de 256 bins, rejeicao de dados corrompidos, compilacao de stubs C++ e instalacao idempotente. **A compilacao com firmware completo e o teste de ponta a ponta com o Pico fisico ainda dependem do checkout do usuario.** A transferencia pode atrasar ou falhar sob carga CAN elevada; 5 s e o prazo de timeout do Node para esse pedido. Os resultados nao representam latencia do BLE ou Wi-Fi fisicos.

Seguranca: nunca compartilhe o token `AUTH UNLOCK` no chat, em logs publicados ou em commits. Se o token foi exposto, planeje rotaciona-lo e regravar o firmware correspondente.
