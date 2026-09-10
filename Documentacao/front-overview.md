# Software

## `tui`

Interface textual de supervisão, diagnóstico e comando do sensor/gateway.

Instalação local:

```bash
cd Front
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -e .
pico-tui --port /dev/ttyACM0 --mode sensor --security-mode presence
```
