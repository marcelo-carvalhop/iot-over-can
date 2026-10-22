"""Regressões para limites e integridade do reagrupamento CAN."""
from __future__ import annotations

from pico_tui.protocol.crc import crc32_ieee
from pico_tui.protocol.fragments import (
    FragmentReassembler, MAX_ACTIVE_TRANSFERS, MAX_FRAGMENT_BYTES,
    MAX_TRANSFER_BYTES,
)


def test_ordinary_fft_fragments_remain_complete():
    reassembler = FragmentReassembler()
    data = bytes(range(256)) * 4  # vetor de 1 KiB, maior que um quadro CAN
    crc = crc32_ieee(data)
    chunks = [data[i:i + 12] for i in range(0, len(data), 12)]
    for i, chunk in enumerate(chunks[::-1]):
        idx = len(chunks) - 1 - i
        result = reassembler.add(("FFT", 123), fragment_index=idx,
                                 fragment_count=len(chunks), data=chunk,
                                 expected_crc32=crc)
    assert result.status == "COMPLETE"
    assert result.payload == data


def test_rejects_absurd_fragment_count_and_oversized_single_fragment():
    assembler = FragmentReassembler()
    assert assembler.add((1,), fragment_index=0, fragment_count=1_000_000,
                         data=b"x").status == "FAILED"
    assert assembler.active_count == 0
    assert assembler.add((1,), fragment_index=0, fragment_count=2,
                         data=b"x" * (MAX_FRAGMENT_BYTES + 1)).status == "FAILED"
    assert assembler.active_count == 0


def test_rejects_crc_conflicts_between_fragments():
    assembler = FragmentReassembler()
    assert assembler.add((2,), fragment_index=0, fragment_count=2,
                         data=b"abc", expected_crc32=0x12345678).status == "PENDING"
    result = assembler.add((2,), fragment_index=1, fragment_count=2,
                           data=b"xyz", expected_crc32=0x87654321)
    assert result.status == "FAILED"
    assert "CRC" in result.reason
    assert assembler.active_count == 0


def test_rejects_oversized_total_without_unbounded_growth():
    assembler = FragmentReassembler()
    for index in range(MAX_TRANSFER_BYTES // MAX_FRAGMENT_BYTES):
        result = assembler.add((3,), fragment_index=index,
                               fragment_count=(MAX_TRANSFER_BYTES // MAX_FRAGMENT_BYTES) + 1,
                               data=b"x" * MAX_FRAGMENT_BYTES)
        assert result.status == "PENDING"
    result = assembler.add((3,), fragment_index=MAX_TRANSFER_BYTES // MAX_FRAGMENT_BYTES,
                           fragment_count=(MAX_TRANSFER_BYTES // MAX_FRAGMENT_BYTES) + 1,
                           data=b"!")
    assert result.status == "FAILED"
    assert assembler.active_count == 0


def test_limits_simultaneous_unfinished_transfers():
    assembler = FragmentReassembler()
    for key in range(MAX_ACTIVE_TRANSFERS):
        assert assembler.add((key,), fragment_index=0,
                             fragment_count=2, data=b"a").status == "PENDING"
    assert assembler.add(("overflow",), fragment_index=0,
                         fragment_count=2, data=b"x").status == "FAILED"
    assert assembler.active_count == MAX_ACTIVE_TRANSFERS
