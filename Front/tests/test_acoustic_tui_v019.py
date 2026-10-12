"""TUI v0.19: sensor acústico INMP441.

Cobre as análises (exposição NR-15/NHO-01, curvas NC, conversa, tacômetro
acústico, eventos, calibração), os decodificadores (Probe 00 e console USB),
o estado a partir da gravação da bancada virtual e as telas.
"""
from __future__ import annotations

import asyncio
import time
from pathlib import Path

import pytest

from pico_tui import acoustic_view, acoustics
from pico_tui.app import PicoTuiApp
from pico_tui.command_catalog import actions_for
from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import SpectrumReceived, TelemetryReceived
from pico_tui.core.models import AcousticEvent, AcousticMetrics, AcquisitionMode, SensorMode, SpectrumSample
from pico_tui.core.state_store import StateStore
from pico_tui.dialogs import ConfigScreen
from pico_tui.protocol.router import DecoderRouter
from pico_tui.screens import FftScreen, SensorScreen
from pico_tui.security import SecurityManager
from pico_tui.services.controller import DomainController
from pico_tui.spectrum import acoustic_spectrum_chart, acoustic_spectrum_peaks
from tests.conftest import settle, wait_for
from tests.fake_acoustic_firmware import FakeAcousticFirmware

ROOT = Path(__file__).resolve().parents[2]
REPLAY = ROOT / "Front/replays/acoustic_sensor.log"

PROBE_LEVEL = (
    "TEL NODE=2 CHILD=1 SEQ=3 GW_TS_MS=13705 PROFILE=ACOUSTIC MODE=LEVEL ACQ=I2S WIN=1024 RATE_EFF_HZ=16000 "
    "INTERVAL_MS=1000 COVERAGE_PCT=100 CPU_PCT=35 SPECTRUM_VALID=YES OVERLOAD=NO MIC_FAULT=NO LAEQ_DB=87.10 "
    "LCEQ_DB=91.30 LZEQ_DB=92.20 LZPEAK_DB=103.50 LAF_DB=86.75 LAFMAX_DB=90.00 LAFMIN_DB=84.50 CREST=4.12 "
    "KURT=0.35 PEAK_HZ=1000.0 CENTROID_HZ=1480 FLATNESS=0.1830 OCT_DB=72.7,78.9,81.8,83.2,84.0,84.7,82.1,76.8,-,- "
    "LEQ_T_DB=87.90 L10_DB=89.15 L50_DB=87.60 L90_DB=86.50 DOSE_NR15_PCT=0.4 DOSE_NHO01_PCT=0.6 ELAPSED_S=120 "
    "DOSE_LIMIT=NO BATT_PCT=255 BATT_MV=65535 DTC=0x0000 DTC_COUNT=0"
)


def _domain():
    bus = EventBus()
    state = StateStore()
    DomainController(bus, state)
    router = DecoderRouter(bus)
    seen: dict[type, list] = {}

    async def remember(event) -> None:
        seen.setdefault(type(event), []).append(event)

    bus.subscribe(TelemetryReceived, remember)
    bus.subscribe(SpectrumReceived, remember)
    return bus, state, router, seen


# --------------------------------------------------------------------------
# 1. Análises
# --------------------------------------------------------------------------


def test_permissible_exposure_follows_nr15_and_nho01_exchange_rates() -> None:
    hour = 3600.0
    assert acoustics.permissible_seconds(85.0, acoustics.NR15_EXCHANGE_DB) == pytest.approx(8 * hour)
    assert acoustics.permissible_seconds(90.0, acoustics.NR15_EXCHANGE_DB) == pytest.approx(4 * hour)
    assert acoustics.permissible_seconds(100.0, acoustics.NR15_EXCHANGE_DB) == pytest.approx(1 * hour)
    assert acoustics.permissible_seconds(88.0, acoustics.NHO01_EXCHANGE_DB) == pytest.approx(4 * hour)
    assert acoustics.permissible_seconds(79.9, acoustics.NHO01_EXCHANGE_DB) is None   # abaixo do limiar
    assert acoustics.permissible_seconds(116.0, acoustics.NR15_EXCHANGE_DB) == 0.0     # não permitido


