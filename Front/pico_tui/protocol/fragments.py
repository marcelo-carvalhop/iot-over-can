from __future__ import annotations

import time
from dataclasses import dataclass, field

from pico_tui.protocol.crc import crc32_ieee


@dataclass(slots=True)
class _Transfer:
    fragment_count: int
    expected_crc32: int | None
    created: float = field(default_factory=time.monotonic)
    fragments: dict[int, bytes] = field(default_factory=dict)


@dataclass(frozen=True, slots=True)
class ReassemblyResult:
    status: str
    payload: bytes | None = None
    reason: str = ""


class FragmentReassembler:
    def __init__(self, timeout_seconds: float = 5.0) -> None:
        self.timeout_seconds = timeout_seconds
        self._transfers: dict[tuple[object, ...], _Transfer] = {}

    def add(
        self,
        key: tuple[object, ...],
        *,
        fragment_index: int,
        fragment_count: int,
        data: bytes,
        expected_crc32: int | None = None,
    ) -> ReassemblyResult:
        self.expire()
        if fragment_count <= 0:
            return ReassemblyResult("FAILED", reason="fragment_count inválido")
        if not 0 <= fragment_index < fragment_count:
            return ReassemblyResult("FAILED", reason="fragment_index inválido")

        transfer = self._transfers.get(key)
        if transfer is None:
            transfer = _Transfer(fragment_count, expected_crc32)
            self._transfers[key] = transfer
        elif transfer.fragment_count != fragment_count:
            self._transfers.pop(key, None)
            return ReassemblyResult("FAILED", reason="fragment_count conflitante")

        if fragment_index in transfer.fragments:
            if transfer.fragments[fragment_index] == data:
                return ReassemblyResult("DUPLICATE")
            self._transfers.pop(key, None)
            return ReassemblyResult("FAILED", reason="fragmento duplicado divergente")

        transfer.fragments[fragment_index] = bytes(data)
        if len(transfer.fragments) != transfer.fragment_count:
            return ReassemblyResult("PENDING")

        payload = b"".join(transfer.fragments[index] for index in range(transfer.fragment_count))
        self._transfers.pop(key, None)
        if transfer.expected_crc32 is not None and crc32_ieee(payload) != transfer.expected_crc32:
            return ReassemblyResult("FAILED", reason="CRC32 mismatch")
        return ReassemblyResult("COMPLETE", payload=payload)

    def expire(self) -> list[tuple[object, ...]]:
        now = time.monotonic()
        expired = [
            key
            for key, transfer in self._transfers.items()
            if now - transfer.created >= self.timeout_seconds
        ]
        for key in expired:
            self._transfers.pop(key, None)
        return expired

    def clear(self) -> None:
        self._transfers.clear()

    @property
    def active_count(self) -> int:
        return len(self._transfers)
