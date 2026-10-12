"""Robustez: gravação de sessões, falhas internas visíveis e linhas corrompidas."""
from __future__ import annotations

import asyncio
import random
from pathlib import Path

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import LogEvent, RawLineReceived
from pico_tui.core.state_store import StateStore
from pico_tui.protocol.router import DecoderRouter
from pico_tui.services.controller import DomainController
from pico_tui.services.recorder import SessionRecorder, recorded_mode, redact

REPLAYS = Path(__file__).resolve().parents[1] / "replays"


def test_recorder_writes_replay_format_without_secrets(tmp_path: Path) -> None:
    recorder = SessionRecorder(tmp_path / "s.log", port="/dev/ttyACM0", mode="sensor")
    recorder.line("STATUS PROFILE=ACOUSTIC AUTH=LOCKED\r\n")
    recorder.line("")
    recorder.command("AUTH UNLOCK 0x1234")
    recorder.line("EDGE> AUTH UNLOCK 0x1234")
    recorder.line("PROV EDGE_LINK_DEVICE_KEY=00112233 TOKEN=abc OK")
    recorder.close()
    recorder.line("depois de fechar")  # ignorado sem erro
    lines = (tmp_path / "s.log").read_text(encoding="utf-8").splitlines()
    assert lines[0].startswith("# iot-over-can") and "modo=sensor" in lines[0]
    assert lines[1].startswith("@") and lines[1].endswith("STATUS PROFILE=ACOUSTIC AUTH=LOCKED")
    assert "comando: AUTH UNLOCK ***" in lines[2]
    text = "\n".join(lines)
    assert "0x1234" not in text and "00112233" not in text and "abc" not in text
    assert len(lines) == 5
    assert recorded_mode(lines) == "sensor"
    assert recorded_mode(["# cenario=formation", "@1 x"]) == "gateway"
    assert redact("AUTH=LOCKED WIFI_PROVISIONED=NO") == "AUTH=LOCKED WIFI_PROVISIONED=NO"


def test_handler_failure_becomes_one_visible_error() -> None:
    async def scenario() -> list[LogEvent]:
        bus = EventBus()
        logs: list[LogEvent] = []

        async def broken(_event) -> None:
            raise RuntimeError("defeito de teste")

        async def keep(event: LogEvent) -> None:
            logs.append(event)

        bus.subscribe(RawLineReceived, broken)
        bus.subscribe(LogEvent, keep)
        for _ in range(5):
            await bus.publish(RawLineReceived("x"))
        return logs

    logs = asyncio.run(scenario())
    assert [(log.level, log.source) for log in logs] == [("ERROR", "INTERNO")]
    assert "defeito de teste" in logs[0].message


def _domain():
    bus = EventBus()
    state = StateStore()
    DomainController(bus, state)
    router = DecoderRouter(bus, requested_mode="gateway")
    problems: list[LogEvent] = []

    async def watch(event: LogEvent) -> None:
        if event.level == "ERROR":
            problems.append(event)

    bus.subscribe(LogEvent, watch)
    return state, router, problems


def test_out_of_range_sequence_is_a_warning_not_a_lost_sample() -> None:
    state, router, problems = _domain()
    line = "TEL NODE=1 CHILD=1 SEQ={} MODE=ROTATING ACQ=POLLING WIN=512 RATE_EFF_HZ=1000.00 RMS=0.1200"

    async def scenario() -> None:
        await router.decode(line.format(-1))
        await router.decode(line.format(7))

    asyncio.run(scenario())
    sensor = state.snapshot().nodes[1].sensors[1]
    assert sensor.latest_telemetry is not None and sensor.latest_telemetry.sequence == 7
    assert state.snapshot().network.parse_errors == 1
    assert problems == []