def test_dose_projection_lex_and_nr15_average_level() -> None:
    assert acoustics.projected_dose(25.0, 7200) == pytest.approx(100.0)
    assert acoustics.seconds_to_full_dose(25.0, 7200) == pytest.approx(21600.0)
    assert acoustics.seconds_to_full_dose(120.0, 7200) == 0.0
    assert acoustics.lex_8h(88.0, 4 * 3600) == pytest.approx(84.99, abs=0.01)
    assert acoustics.nr15_average_level(100.0) == pytest.approx(85.0)
    assert acoustics.nr15_average_level(200.0) == pytest.approx(90.0, abs=0.01)


@pytest.mark.parametrize(
    ("dose", "label"),
    [(10.0, "Aceitável"), (60.0, "Acima do nível de ação"), (90.0, "Região de incerteza"),
     (100.0, "Região de incerteza"), (150.0, "Acima do limite de exposição")],
)
def test_nho01_decision_criterion(dose: float, label: str) -> None:
    decision = acoustics.nho01_decision(dose)
    assert decision is not None and decision.label == label


def test_nc_rating_uses_the_tangent_curve_and_tolerates_missing_8khz() -> None:
    nc40 = [None, 64, 56, 50, 45, 41, 39, 38, 37, None]
    rating = acoustics.nc_rating(nc40)
    assert rating is not None and rating.value == 40 and not rating.missing_hz
    louder_rumble = [None, 71, 56, 50, 45, 41, 39, 38, 37, None]   # 63 Hz na curva NC-50
    rating = acoustics.nc_rating(louder_rumble)
    assert rating.value == 50 and rating.governing_hz == 63.0
    partial = [None, 64, 56, 50, 45, 41, 39, 38, None, None]       # 16 kHz de amostragem
    rating = acoustics.nc_rating(partial)
    assert rating.value == 40 and rating.missing_hz == (8000.0,)
    assert acoustics.nc_rating([None, 64, 56, None, 45, 41, 39, 38, 37, None]) is None
    factory = acoustics.nc_rating([None, 90, 88, 86, 85, 84, 82, 80, None, None])
    assert factory.above_table and factory.value == 70 and factory.label == "acima de NC-70"


def test_speech_interference_and_conversation_distance() -> None:
    quiet = [None, None, None, None, 40.0, 40.0, 40.0, 40.0, None, None]
    loud = [None, None, None, None, 75.0, 75.0, 75.0, 75.0, None, None]
    assert acoustics.speech_interference_level(quiet) == pytest.approx(40.0)
    assert acoustics.conversation_distance_m(57.5) == pytest.approx(1.0)
    assert acoustics.conversation_distance_m(63.5) == pytest.approx(0.5, rel=0.01)
    assert acoustics.speech_summary(40.0) == "Conversa em voz normal até cerca de 7,5 m"
    assert "sem restrição" in acoustics.speech_summary(35.0)
    assert acoustics.speech_summary(75.0) == "Voz normal não basta além de 13 cm; em voz muito alta, até cerca de 53 cm"
    assert "impraticável" in acoustics.speech_summary(95.0)
    assert acoustics.speech_summary(acoustics.speech_interference_level(loud)) is not None


def test_tachometer_search_range_follows_resolution() -> None:
    low, high = acoustics.f0_range(16000.0, 1024)
    assert low == pytest.approx(62.5) and high == pytest.approx(1593.75)
    low, high = acoustics.f0_range(8000.0, 2048)
    assert low == pytest.approx(20.0) and high == pytest.approx(798.4, abs=0.1)
    assert acoustics.f0_range(None, 1024) is None


