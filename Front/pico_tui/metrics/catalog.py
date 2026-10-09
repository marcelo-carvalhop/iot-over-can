"""Nomes das mensagens CAN do projeto, a partir do identificador.

Espelha Codigo/node-can/include/can_ids.h. Serve apenas para apresentação e
para a análise de tempo de resposta; nenhuma decisão de protocolo passa por
aqui.
"""
from __future__ import annotations

from dataclasses import dataclass

HEARTBEAT_ID = 0x100
LOCAL_SENSOR_ID = 0x200
TEST_LOAD_HIGH_ID = 0x010
TEST_LOAD_LOW_ID = 0x7EF
WIRELESS_DATA_BASE = 0x380


@dataclass(frozen=True, slots=True)
class MessageInfo:
    can_id: int
    name: str
    group: str
    node_id: int | None = None


def describe(can_id: int) -> MessageInfo:
    if can_id == TEST_LOAD_HIGH_ID:
        return MessageInfo(can_id, "Carga de ensaio, alta prioridade", "Ensaio")
    if can_id == TEST_LOAD_LOW_ID:
        return MessageInfo(can_id, "Carga de ensaio, baixa prioridade", "Ensaio")
    if can_id == 0x050:
        return MessageInfo(can_id, "Eleição: presença", "Coordenação")
    if can_id == 0x060:
        return MessageInfo(can_id, "Anúncio de líder", "Coordenação")
    if can_id == 0x080:
        return MessageInfo(can_id, "Controle e estado dos módulos", "Coordenação")
    if can_id == HEARTBEAT_ID:
        return MessageInfo(can_id, "Sinal de presença do líder", "Coordenação")
    if can_id == LOCAL_SENSOR_ID:
        return MessageInfo(can_id, "Sensor local dos módulos", "Dados")
    if 0x280 <= can_id <= 0x2BF:
        node, part = divmod(can_id - 0x280, 2)
        return MessageInfo(can_id, f"Descoberta BLE, Módulo {node:02d}, parte {'AB'[part]}", "Sem fio", node)
    if can_id in (0x300, 0x301):
        return MessageInfo(can_id, f"Comando de associação, parte {'AB'[can_id - 0x300]}", "Sem fio")
    if can_id in (0x302, 0x303):
        return MessageInfo(can_id, f"Reassociação pedida pelo líder, parte {'AB'[can_id - 0x302]}", "Sem fio")
    if can_id == 0x304:
        return MessageInfo(can_id, "Comando ao sensor sem fio", "Sem fio")
    if can_id == 0x305:
        return MessageInfo(can_id, "Comando de ensaio", "Ensaio")
    if can_id == 0x306:
        return MessageInfo(can_id, "Estado de ensaio", "Ensaio")
    if 0x320 <= can_id <= 0x35F:
        node, part = divmod(can_id - 0x320, 2)
        return MessageInfo(can_id, f"Estado de associação, Módulo {node:02d}, parte {'AB'[part]}", "Sem fio", node)
    if WIRELESS_DATA_BASE < can_id <= 0x39F:
        node = can_id - WIRELESS_DATA_BASE
        return MessageInfo(can_id, f"Dados de sensores sem fio, Módulo {node:02d}", "Dados", node)
    return MessageInfo(can_id, f"Identificador 0x{can_id:03X}", "Outros")
