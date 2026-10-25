Import("env")
import os
import re

# Chaves do painel. Nunca versionadas: Codigo/scripts/build_paper.sh as lê de
# .env.local (bancada: chave mestra, UUID pelo endereço MAC) ou de
# .env.sensor-<UUID>.local (chaves derivadas para um painel com UUID fixo).
HEX64 = re.compile(r"^[0-9a-fA-F]{64}$")


def define(name, value):
    env.Append(CPPDEFINES=[(name, env.StringifyMacro(value))])


master = os.environ.get("IOT_PANEL_MASTER_KEY", "").strip()
device = os.environ.get("IOT_PANEL_DEVICE_KEY", "").strip()
network = os.environ.get("IOT_PANEL_NETWORK_KEY", "").strip()
uuid = os.environ.get("IOT_PANEL_UUID", "").strip()
for label, value in (("IOT_PANEL_MASTER_KEY", master), ("IOT_PANEL_DEVICE_KEY", device), ("IOT_PANEL_NETWORK_KEY", network)):
    if value and not HEX64.match(value):
        raise ValueError(f"{label} inválida: use 64 caracteres hexadecimais.")
if device or network or uuid:
    if not (device and network and re.fullmatch(r"(0x)?[0-9a-fA-F]{16}", uuid)):
        raise ValueError("Chaves por dispositivo exigem IOT_PANEL_DEVICE_KEY, IOT_PANEL_NETWORK_KEY e IOT_PANEL_UUID.")
    define("IOT_PANEL_DEVICE_KEY_HEX", device)
    define("IOT_PANEL_NETWORK_KEY_HEX", network)
    env.Append(CPPDEFINES=[("IOT_PANEL_UUID", "0x" + uuid.lower().removeprefix("0x") + "ULL")])
    print("iot-over-can: painel com chaves derivadas para o UUID " + uuid)
elif master:
    define("IOT_PANEL_MASTER_KEY_HEX", master)
    print("iot-over-can: painel em modo de bancada (chave mestra; UUID pelo MAC)")
else:
    print("iot-over-can: painel sem chave de enlace; só funcionará pela USB com a bancada virtual")