def test_acoustic_tachometer_and_induction_motor_hypothesis() -> None:
    metrics = AcousticMetrics(f0_hz=29.6, harmonics=6)
    tach = acoustics.tachometer(metrics, [29.6, 29.61, 29.59])
    assert tach.rpm == pytest.approx(1776.0)
    assert [motor.poles for motor in tach.motors] == [4]
    assert tach.motors[0].slip_pct == pytest.approx(1.333, abs=0.01)
    assert tach.spread_pct is not None and tach.spread_pct < 0.1
    hum = acoustics.tachometer(AcousticMetrics(f0_hz=120.2))
    assert hum.mains_multiple == 2 and hum.motors == []
    texts = " ".join(item.text for item in acoustics.machine_insights(AcousticMetrics(f0_hz=120.0), 3.0))
    assert "rede elétrica" in texts


def test_machine_anomaly_levels_follow_the_threshold() -> None:
    ready = AcousticMetrics(f0_hz=0.0, baseline="READY", baseline_pct=100, anomaly_z=5.1, anomaly=True)
    items = acoustics.machine_insights(ready, 4.0)
    assert any(item.level == 2 and item.title == "Anomalia" for item in items)
    calm = AcousticMetrics(baseline="READY", anomaly_z=1.0, anomaly=False)
    assert all(item.level == 0 for item in acoustics.machine_insights(calm, 4.0) if item.title == "Anomalia")
    learning = AcousticMetrics(baseline="LEARNING", baseline_pct=40)
    assert any("40 %" in item.text for item in acoustics.machine_insights(learning, 4.0))


def test_character_insights_flag_low_frequency_impulsive_and_leaks() -> None:
    metrics = AcousticMetrics(
        laeq_db=60.0, lceq_db=82.0, lzeq_db=84.0, lzpeak_db=105.0, kurtosis=0.2,
        l10_db=70.0, l90_db=55.0, flatness=0.55, centroid_hz=6200.0,
        octave_db=[50, 52, 50, 48, 46, 47, 52, 58, 60, None],
    )
    titles = {item.title: item for item in acoustics.character_insights(metrics)}
    assert titles["Baixa frequência"].level == 1          # LCeq - LAeq = 22 dB
    assert titles["Impulsividade"].level == 1             # pico 21 dB acima do nível eficaz
    assert "variável" in titles["Variabilidade"].text
    assert "vazamento" in titles["Caráter espectral"].text
    assert titles["Critério NC"].text.startswith("NC-")
    fault = AcousticMetrics(mic_fault=True, overload=True)
    assert [item.level for item in acoustics.character_insights(fault)][:2] == [2, 1]


def test_event_statistics_rate_and_combined_sel() -> None:
    now = time.time()
    events = [
        AcousticEvent("IMPACT", peak_db=90.0, sel_db=70.0, received_wall_time=now - 120),
        AcousticEvent("CRACK", peak_db=95.0, sel_db=70.0, received_wall_time=now - 60),
        AcousticEvent("IMPACT", peak_db=88.0, sel_db=None, received_wall_time=now - 30),
    ]
    stats = acoustics.event_statistics(events, now=now)
    assert stats.total == 3 and stats.per_class == {"IMPACT": 2, "CRACK": 1}
    assert stats.loudest.event_class == "CRACK"
    assert stats.combined_sel_db == pytest.approx(73.01, abs=0.01)   # dois eventos de 70 dB
    assert stats.rate_per_min == pytest.approx(1.5)
    assert acoustics.event_label("TONAL") == "Tonal" and acoustics.event_label(None) == "Não classificado"


def test_calibration_gain_from_calibrator_readings() -> None:
    result = acoustics.calibration([93.0, 93.1, 92.9], 94.0, 1.0)
    assert result is not None and result.stable
    assert result.new_gain == pytest.approx(10.0 ** (1.0 / 20.0), rel=1e-4)
    unstable = acoustics.calibration([93.0, 94.2, 92.9], 94.0, 1.0)
    assert unstable is not None and not unstable.stable
    assert acoustics.calibration([93.0, 93.1], 94.0, 1.0) is None


