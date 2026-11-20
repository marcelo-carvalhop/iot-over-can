Import("env")
import os

raw_node_id = os.environ.get("IOT_NODE_ID", "").strip()
if not raw_node_id:
    # Sem identificador explícito a placa sairia com um número qualquer, e
    # duas placas com o mesmo número disputam os mesmos identificadores CAN
    # e o mesmo ponto de acesso. O botão de compilar do editor não informa o
    # número; use os scripts, que também entregam a chave do enlace.
    raise ValueError(
        "IOT_NODE_ID não informado. Grave com ./Codigo/scripts/upload_esp32_can_node.sh <ID> <porta> "
        "(0 = Probe 00, 1..31 = Nodes) ou defina IOT_NODE_ID antes de chamar o PlatformIO."
    )

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

# Autenticação de origem dos comandos 0x304 e 0x305 (node-can/include/canauth.h): só com IOT_CAN_AUTH=1, que os
# scripts de compilação definem por --auth-can. Ligada, a Probe também precisa da chave de enlace.
auth_can = os.environ.get("IOT_CAN_AUTH", "").strip() == "1"
if auth_can:
    env.Append(CPPDEFINES=[("IOT_CAN_AUTH", 1)])
    print("iot-over-can: autenticação de origem dos comandos CAN ligada (IOT_CAN_AUTH=1)")

# Chave mestra do enlace wireless (64 caracteres hexadecimais). Nunca é
# versionada: Codigo/scripts/build_esp32_can_node.sh a lê de .env.local.
# Sem ela o firmware compila, mas o plano de dados permanece desabilitado.
raw_link_key = os.environ.get("IOT_LINK_MASTER_KEY", "").strip()
if raw_link_key:
    if len(raw_link_key) != 64 or any(c not in "0123456789abcdefABCDEF" for c in raw_link_key):
        raise ValueError("IOT_LINK_MASTER_KEY inválida: use 64 caracteres hexadecimais.")
    env.Append(CPPDEFINES=[("IOT_LINK_MASTER_KEY_HEX", env.StringifyMacro(raw_link_key))])
    print("iot-over-can: chave de enlace fornecida; plano de dados wireless habilitado")
elif auth_can:
    # Sem chave não há como etiquetar nem conferir: a opção não faria nada, e o firmware nem compilaria (static_assert).
    raise ValueError("IOT_CAN_AUTH=1 exige IOT_LINK_MASTER_KEY, também na Probe 00 (use --auth-can nos scripts).")
elif value != 0:
    # Um módulo sem chave associa sensores mas nunca abre o ponto de acesso (NO_KEY). O aviso
    # no meio da saída do build já passou despercebido (CH-056): recusa, a menos que se peça.
    if os.environ.get("IOT_ALLOW_NO_LINK_KEY", "").strip() != "1":
        raise ValueError(
            "IOT_LINK_MASTER_KEY ausente: o módulo não abriria o ponto de acesso dos sensores. Use "
            "./Codigo/scripts/provision_sensor_security.sh --add-link-key e os scripts de gravação, "
            "ou defina IOT_ALLOW_NO_LINK_KEY=1 para compilar sem chave de propósito."
        )
    print("iot-over-can: IOT_LINK_MASTER_KEY ausente (--sem-chave); plano de dados wireless desabilitado neste Node")
