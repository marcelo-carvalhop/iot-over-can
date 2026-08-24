# Build e testes

## Firmware Pico

```bash
cd sensor_pico_polling_secure
rm -rf build
mkdir build
cd build
cmake -DPICO_BOARD=pico2_w ..
cmake --build . -j$(nproc)
```

Teste serial esperado no boot:

```text
STATUS
```

Resultado esperado:

```text
STATUS NET=DISABLED ... ACQ=POLLING ... DTC=0x0000 ... DRDY_IRQ=0 DRDY_MISSED=0
```

Para habilitar Wi-Fi manualmente:

```text
NET WIFI ON
NET WIFI STATUS
```

Para desligar:

```text
NET WIFI OFF
```

## TUI

```bash
cd tui_secure
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -e .
```

Execução com YubiKey por presença:

```bash
pico-tui --port /dev/ttyACM0 --mode sensor --security-mode presence
```

Execução de desenvolvimento sem proteção:

```bash
pico-tui --port /dev/ttyACM0 --mode sensor --security-mode off
```

Comandos internos relevantes:

```text
:wifi on
:wifi off
:wifi status
:security
:lock
:unlock <otp>
```

## Verificações feitas nesta integração

- A árvore Python da TUI foi validada com `compileall`.
- Os testes unitários completos não foram executados neste ambiente porque a dependência `textual` não está instalada no runtime da sessão.
- O firmware Pico não foi compilado aqui contra o Pico SDK real; a alteração foi feita sobre código previamente compilável e deve ser validada no ambiente local com o comando CMake acima.
