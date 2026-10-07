Import("env")
import os

raw_node_id = os.environ.get("IOT_NODE_ID", "").strip()
if not raw_node_id:
    raw_node_id = "4"

try:
    value = int(raw_node_id, 0)
except ValueError:
    raise ValueError(f"IOT_NODE_ID inválido: {raw_node_id!r}. Use 0..31.")

# 0 = Probe 00; 1..31 = Nodes funcionais. Os identificadores CAN de
# descoberta, associação e dados wireless reservam uma posição por Node
# até 31, e o SSID do ponto de acesso usa dois dígitos.
if value < 0 or value > 31:
    raise ValueError(f"IOT_NODE_ID fora da faixa: {value}. Use 0..31.")

env.Append(CPPDEFINES=[("IOT_NODE_ID", value)])
print(f"Building iot-over-can ESP32 CAN node with IOT_NODE_ID={value}")

# Chave mestra do enlace wireless (64 caracteres hexadecimais). Nunca é
# versionada: Codigo/scripts/build_esp32_can_node.sh a lê de .env.local.
# Sem ela o firmware compila, mas o plano de dados permanece desabilitado.
raw_link_key = os.environ.get("IOT_LINK_MASTER_KEY", "").strip()
if raw_link_key:
    if len(raw_link_key) != 64 or any(c not in "0123456789abcdefABCDEF" for c in raw_link_key):
        raise ValueError("IOT_LINK_MASTER_KEY inválida: use 64 caracteres hexadecimais.")
    env.Append(CPPDEFINES=[("IOT_LINK_MASTER_KEY_HEX", env.StringifyMacro(raw_link_key))])
    print("iot-over-can: chave de enlace fornecida; plano de dados wireless habilitado")
elif value != 0:
    print("iot-over-can: IOT_LINK_MASTER_KEY ausente; plano de dados wireless desabilitado neste Node")
