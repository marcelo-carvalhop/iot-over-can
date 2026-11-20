"""Coleta de exemplos de eventos sonoros, com rótulo, para treinar um classificador no futuro.

O modo EVENTS do sensor acústico classifica cada evento por regras fixas
(``EVENT_CLASSES`` em ``acoustics.py``). Ajustar esses limites aos sons de uma
instalação exige exemplos reais, rotulados por quem estava lá. Este módulo só
coleta: com um rótulo ativo (``:rotulo palmas``), cada evento detectado vira uma
linha em ``exemplos_sonoros.csv``, com as características que o sensor mede e a
classe que o próprio firmware atribuiu. Sem rótulo ativo, nada é gravado. O
classificador do firmware não é alterado.

O arquivo é aberto em modo de acréscimo, com um único cabeçalho, para que várias
sessões alimentem o mesmo conjunto de treino.
"""
from __future__ import annotations

import csv
import re
import threading
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

FILE_NAME = "exemplos_sonoros.csv"

COLUMNS = (
    "timestamp_utc", "session_id", "sensor", "label", "firmware_class", "events_total",
    "event_peak_db", "event_sel_db", "event_duration_ms", "event_centroid_hz",
    "sta_lta_db", "background_db", "laeq_db", "lceq_db", "lzeq_db", "lzpeak_db", "lafmax_db",
    "centroid_hz", "flatness", "octave_db",
)

_LABEL = re.compile(r"^[\w.+-]{1,40}$", re.UNICODE)


def normalize_label(text: str) -> str | None:
    """Rótulo em minúsculas, sem espaços (``bate palma`` não vale; use ``bate-palma``), ou None se inválido."""

    label = text.strip().lower()
    return label if _LABEL.match(label) else None


class SoundExampleCollector:
    def __init__(self, directory: str | Path, session_id: str) -> None:
        self.path = Path(directory) / FILE_NAME
        self.session_id = session_id
        self.label: str | None = None
        self.count = 0
        self._lock = threading.Lock()

    @property
    def active(self) -> bool:
        return self.label is not None

    def set_label(self, label: str | None) -> None:
        self.label = label

    def status(self) -> str:
        if self.label is None:
            return "Coleta de exemplos sonoros desligada. Use :rotulo NOME para gravar os próximos eventos com um rótulo."
        return (f"Coleta ligada: cada evento detectado é gravado com o rótulo '{self.label}' em {self.path} "
                f"({self.count} nesta sessão). :rotulo off encerra.")

    def record(self, parent_node_id: int, child_id: int, metrics: Any, event: Any) -> bool:
        """Grava o evento se houver rótulo ativo. Devolve se gravou."""

        if self.label is None:
            return False
        octave = getattr(metrics, "octave_db", None) or ()
        row = {
            "timestamp_utc": datetime.now(timezone.utc).isoformat(),
            "session_id": self.session_id,
            "sensor": f"{parent_node_id:02d}.{child_id:02d}",
            "label": self.label,
            "firmware_class": getattr(event, "event_class", ""),
            "events_total": getattr(metrics, "events_total", None),
            "event_peak_db": getattr(event, "peak_db", None),
            "event_sel_db": getattr(event, "sel_db", None),
            "event_duration_ms": getattr(event, "duration_ms", None),
            "event_centroid_hz": getattr(event, "centroid_hz", None),
            "sta_lta_db": getattr(metrics, "sta_lta_db", None),
            "background_db": getattr(metrics, "background_db", None),
            "laeq_db": getattr(metrics, "laeq_db", None),
            "lceq_db": getattr(metrics, "lceq_db", None),
            "lzeq_db": getattr(metrics, "lzeq_db", None),
            "lzpeak_db": getattr(metrics, "lzpeak_db", None),
            "lafmax_db": getattr(metrics, "lafmax_db", None),
            "centroid_hz": getattr(metrics, "centroid_hz", None),
            "flatness": getattr(metrics, "flatness", None),
            "octave_db": ";".join("" if value is None else f"{value:.1f}" for value in octave),
        }
        with self._lock:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            new_file = not self.path.exists() or self.path.stat().st_size == 0
            with self.path.open("a", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=COLUMNS)
                if new_file:
                    writer.writeheader()
                writer.writerow({key: "" if value is None else value for key, value in row.items()})
            self.count += 1
        return True
