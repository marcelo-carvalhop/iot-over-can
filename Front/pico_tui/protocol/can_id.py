from __future__ import annotations

from dataclasses import dataclass

MAX_EXTENDED_CAN_ID = (1 << 29) - 1


@dataclass(frozen=True, slots=True)
class CanIdFields:
    priority: int
    domain: int
    parent_node_id: int
    child_id: int
    message_type: int

    @property
    def logical_id(self) -> str:
        return f"{self.parent_node_id:02d}.{self.child_id:02d}"


def decode_can_id(can_id: int) -> CanIdFields:
    if not 0 <= can_id <= MAX_EXTENDED_CAN_ID:
        raise ValueError("CAN ID deve possuir 29 bits")
    return CanIdFields(
        priority=(can_id >> 26) & 0x07,
        domain=(can_id >> 22) & 0x0F,
        parent_node_id=(can_id >> 14) & 0xFF,
        child_id=(can_id >> 8) & 0x3F,
        message_type=can_id & 0xFF,
    )


def encode_can_id(fields: CanIdFields) -> int:
    if not 0 <= fields.priority <= 7:
        raise ValueError("priority fora da faixa 0..7")
    if not 0 <= fields.domain <= 15:
        raise ValueError("domain fora da faixa 0..15")
    if not 0 <= fields.parent_node_id <= 255:
        raise ValueError("parent_node_id fora da faixa 0..255")
    if not 0 <= fields.child_id <= 63:
        raise ValueError("child_id fora da faixa 0..63")
    if not 0 <= fields.message_type <= 255:
        raise ValueError("message_type fora da faixa 0..255")
    return (
        (fields.priority << 26)
        | (fields.domain << 22)
        | (fields.parent_node_id << 14)
        | (fields.child_id << 8)
        | fields.message_type
    )
