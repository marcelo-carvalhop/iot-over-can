"""Coleta de exemplos de eventos sonoros rotulados (para treinar um classificador no futuro)."""
from __future__ import annotations

import csv

from pico_tui.core.models import AcousticEvent
from pico_tui.sound_examples import COLUMNS, FILE_NAME, SoundExampleCollector, normalize_label


class Metrics:
    events_total = 7
    sta_lta_db = 14.5
    background_db = 41.0
    laeq_db = 63.2
    lceq_db = 64.0
    lzeq_db = 64.5
    lzpeak_db = 101.3
    lafmax_db = 88.8
    centroid_hz = 2100.0
    flatness = 0.31
    octave_db = [30.0, 35.5, None, 50.25]


EVENT = AcousticEvent(event_class="IMPACT", peak_db=99.5, sel_db=80.1, duration_ms=120, centroid_hz=1800.0)


def read_rows(path):
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def test_nothing_is_written_without_an_active_label(tmp_path):
    collector = SoundExampleCollector(tmp_path, "s1")
    assert collector.record(2, 1, Metrics(), EVENT) is False
    assert not (tmp_path / FILE_NAME).exists() and "desligada" in collector.status()


def test_events_are_written_with_label_features_and_firmware_class(tmp_path):
    collector = SoundExampleCollector(tmp_path, "s1")
    collector.set_label("bate-palma")
    assert collector.record(2, 1, Metrics(), EVENT) is True
    assert collector.record(2, 1, Metrics(), EVENT) is True
    rows = read_rows(tmp_path / FILE_NAME)
    assert len(rows) == 2 and collector.count == 2
    row = rows[0]
    assert tuple(row) == COLUMNS
    assert row["label"] == "bate-palma" and row["firmware_class"] == "IMPACT" and row["sensor"] == "02.01"
    assert row["session_id"] == "s1" and row["events_total"] == "7"
    assert float(row["event_peak_db"]) == 99.5 and row["event_duration_ms"] == "120"
    assert row["octave_db"] == "30.0;35.5;;50.2" or row["octave_db"] == "30.0;35.5;;50.3"  # arredondamento do formato .1f
    assert "bate-palma" in collector.status() and "2 nesta sessão" in collector.status()


def test_header_is_written_once_across_sessions(tmp_path):
    for session in ("a", "b"):
        collector = SoundExampleCollector(tmp_path, session)
        collector.set_label("porta")
        collector.record(1, 1, Metrics(), EVENT)
    text = (tmp_path / FILE_NAME).read_text(encoding="utf-8")
    assert text.count("timestamp_utc") == 1
    assert [row["session_id"] for row in read_rows(tmp_path / FILE_NAME)] == ["a", "b"]


def test_label_can_be_turned_off(tmp_path):
    collector = SoundExampleCollector(tmp_path, "s1")
    collector.set_label("porta")
    collector.set_label(None)
    assert collector.active is False and collector.record(1, 1, Metrics(), EVENT) is False


def test_labels_are_normalized_and_validated():
    assert normalize_label("  Bate-Palma ") == "bate-palma"
    assert normalize_label("máquina_1") == "máquina_1"
    assert normalize_label("bate palma") is None and normalize_label("") is None
    assert normalize_label("x" * 41) is None and normalize_label("a;b") is None


def test_collector_hooked_to_the_controller_records_each_detected_event(tmp_path):
    """Com a gravação da bancada: um exemplo por evento, com a classe que o firmware deu."""

    import asyncio
    from pathlib import Path

    from pico_tui.core.event_bus import EventBus
    from pico_tui.core.state_store import StateStore
    from pico_tui.protocol.router import DecoderRouter
    from pico_tui.services.controller import DomainController

    replay = Path(__file__).resolve().parents[1] / "replays" / "acoustic_sensor.log"
    bus, state = EventBus(), StateStore()
    controller = DomainController(bus, state)
    collector = SoundExampleCollector(tmp_path, "teste")
    collector.set_label("bancada")
    controller.on_acoustic_event = collector.record
    router = DecoderRouter(bus)

    async def run() -> None:
        for raw in replay.read_text(encoding="utf-8").splitlines():
            if raw.startswith("@"):
                await router.decode(raw.partition(" ")[2])

    asyncio.run(run())
    sensor = state.snapshot().nodes[2].sensors[1]
    rows = read_rows(tmp_path / FILE_NAME)
    assert len(rows) == len(sensor.acoustic_events) >= 3
    assert {row["firmware_class"] for row in rows} >= {"CRACK", "TONAL"}
    assert all(row["label"] == "bancada" and row["sensor"] == "02.01" for row in rows)
    assert all(row["octave_db"] for row in rows)          # o espectro por oitava acompanha cada exemplo


def test_a_failing_collector_does_not_stop_the_telemetry(tmp_path):
    import asyncio
    from pathlib import Path

    from pico_tui.core.event_bus import EventBus
    from pico_tui.core.state_store import StateStore
    from pico_tui.protocol.router import DecoderRouter
    from pico_tui.services.controller import DomainController

    def broken(*_args):
        raise OSError("disco cheio")

    replay = Path(__file__).resolve().parents[1] / "replays" / "acoustic_sensor.log"
    bus, state = EventBus(), StateStore()
    DomainController(bus, state).on_acoustic_event = broken
    router = DecoderRouter(bus)

    async def run() -> None:
        for raw in replay.read_text(encoding="utf-8").splitlines():
            if raw.startswith("@"):
                await router.decode(raw.partition(" ")[2])

    asyncio.run(run())
    sensor = state.snapshot().nodes[2].sensors[1]
    assert len(sensor.acoustic_events) >= 3
    assert any("Falha ao gravar o exemplo sonoro" in item.message for item in sensor.incidents)