def _mutate(line: str, rng: random.Random) -> str:
    extremes = ["", "-", "nan", "inf", "-1", "4294967296", "1e309", "abc", "\x00", ",,,"]
    tokens = line.split()
    kind = rng.randrange(5)
    if kind == 0 and line:
        return line[: rng.randrange(len(line))]
    if kind == 1 and line:
        index = rng.randrange(len(line))
        return line[:index] + chr(rng.choice([0, 0x7F, 0xE9, rng.randrange(32, 127)])) + line[index + 1 :]
    if not tokens:
        return line
    index = rng.randrange(len(tokens))
    if kind == 2:
        del tokens[index]
    elif "=" in tokens[index]:
        tokens[index] = tokens[index].split("=", 1)[0] + "=" + rng.choice(extremes)
    else:
        tokens.insert(index, tokens[index])
    return " ".join(tokens)


def test_corrupted_probe_lines_never_break_decoding() -> None:
    """Fuzz curto e determinístico; a versão longa encontrou o caso do SEQ negativo."""

    rng = random.Random(2026)
    lines = [
        raw.partition(" ")[2]
        for path in sorted(REPLAYS.glob("*.log"))
        for raw in path.read_text(encoding="utf-8").splitlines()
        if raw.startswith("@")
    ]
    sample = rng.sample(lines, 400)
    state, router, problems = _domain()

    async def scenario() -> None:
        for line in sample:
            for candidate in (line, _mutate(line, rng), _mutate(line, rng)):
                await router.decode(candidate)  # exceção aqui reprova o teste

    asyncio.run(scenario())
    assert [event.message for event in problems if event.source == "INTERNO"] == []
    assert state.snapshot().nodes  # as linhas íntegras continuaram valendo


# --------------------------------------------------------------------------
# Alarmes por limiar
# --------------------------------------------------------------------------


def _vibration(rms: float, seq: int = 1):
    from pico_tui.core.models import TelemetrySample

    return TelemetrySample(parent_node_id=3, child_id=1, sequence=seq, rms=rms, rms_unit="g")


def test_alarm_lights_clears_with_hysteresis_and_reaches_the_condition() -> None:
    from pico_tui import presentation as pres

    store = StateStore()
    store.load_alarm_limits({"03.01": {"rms": 0.5, "desconhecida": 1.0}})
    store.update_telemetry(_vibration(0.40))
    assert store.pop_alarm_transitions() == []
    store.update_telemetry(_vibration(0.60, 2))
    [(ident, message, lit)] = store.pop_alarm_transitions()
    assert (ident, lit) == ("03.01", True) and "acima do limite de 0,5 g" in message
    sensor = store.find_sensor("03.01")
    assert sensor.alarm_limits == {"rms": 0.5}
    condition = pres.sensor_condition(sensor)
    assert condition.level == pres.Level.ATTENTION and condition.reasons[0].startswith("Alarme: RMS")
    store.update_telemetry(_vibration(0.49, 3))      # dentro da histerese (5 %): continua aceso
    assert store.pop_alarm_transitions() == []
    store.update_telemetry(_vibration(0.47, 4))
    [(_, message, lit)] = store.pop_alarm_transitions()
    assert not lit and message.startswith("Alarme normalizado")
    assert [incident.kind for incident in store.find_sensor("03.01").incidents].count("ALARME") == 2


def test_alarm_limits_apply_immediately_and_survive_in_preferences(tmp_path: Path) -> None:
    from pico_tui.preferences import Preferences, load_preferences, save_preferences

    store = StateStore()
    store.update_telemetry(_vibration(0.30))
    assert store.set_alarm_limits("03.01", {"rms": 0.2, "ppv": float("nan")}) == {"rms": 0.2}
    assert [lit for *_rest, lit in store.pop_alarm_transitions()] == [True]
    store.set_alarm_limits("03.01", {})
    assert [lit for *_rest, lit in store.pop_alarm_transitions()] == [False]
    assert store.alarm_limits() == {}

    path = tmp_path / "tui.json"
    save_preferences(Preferences(alarm_limits={"03.01": {"laeq": 85.0}}), path)
    prefs, warning = load_preferences(path)
    assert warning == "" and prefs.alarm_limits == {"03.01": {"laeq": 85.0}}