def test_bar_cells_and_band_labels() -> None:
    assert acoustics.bar_cells(70.0, 40.0, 90.0, 50) == 30
    assert acoustics.bar_cells(None, 40.0, 90.0, 50) == 0
    assert acoustics.bar_cells(120.0, 40.0, 90.0, 50) == 50
    assert acoustics.band_label(31.5) == "31,5 Hz" and acoustics.band_label(16000.0) == "16 kHz"
    assert acoustics.pa_to_db(1.0) == pytest.approx(93.98, abs=0.01)
    assert acoustics.pa_to_db(0.0) is None


# --------------------------------------------------------------------------
# 2. Decodificadores e estado
# --------------------------------------------------------------------------


def test_probe_line_becomes_acoustic_sample_without_vibration_fields() -> None:
    async def scenario() -> None:
        _bus, state, router, seen = _domain()
        await router.decode(PROBE_LEVEL)
        sample = seen[TelemetryReceived][0].sample
        assert (sample.parent_node_id, sample.child_id, sample.sequence) == (2, 1, 3)
        assert sample.mode == SensorMode.LEVEL and sample.acquisition_mode == AcquisitionMode.I2S
        assert sample.rms is None and sample.clipping is None and sample.stalta_triggered is None
        metrics = sample.acoustic
        assert metrics.laeq_db == pytest.approx(87.1) and metrics.lceq_db == pytest.approx(91.3)
        assert metrics.octave_db[0] == pytest.approx(72.7) and metrics.octave_db[8] is None
        assert metrics.dose_nho01_pct == pytest.approx(0.6) and metrics.elapsed_s == 120
        sensor = state.snapshot().nodes[2].sensors[1]
        assert sensor.profile_id == "ACOUSTIC"
        assert sensor.configuration.sample_rate_effective_hz == 16000.0

    asyncio.run(scenario())


def test_wireless_config_with_profile_keeps_acoustic_mode_names() -> None:
    async def scenario() -> None:
        _bus, state, router, _seen = _domain()
        await router.decode(PROBE_LEVEL)
        await router.decode(
            "[GW] WIRELESS_CONFIG node=2 child=1 status=APPLIED mode=MACHINE rate_req_hz=48000.00 "
            "rate_eff_hz=48003.07 window=BLACKMAN window_size=2048 stalta=3.000 gain=1.1220 profile=ACOUSTIC"
        )
        sensor = state.snapshot().nodes[2].sensors[1]
        cfg = sensor.configuration
        assert sensor.profile_id == "ACOUSTIC" and sensor.sensor_mode == SensorMode.MACHINE
        assert cfg.mode == SensorMode.MACHINE and cfg.window_size == 2048 and cfg.window_type == "BLACKMAN"
        assert cfg.sample_rate_requested_hz == 48000.0 and cfg.stalta_threshold == 3.0
        assert cfg.transaction_state == "APPLIED"
        # Uma telemetria sem a taxa pedida não apaga o que a configuração informou.
        await router.decode(PROBE_LEVEL.replace("MODE=LEVEL", "MODE=MACHINE"))
        assert state.snapshot().nodes[2].sensors[1].configuration.sample_rate_requested_hz == 48000.0

    asyncio.run(scenario())


def test_direct_console_lines_for_the_acoustic_firmware() -> None:
    async def scenario() -> None:
        _bus, state, router, seen = _domain()
        fw = FakeAcousticFirmware()
        await router.decode("VERSION PROTOCOL=6 NODE_UUID=0xE6616408432B7A11 PROFILE=ACOUSTIC CHIP=RP2040 BOARD=pico_w")
        await router.decode(fw._status_line().strip())
        await router.decode(fw._telemetry_line().strip())
        await router.decode(fw._fft_line().strip())
        sensor = state.snapshot().nodes[1].sensors[1]
        assert sensor.profile_id == "ACOUSTIC" and sensor.acquisition_mode == AcquisitionMode.I2S
        assert sensor.health.microphone_state == "OK" and sensor.health.chip == "RP2040"
        assert sensor.configuration.stalta_threshold == 8.0
        spectrum = seen[SpectrumReceived][0].sample
        assert spectrum.sample_rate_hz == 16000.0 and spectrum.fft_size == 128 and spectrum.magnitude_unit == "PA"
        assert seen[TelemetryReceived][0].sample.acoustic.dose_nr15_pct == pytest.approx(12.5)

    asyncio.run(scenario())


