from __future__ import annotations

import csv
import json
import time
from collections import deque
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import (
    LocalNodeTelemetryReceived,
    LogEvent,
    TelemetryReceived,
    WirelessCandidateReceived,
)


@dataclass(slots=True)
class LogEntry:
    timestamp_utc: str
    monotonic: float
    level: str
    source: str
    message: str
    data: dict[str, Any]


class LogManager:
    def __init__(
        self,
        bus: EventBus,
        *,
        session_id: str,
        directory: str | Path = "logs",
        enable_file: bool = True,
        max_entries: int = 5000,
    ) -> None:
        self.bus = bus
        self.session_id = session_id
        self.directory = Path(directory)
        self.enable_file = enable_file
        self.history: deque[LogEntry] = deque(maxlen=max_entries)
        self.telemetry_rows: deque[dict[str, Any]] = deque(maxlen=100000)
        self._file = None
        if enable_file:
            self.directory.mkdir(parents=True, exist_ok=True)
            self._file = (self.directory / f"session_{session_id}.jsonl").open("a", encoding="utf-8")
        bus.subscribe(LogEvent, self._on_log)
        bus.subscribe(TelemetryReceived, self._on_telemetry)
        bus.subscribe(LocalNodeTelemetryReceived, self._on_local_node_telemetry)
        bus.subscribe(WirelessCandidateReceived, self._on_wireless_candidate)

    async def _on_log(self, event: LogEvent) -> None:
        entry = LogEntry(
            timestamp_utc=datetime.now(timezone.utc).isoformat(),
            monotonic=time.monotonic(),
            level=event.level.upper(),
            source=event.source,
            message=event.message,
            data=event.data,
        )
        self.history.append(entry)
        self._write_jsonl({"type": "log", **asdict(entry)})

    async def _on_telemetry(self, event: TelemetryReceived) -> None:
        sample = event.sample
        row = {
            "timestamp_utc": datetime.now(timezone.utc).isoformat(),
            "logical_id": sample.logical_id,
            "sequence": sample.sequence,
            "mode": sample.mode.value,
            "acquisition": sample.acquisition_mode.value,
            "rms": sample.rms,
            "rms_unit": sample.rms_unit,
            "kurtosis": sample.kurtosis,
            "crest_factor": sample.crest_factor,
            "peak_frequency_hz": sample.peak_frequency_hz,
            "peak_amplitude": sample.peak_amplitude,
            "spectral_entropy": sample.spectral_entropy,
            "ppv_mm_s": sample.ppv_mm_s,
            "stalta_triggered": sample.stalta_triggered,
            "clipping": sample.clipping,
            "dtc_code": sample.dtc_code,
            "quality": sample.quality.value,
        }
        self.telemetry_rows.append(row)
        self._write_jsonl({"type": "telemetry", **row})

    async def _on_local_node_telemetry(self, event: LocalNodeTelemetryReceived) -> None:
        self._write_jsonl(
            {
                "type": "local_node_telemetry",
                "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                "node": event.parent_node_id,
                "profile": event.profile_id,
                "value": event.value,
                "round": event.round_number,
                "enabled": event.enabled,
            }
        )

    async def _on_wireless_candidate(self, event: WirelessCandidateReceived) -> None:
        self._write_jsonl(
            {
                "type": "wireless_candidate",
                "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                "reporter": event.reporter_node_id,
                "uuid": event.wireless_uuid,
                "profile": event.profile_id,
                "rssi_dbm": event.rssi_dbm,
                "protocol": event.protocol_version,
            }
        )

    def _write_jsonl(self, data: dict[str, Any]) -> None:
        if self._file is None:
            return
        self._file.write(json.dumps(data, ensure_ascii=False, default=str) + "\n")
        self._file.flush()

    def export_telemetry_csv(self, path: str | Path) -> Path:
        output = Path(path)
        output.parent.mkdir(parents=True, exist_ok=True)
        rows = list(self.telemetry_rows)
        if not rows:
            output.write_text("", encoding="utf-8")
            return output
        with output.open("w", newline="", encoding="utf-8") as file:
            writer = csv.DictWriter(file, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        return output

    def close(self) -> None:
        if self._file is not None:
            self._file.close()
            self._file = None
