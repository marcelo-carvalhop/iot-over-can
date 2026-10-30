"""Réplica do gerador de carga da Probe 00 (fault_injection.cpp).

Com a mesma semente, classe de prioridade e número de quadros, gera a mesma
sequência de identificadores e conteúdos e o mesmo ``seq_hash`` (FNV-1a de 32
bits sobre id, DLC e dados de cada quadro). A análise usa isso para comprovar
que o barramento recebeu exatamente a carga declarada no plano.
"""
from __future__ import annotations

from collections.abc import Iterator

ID_HIGH = 0x010
ID_MID = 0x1F0
ID_LOW = 0x7EF
CLASS_IDS = {"HIGH": ID_HIGH, "MID": ID_MID, "LOW": ID_LOW}
FNV_OFFSET = 2166136261
FNV_PRIME = 16777619
MASK32 = 0xFFFFFFFF


def _xorshift32(x: int) -> int:
    x ^= (x << 13) & MASK32
    x ^= x >> 17
    x ^= (x << 5) & MASK32
    return x & MASK32


def frames(seed: int, priority: str, count: int) -> Iterator[tuple[int, bytes]]:
    """Quadros (id, dados) na ordem em que a Probe os gera."""
    priority = priority.upper()
    if priority not in ("HIGH", "MID", "LOW", "MIX"):
        raise ValueError(f"prioridade desconhecida: {priority}")
    if seed == 0 and priority == "MIX":
        seed = 1  # a Probe faz o mesmo
    rng = seed & MASK32
    classes = ("HIGH", "MID", "LOW")
    for counter in range(count):
        cls = priority
        if priority == "MIX":
            rng = _xorshift32(rng)
            cls = classes[rng % 3]
        head = bytes(((counter >> 8) & 0xFF, counter & 0xFF))
        if seed == 0:
            body = bytes(0xAA if i & 1 else 0x55 for i in range(2, 8))
        else:
            rng = _xorshift32(rng)
            a = rng
            rng = _xorshift32(rng)
            b = rng
            body = a.to_bytes(4, "little") + bytes((b & 0xFF, (b >> 8) & 0xFF))
        yield CLASS_IDS[cls], head + body


def sequence_hash(seed: int, priority: str, count: int) -> int:
    h = FNV_OFFSET
    for can_id, data in frames(seed, priority, count):
        for byte in (can_id & 0xFF, can_id >> 8, len(data), *data):
            h = ((h ^ byte) * FNV_PRIME) & MASK32
    return h


def frame_bits(can_id: int, data: bytes) -> int:
    """Duração exata de um quadro CAN 2.0A no barramento (com preenchimento), como ioc_canbits.c."""
    bits = [0]  # SOF
    bits += [(can_id >> (10 - i)) & 1 for i in range(11)]
    bits += [0, 0, 0]  # RTR, IDE, r0
    bits += [(len(data) >> (3 - i)) & 1 for i in range(4)]
    for byte in data:
        bits += [(byte >> (7 - i)) & 1 for i in range(8)]
    crc = 0
    for bit in bits:
        crc_next = ((crc >> 14) & 1) ^ bit
        crc = (crc << 1) & 0x7FFF
        if crc_next:
            crc ^= 0x4599
    bits += [(crc >> (14 - i)) & 1 for i in range(15)]
    stuffed, run, last = 0, 0, None
    for bit in bits:
        stuffed += 1
        if bit == last:
            run += 1
        else:
            run, last = 1, bit
        if run == 5:
            stuffed += 1
            last, run = 1 - bit, 1
    return stuffed + 13  # delimitador do CRC, ACK, EOF e intervalo entre quadros