def _replay(state_router):
    _bus, state, router, seen = state_router

    async def run() -> None:
        for raw in REPLAY.read_text(encoding="utf-8").splitlines():
            if raw.startswith("@"):
                await router.decode(raw.partition(" ")[2])

    asyncio.run(run())
    return state, seen


def test_bench_replay_drives_every_acoustic_mode() -> None:
    state, seen = _replay(_domain())
    sensor = state.snapshot().nodes[2].sensors[1]
    assert sensor.profile_id == "ACOUSTIC" and sensor.wireless_uuid.upper() == "0XE6616408432B7A11"
    modes = {event.sample.mode for event in seen[TelemetryReceived] if event.sample.acoustic is not None}
    assert modes == {SensorMode.LEVEL, SensorMode.MACHINE, SensorMode.EVENTS}
    # Eventos: cada fim de evento vira um registro, com a classe informada.
    classes = [event.event_class for event in sensor.acoustic_events]
    assert len(classes) >= 3 and {"CRACK", "TONAL"} <= set(classes)
    kinds = [incident.message for incident in sensor.incidents]
    assert any("Assinatura sonora da máquina fora do normal" in text for text in kinds)
    assert any("Assinatura de referência aprendida" in text for text in kinds)
    assert any(text.startswith("Evento sonoro: Estalo") for text in kinds)
    # O espectro pedido chega reduzido a 64 faixas, recuperado em Pa.
    spectra = [event.sample for event in seen[SpectrumReceived]]
    assert len(spectra) == 1 and len(spectra[0].magnitudes) == 64
    assert max(spectra[0].magnitudes) == pytest.approx(0.254, rel=1e-3)
    # Tacômetro do modo MACHINE com a fundamental da gravação.
    machine = next(e.sample.acoustic for e in seen[TelemetryReceived] if e.sample.mode == SensorMode.MACHINE)
    tach = acoustics.tachometer(machine)
    assert tach.rpm == pytest.approx(1776.0, abs=1.0) and tach.motors[0].poles == 4


def test_acoustic_view_builds_fields_for_each_mode() -> None:
    state, _seen = _replay(_domain())
    sensor = state.snapshot().nodes[2].sensors[1]
    metrics = acoustic_view.latest(sensor)
    assert sensor.sensor_mode == SensorMode.EVENTS
    labels = [label for label, _ in acoustic_view.mode_fields(sensor, metrics)]
    assert "Último evento" in labels and "SEL combinado da sessão" in labels
    octaves = acoustic_view.octave_lines(metrics, 60).plain
    assert "31,5 Hz" in octaves and "acima de fs/2" in octaves
    for line in octaves.splitlines():
        assert len(line) <= 60
    assert acoustic_view.summary_segments(sensor)[0].startswith("LAeq")
    assert acoustic_view.event_items(sensor)


def test_acoustic_spectrum_chart_is_in_db_spl() -> None:
    spectrum = SpectrumSample(1, 1, [0.002] * 63 + [1.0], sample_rate_hz=16000.0, fft_size=128, magnitude_unit="Pa")
    chart = acoustic_spectrum_chart(spectrum, width=60)
    assert "dB ┤" in chart and "dB re 20 µPa" in chart
    assert all(len(line) <= 60 for line in chart.splitlines()[:-1])
    assert acoustic_spectrum_peaks(spectrum)[0] == "1. 7875,0 Hz  94,0 dB SPL  faixa 63"


