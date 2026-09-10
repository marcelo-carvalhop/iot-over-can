Import("env")
import os

raw_node_id = os.environ.get("IOT_NODE_ID", "").strip()
if not raw_node_id:
    raw_node_id = "4"

try:
    value = int(raw_node_id, 0)
except ValueError:
    raise ValueError(f"IOT_NODE_ID inválido: {raw_node_id!r}. Use 0..255.")

if value < 0 or value > 255:
    raise ValueError(f"IOT_NODE_ID fora da faixa: {value}. Use 0..255.")

env.Append(CPPDEFINES=[("IOT_NODE_ID", value)])
print(f"Building iot-over-can ESP32 CAN node with IOT_NODE_ID={value}")
