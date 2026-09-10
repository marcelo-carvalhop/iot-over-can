from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class SequenceResult:
    classification: str
    lost: int = 0
    duplicate: bool = False
    out_of_order: bool = False


class SequenceTracker:
    """Rastreador de sequência de 16 bits independente por fluxo."""

    MODULUS = 1 << 16
    HALF_RANGE = 1 << 15

    def __init__(self) -> None:
        self._last: dict[tuple[object, ...], int] = {}

    def update(self, key: tuple[object, ...], sequence: int) -> SequenceResult:
        if not 0 <= sequence < self.MODULUS:
            raise ValueError("sequence deve estar em 0..65535")
        previous = self._last.get(key)
        if previous is None:
            self._last[key] = sequence
            return SequenceResult("FIRST")

        delta = (sequence - previous) % self.MODULUS
        if delta == 0:
            return SequenceResult("DUPLICATE", duplicate=True)
        if delta < self.HALF_RANGE:
            self._last[key] = sequence
            lost = max(0, delta - 1)
            return SequenceResult("GAP" if lost else "OK", lost=lost)
        return SequenceResult("OUT_OF_ORDER", out_of_order=True)

    def reset(self, key: tuple[object, ...] | None = None) -> None:
        if key is None:
            self._last.clear()
        else:
            self._last.pop(key, None)