def test_acoustic_actions_are_offered_only_for_the_acoustic_profile() -> None:
    acoustic = {action.key for action in actions_for("sensor", profile="ACOUSTIC", mode="GATEWAY_CAN")}
    vibration = {action.key for action in actions_for("sensor", profile="VIBRATION", mode="GATEWAY_CAN")}
    new_actions = {"sensor.measure_reset", "sensor.baseline_learn", "sensor.baseline_clear", "sensor.acoustic_calibrate"}
    assert new_actions <= acoustic and not (new_actions & vibration)
    assert {"sensor.fft", "sensor.configure"} <= acoustic
    direct = {action.key for action in actions_for("sensor", profile="ACOUSTIC", mode="SENSOR_DIRECT")}
    assert "sensor.restart_acq" not in direct   # ACQ POLLING é do acelerômetro


def test_acoustic_console_commands_require_authorization() -> None:
    security = SecurityManager("presence", "/caminho/inexistente")
    for command in ("MEASURE RESET", "BASELINE LEARN 60", "BASELINE CLEAR", "CAL 94"):
        assert security.command_requires_auth(command), command


# --------------------------------------------------------------------------
# 3. Telas
# --------------------------------------------------------------------------


def demo_app(prefs_file) -> PicoTuiApp:
    return PicoTuiApp(demo=True, enable_file_log=False, security_mode="off", preferences_path=prefs_file)


def _static(screen, selector: str) -> str:
    from textual.css.query import NoMatches

    try:
        return str(screen.query_one(selector).render())
    except NoMatches:
        return ""


def _text(screen, selector: str) -> str:
    """Texto de um widget, ou "" enquanto a tela ainda está sendo montada."""

    from textual.css.query import NoMatches

    try:
        return screen.query_one(selector).source_text()
    except NoMatches:
        return ""


async def test_demo_acoustic_sensor_screen_and_modes(prefs_file) -> None:
    app = demo_app(prefs_file)
    async with app.run_test(size=(150, 50)) as pilot:
        await settle(pilot, 6)
        app.open_target("sensor:22.01")
        assert await wait_for(pilot, lambda: isinstance(app.screen, SensorScreen))
        screen = app.screen
        assert await wait_for(pilot, lambda: "LAeq" in _text(screen, "#acoustic-levels"), 5.0)
        assert screen.query_one("#sec-acoustic").display is True
        assert screen.query_one("#sec-metrics").display is False
        assert screen.query_one("#sec-raw").display is False
        assert screen.query_one("#sec-acoustic-mode").border_title.startswith("Exposição ao ruído")
        assert "Dose NHO-01" in screen.query_one("#acoustic-mode").source_text()
        assert "Critério NC" in screen.query_one("#acoustic-insights").source_text()
        hints = screen.query_one("#key-bar").source_text()
        assert "Espectro" in hints and "Reiniciar medição" in hints and "Aprender referência" not in hints

        # Modo MACHINE pela configuração (demonstração). A 16 kHz com 1024 pontos a
        # busca começa em 62,5 Hz: o motor de 1770 rpm (29,5 Hz) não aparece.
        app.demo_producer.configure(22, 1, "MACHINE", 3.0)
        assert await wait_for(pilot, lambda: "62,5 a 1594 Hz" in _text(screen, "#acoustic-insights"), 5.0)
        app.demo_producer.configure(22, 1, "MACHINE", 3.0, 8000.0, 2048)
        assert await wait_for(pilot, lambda: screen.query_one("#sec-acoustic-mode").border_title == "Diagnóstico de máquina", 5.0)
        assert await wait_for(pilot, lambda: "rpm" in _text(screen, "#acoustic-mode"), 5.0)
        assert "Aprender referência" in screen.query_one("#key-bar").source_text()
        await app.execute_action("sensor.baseline_learn", "sensor:22.01", "5")
        assert await wait_for(pilot, lambda: "Pronta" in _text(screen, "#acoustic-mode"), 10.0)

        # Espectro em dB SPL.
        await app.execute_action("sensor.fft", "sensor:22.01", "64")
        assert await wait_for(pilot, lambda: isinstance(app.screen, FftScreen))
        assert await wait_for(pilot, lambda: "dB ┤" in _static(app.screen, "#fft-ascii"), 5.0)


async def test_config_dialog_offers_acoustic_choices(prefs_file) -> None:
    app = demo_app(prefs_file)
    async with app.run_test(size=(150, 50)) as pilot:
        await settle(pilot, 5)
        sensor = app.state_store.find_sensor("22.01")
        assert sensor is not None and sensor.profile_id == "ACOUSTIC"
        dialog = ConfigScreen(sensor)
        app.push_screen(dialog)
        await settle(pilot, 2)
        from textual.widgets import Select

        modes = [value for _label, value in dialog.query_one("#cfg-mode", Select)._options if value is not Select.BLANK]
        assert modes == ["IDLE", "LEVEL", "MACHINE", "EVENTS"]
        rates = [value for _label, value in dialog.query_one("#cfg-rate-select", Select)._options if value is not Select.BLANK]
        assert rates == [8000, 16000, 24000, 32000, 48000]
        request = dialog._build(True)
        assert request is not None and request.mode == "LEVEL" and request.rate_hz == 16000.0


async def test_direct_acoustic_firmware_calibration_and_measure_reset(security_file, prefs_file) -> None:
    firmware = FakeAcousticFirmware()
    firmware.start()
    await asyncio.sleep(0.15)
    try:
        app = PicoTuiApp(
            port=firmware.slave_name,
            mode="sensor",
            enable_file_log=False,
            security_mode="off",
            security_config=str(security_file),
            preferences_path=prefs_file,
        )
        async with app.run_test(size=(140, 45)) as pilot:
            assert await wait_for(pilot, lambda: (app.state_store.find_sensor("01.01") or None) is not None
                                  and app.state_store.find_sensor("01.01").profile_id == "ACOUSTIC", 5.0)
            await app.execute_action("sensor.telemetry_once", "sensor:01.01")
            assert await wait_for(pilot, lambda: app.state_store.find_sensor("01.01").latest_telemetry is not None, 5.0)
            await app.execute_action("sensor.acoustic_calibrate", "sensor:01.01", "94")
            assert await wait_for(pilot, lambda: "CAL 94" in firmware.received_commands, 5.0)
            assert await wait_for(
                pilot, lambda: app.state_store.find_sensor("01.01").configuration.calibration_gain == pytest.approx(1.122, abs=1e-3), 5.0
            )
            await app.execute_action("sensor.measure_reset", "sensor:01.01")
            assert await wait_for(pilot, lambda: firmware.measure_resets == 1, 5.0)
            # Comandos mutáveis passam pela autenticação do firmware.
            assert any(command.startswith("AUTH UNLOCK") for command in firmware.received_commands)
    finally:
        firmware.stop()


async def test_config_dialog_proposes_the_threshold_of_each_mode(prefs_file) -> None:
    app = demo_app(prefs_file)
    async with app.run_test(size=(150, 50)) as pilot:
        await settle(pilot, 5)
        from textual.widgets import Input, Select

        dialog = ConfigScreen(app.state_store.find_sensor("22.01"))
        app.push_screen(dialog)
        await settle(pilot, 2)
        threshold = dialog.query_one("#cfg-stalta", Input)
        assert threshold.value == "8"                    # limiar atual do sensor (padrão de EVENTS)
        dialog.query_one("#cfg-mode", Select).value = "MACHINE"
        assert await wait_for(pilot, lambda: threshold.value == "3")
        threshold.value = "5"                            # valor escolhido pelo operador fica
        dialog.query_one("#cfg-mode", Select).value = "EVENTS"
        await settle(pilot, 2)
        assert threshold.value == "5"
