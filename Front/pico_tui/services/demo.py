from __future__ import annotations

import asyncio
import math
import random

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import (
    BusStatsReceived,
    WirelessConfigReceived,
    FaultCommandSent,
    FaultEventReceived,
    GatewayDetected,
    GatewayStatusReceived,
    IdStatsReceived,
    LegacyHeartbeatReceived,
    PhysicalNodeReceived,
    SensorStatusReceived,
    TelemetryReceived,
)
from pico_tui.core.models import (
    AcousticMetrics,
    AcquisitionMode,
    BatteryInfo,
    DataQuality,
    SensorMode,
    SpectrumSample,
    TelemetrySample,
)


# Identificadores e períodos (ms) do tráfego sintético da demonstração:
# (identificador, quadros por rajada, período da rajada, DLC).
DEMO_TRAFFIC = (
    (0x080, 3, 10000, 4),
    (0x100, 1, 1000, 5),
    (0x200, 1, 1000, 5),
    (0x2A8, 1, 5000, 8),
    (0x2A9, 1, 5000, 8),
    (0x348, 1, 10000, 8),
    (0x349, 1, 10000, 8),
    (0x394, 18, 1000, 8),
    (0x395, 9, 1000, 8),
    (0x396, 19, 1000, 8),   # relatório acústico: 108 bytes em 19 quadros
)
DEMO_LEADER = 23
ACOUSTIC_DEMO = (22, 1, "0x10C1")

# Forma do espectro em bandas de oitava (31,5 Hz a 16 kHz) de uma oficina,
# relativa ao LAeq; somada ao nível do momento.
_OCTAVE_SHAPE = (-14.0, -6.0, -2.0, 0.0, 1.0, 0.5, -2.5, -7.0, -14.0, -23.0)
_EVENT_CLASSES = ("IMPACT", "CRACK", "TONAL", "TRANSIENT")


class DemoAcousticSensor:
    """Sensor acústico simulado: um relatório por segundo, nos três modos.

    Reage à configuração (modo e limiar), ao reinício da medição, ao
    aprendizado da assinatura de referência e à calibração, para que as
    análises da TUI possam ser exploradas sem hardware.
    """

    def __init__(self, parent: int, child: int) -> None:
        self.parent = parent
        self.child = child
        self.mode = "LEVEL"
        self.threshold = 8.0
        self.gain_db = 0.0
        self.seconds = 0
        self.measure_seconds = 0
        self.dose_nr15 = 0.0
        self.dose_nho01 = 0.0
        self.levels: list[float] = []
        self.events = 0
        self.next_event = 6
        self.last_event: tuple[str, float, float, int, float, int] | None = None   # classe, pico, SEL, ms, Hz, fim
        self.baseline = "NONE"
        self.baseline_until = 0
        self.baseline_length = 0
        self.seq = 0

    # -------------------------------------------------------------- controle
    def configure(self, mode: str, threshold: float | None) -> None:
        if mode.upper() in {"IDLE", "LEVEL", "MACHINE", "EVENTS"}:
            self.mode = mode.upper()
        if threshold is not None and threshold > 1.0:
            self.threshold = threshold

    def control(self, action: str, seconds: int) -> None:
        if action == "MEASURE_RESET":
            self.measure_seconds = 0
            self.dose_nr15 = self.dose_nho01 = 0.0
            self.levels.clear()
            self.events = 0
            self.last_event = None
        elif action == "BASELINE_LEARN":
            self.baseline = "LEARNING"
            self.baseline_length = max(5, seconds)
            self.baseline_until = self.seconds + self.baseline_length
        elif action == "BASELINE_CLEAR":
            self.baseline = "NONE"

    # ---------------------------------------------------------------- medida
    def _laeq(self) -> float:
        return 81.0 + 3.0 * math.sin(self.seconds / 23.0) + 1.2 * math.sin(self.seconds / 3.1) + self.gain_db

    def report(self) -> TelemetrySample | None:
        self.seconds += 1
        if self.mode == "IDLE":
            return None
        self.seq = (self.seq + 1) & 0xFFFF
        self.measure_seconds += 1
        laeq = self._laeq()
        # A 16 kHz de amostragem o firmware omite as bandas de 8 e 16 kHz (acima de fs/2).
        octaves: list[float | None] = [laeq + shape + random.uniform(-0.6, 0.6) for shape in _OCTAVE_SHAPE[:8]]
        octaves += [None, None]
        metrics = AcousticMetrics(
            interval_ms=1000,
            coverage_pct=100,
            dsp_load_pct=34 + self.seconds % 6,
            channel="LEFT",
            spectrum_valid=True,
            overload=False,
            mic_fault=False,
            laeq_db=laeq,
            lceq_db=laeq + 8.2,
            lzeq_db=laeq + 9.1,
            lzpeak_db=laeq + 9.1 + 11.5 + random.uniform(-0.5, 0.5),
            laf_db=laeq - 0.4,
            lafmax_db=laeq + 3.1,
            lafmin_db=laeq - 2.7,
            crest=3.8,
            kurtosis=0.2,
            peak_hz=1000.0,
            centroid_hz=1350.0,
            flatness=0.21,
            octave_db=octaves,
        )
        if self.mode == "LEVEL":
            self.levels.append(laeq)
            self.dose_nr15 += 100.0 / (28800.0 / 2.0 ** ((laeq - 85.0) / 5.0)) if laeq >= 80.0 else 0.0
            self.dose_nho01 += 100.0 / (28800.0 / 2.0 ** ((laeq - 85.0) / 3.0)) if laeq >= 80.0 else 0.0
            ordered = sorted(self.levels)
            metrics.leq_t_db = 10.0 * math.log10(sum(10.0 ** (v / 10.0) for v in self.levels) / len(self.levels))
            metrics.l10_db = ordered[int(0.9 * (len(ordered) - 1))]
            metrics.l50_db = ordered[int(0.5 * (len(ordered) - 1))]
            metrics.l90_db = ordered[int(0.1 * (len(ordered) - 1))]
            metrics.dose_nr15_pct = self.dose_nr15
            metrics.dose_nho01_pct = self.dose_nho01
            metrics.elapsed_s = self.measure_seconds
            metrics.dose_limit = self.dose_nho01 >= 100.0
        elif self.mode == "MACHINE":
            # Motor de 4 polos a 60 Hz com 1,7 % de escorregamento: 1770 rpm.
            metrics.f0_hz = 29.5 + random.uniform(-0.05, 0.05)
            metrics.harmonics = 5
            metrics.tonal_db = 13.5 + random.uniform(-0.5, 0.5)
            metrics.peak_hz = 4 * metrics.f0_hz
            metrics.flatness = 0.04
            if self.baseline == "LEARNING" and self.seconds >= self.baseline_until:
                self.baseline = "READY"
            metrics.baseline = self.baseline
            if self.baseline == "LEARNING":
                done = self.baseline_length - (self.baseline_until - self.seconds)
                metrics.baseline_pct = max(0, min(99, round(100.0 * done / self.baseline_length)))
            elif self.baseline == "READY":
                metrics.baseline_pct = 100
                metrics.anomaly_z = 0.9 + 0.6 * abs(math.sin(self.seconds / 9.0))
                metrics.anomaly = metrics.anomaly_z >= self.threshold
            else:
                metrics.baseline_pct = 0
        elif self.mode == "EVENTS":
            metrics.background_db = laeq - 3.0
            metrics.sta_lta_db = random.uniform(0.0, 2.0)
            metrics.events_interval = 0
            metrics.event_active = False
            if self.measure_seconds >= self.next_event:
                self.next_event = self.measure_seconds + random.randint(7, 15)
                self.events += 1
                kind = random.choice(_EVENT_CLASSES)
                peak = laeq + random.uniform(12.0, 22.0)
                duration = 900 if kind in {"TONAL", "TRANSIENT"} else random.randint(25, 120)
                centroid = 5200.0 if kind == "CRACK" else (2400.0 if kind == "TONAL" else 700.0)
                self.last_event = (kind, peak, peak - 14.0 + duration / 200.0, duration, centroid, self.measure_seconds)
                metrics.events_interval = 1
                metrics.event_active = True
                metrics.sta_lta_db = peak - laeq
            metrics.events_total = self.events
            if self.last_event is not None:
                kind, peak, sel, duration, centroid, ended = self.last_event
                metrics.event_class = kind
                metrics.event_peak_db = peak
                metrics.event_sel_db = sel
                metrics.event_duration_ms = duration
                metrics.event_centroid_hz = centroid
                metrics.event_age_s = self.measure_seconds - ended
        return TelemetrySample(
            parent_node_id=self.parent,
            child_id=self.child,
            sequence=self.seq,
            mode=SensorMode(self.mode),
            acquisition_mode=AcquisitionMode.I2S,
            sample_rate_requested_hz=16000.0,
            sample_rate_effective_hz=16000.0,
            window_type="HANN",
            window_size=1024,
            fft_valid=True,
            kurtosis=metrics.kurtosis,
            crest_factor=metrics.crest,
            peak_frequency_hz=metrics.peak_hz,
            quality=DataQuality.REAL,
            battery=BatteryInfo(present=True, percentage=78.0, voltage_v=3.86, source="ADC", valid=True),
            acoustic=metrics,
        )

    def spectrum(self, bins: int) -> SpectrumSample:
        """Espectro em Pa eficaz por faixa, de 0 a fs/2 (16 kHz de amostragem)."""

        rate = 16000.0
        df = rate / 2.0 / bins
        reference = 20e-6 * 10.0 ** (self._laeq() / 20.0)
        values = []
        for index in range(bins):
            if index == 0:
                values.append(0.0)   # o firmware descarta a componente contínua
                continue
            frequency = index * df
            pink = reference * 0.02 / math.sqrt(1.0 + frequency / 400.0)
            value = pink * random.uniform(0.85, 1.15)
            if self.mode == "MACHINE":
                for harmonic in range(1, 6):
                    if abs(frequency - harmonic * 29.5) < df:
                        value += reference * 0.25 / harmonic
            elif abs(frequency - 1000.0) < df:
                value += reference * 0.12
            values.append(value)
        return SpectrumSample(
            self.parent, self.child, values, transfer_id=self.seq, sample_rate_hz=rate, fft_size=bins * 2,
            window_type="HANN", magnitude_unit="Pa",
        )


class DemoProducer:
    def __init__(self, bus: EventBus) -> None:
        self.bus = bus
        self._running = True
        self._seq: dict[tuple[int, int], int] = {}
        self._pending: list[str] = []
        self._load_percent = 0.0
        self._load_ticks = 0
        self._fault_ticks = 0
        self._fault: tuple[int, str, int, int] | None = None   # módulo, tipo, duração (ms), sequência
        self._fault_seq = 0
        self.acoustic = DemoAcousticSensor(ACOUSTIC_DEMO[0], ACOUSTIC_DEMO[1])

    # Sensor acústico da demonstração: chamado pelo app no lugar do envio serial.
    def _acoustic_for(self, parent: int, child: int) -> DemoAcousticSensor | None:
        return self.acoustic if (parent, child) == (self.acoustic.parent, self.acoustic.child) else None

    def configure(self, parent: int, child: int, mode: str, threshold: float | None) -> None:
        if (sensor := self._acoustic_for(parent, child)) is not None:
            sensor.configure(mode, threshold)

    def acoustic_control(self, parent: int, child: int, action: str, seconds: int) -> None:
        if (sensor := self._acoustic_for(parent, child)) is not None:
            sensor.control(action, seconds)

    def acoustic_gain(self, parent: int, child: int, factor: float) -> None:
        if (sensor := self._acoustic_for(parent, child)) is not None and factor > 0:
            sensor.gain_db += 20.0 * math.log10(factor)

    def acoustic_spectrum(self, parent: int, child: int, bins: int) -> SpectrumSample:
        sensor = self._acoustic_for(parent, child) or self.acoustic
        return sensor.spectrum(bins)

    def handle_command(self, command: str) -> None:
        """Comandos de ensaio têm um efeito mínimo na demonstração.

        A rede simulada não reage como a real: apenas o início e o fim do
        ensaio e a carga no barramento são representados. Para ver um ensaio
        completo, use a reprodução de uma gravação (--replay).
        """

        if command.strip().upper().startswith("FAULT "):
            self._pending.append(command.strip().upper())

    async def _process_commands(self) -> None:
        while self._pending:
            parts = self._pending.pop(0).split()
            kind = parts[1] if len(parts) > 1 else ""
            try:
                if kind == "LOAD" and len(parts) >= 4:
                    self._load_percent = float(parts[2])
                    self._load_ticks = int(parts[3]) * 2
                    self._fault_seq += 1
                    await self.bus.publish(
                        FaultEventReceived(
                            0, "BUS_LOAD", "STARTED", int(parts[3]) * 1000, self._fault_seq,
                            {"LOAD_PCT": parts[2], "PRIORITY": parts[4] if len(parts) > 4 else "HIGH"},
                        )
                    )
                elif kind == "CANCEL":
                    self._load_ticks = min(self._load_ticks, 1) if self._load_ticks else 0
                    self._fault_ticks = min(self._fault_ticks, 1) if self._fault_ticks else 0
                elif kind in {"POWER_CYCLE", "SILENCE", "AP_OUTAGE", "DROP_SESSION"} and len(parts) >= 3:
                    node = int(parts[2])
                    seconds = int(parts[3]) if len(parts) > 3 else 0
                    self._fault_seq += 1
                    await self.bus.publish(FaultCommandSent(kind, node, seconds * 1000, self._fault_seq))
                    await self.bus.publish(FaultEventReceived(node, kind, "STARTED", seconds * 1000, self._fault_seq))
                    self._fault = (node, kind, seconds * 1000, self._fault_seq)
                    self._fault_ticks = max(1, seconds * 2)
            except ValueError:
                continue

    async def _network_tick(self, tick: int) -> None:
        """Sinal de presença e estatísticas do barramento, duas vezes por segundo."""

        await self._process_commands()
        if self._fault_ticks:
            self._fault_ticks -= 1
            if self._fault_ticks == 0 and self._fault is not None:
                node, kind, duration_ms, seq = self._fault
                await self.bus.publish(FaultEventReceived(node, kind, "ENDED", duration_ms, seq))
                self._fault = None
        if tick % 2:
            return
        second = tick // 2
        await self.bus.publish(LegacyHeartbeatReceived(DEMO_LEADER, second & 0xFF, 3, 3, 1000))

        frames = sum(burst * 1000 / period for _id, burst, period, _dlc in DEMO_TRAFFIC)
        bits = sum(burst * 1000 / period * (47 + 8 * dlc + 12) for _id, burst, period, dlc in DEMO_TRAFFIC)
        load = 100.0 * bits / 500_000 + random.random() * 0.05
        own = 0
        if self._load_ticks:
            self._load_ticks -= 2
            load += self._load_percent
            own = int(self._load_percent * 500_000 / 100 / 125)
            frames += own
            if self._load_ticks <= 0:
                self._load_ticks = 0
                await self.bus.publish(
                    FaultEventReceived(0, "BUS_LOAD", "ENDED", 0, self._fault_seq,
                                       {"LOAD_PCT": f"{self._load_percent:g}", "ACHIEVED_PCT": f"{self._load_percent:.2f}"})
                )
        await self.bus.publish(
            BusStatsReceived(
                probe_ms=second * 1000,
                window_ms=1000,
                frames=int(frames),
                bits=int(load * 5000),
                load_percent=round(load, 2),
                peak_percent=round(load * 1.6 + 0.3, 2),
                rx_queue_peak=9,
                ids=len(DEMO_TRAFFIC) + (1 if own else 0),
                transfers_ok=second * 3,
                own_frames=own,
            )
        )
        if second % 5 == 1:
            for can_id, burst, period, dlc in DEMO_TRAFFIC:
                count = max(1, burst * 5000 // period)
                jitter = random.randint(20, 400)
                if burst > 1:
                    spacing = 280 + random.randint(0, 60)
                    average = period * 1000 // burst
                    longest = period * 1000 - (burst - 1) * spacing
                    stats = (spacing, average, longest)
                else:
                    stats = (period * 1000 - jitter, period * 1000, period * 1000 + jitter)
                await self.bus.publish(
                    IdStatsReceived(can_id, 5000, count, dlc, count * (47 + 8 * dlc + 12), *stats)
                )

    async def run(self) -> None:
        await self.bus.publish(GatewayDetected("demo-1.0", "1"))
        await self.bus.publish(
            GatewayStatusReceived(
                {
                    "NODE": "0",
                    "CAN": "OK",
                    "WIFI": "OK",
                    "ARB": "500000",
                    "DATA": "2000000",
                    "UPTIME_MS": "1000",
                }
            )
        )
        sensors = ((20, 1, "0x10A4"), (20, 2, "0x10A5"), (21, 1, "0x10B1"))
        for parent in (20, 21, 22, 23):
            await self.bus.publish(PhysicalNodeReceived(parent, {"STATE": "ONLINE", "TYPE": "SENSOR_BRIDGE"}))
        for parent, child, uuid in sensors:
            await self.bus.publish(
                SensorStatusReceived(
                    parent,
                    child,
                    {"STATE": "ONLINE", "UUID": uuid, "ACQ": "POLLING", "QUALITY": "REAL", "TELEMETRY": "ON", "PERIOD_MS": "500"},
                )
            )
        parent, child, uuid = ACOUSTIC_DEMO
        await self.bus.publish(
            SensorStatusReceived(
                parent,
                child,
                {"STATE": "ONLINE", "UUID": uuid, "PROFILE": "ACOUSTIC", "MODE": "LEVEL", "ACQ": "I2S",
                 "QUALITY": "REAL", "TELEMETRY": "ON", "PERIOD_MS": "1000"},
            )
        )
        # Configuração em vigor, como o Node a publica depois do handshake.
        await self.bus.publish(
            WirelessConfigReceived(
                parent,
                child,
                "CURRENT",
                {"MODE": "LEVEL", "RATE_REQ_HZ": "16000", "RATE_EFF_HZ": "16000", "WINDOW": "HANN",
                 "WINDOW_SIZE": "1024", "STALTA": f"{self.acoustic.threshold:g}", "GAIN": "1.0", "PROFILE": "ACOUSTIC"},
            )
        )

        tick = 0
        while self._running:
            tick += 1
            await self._network_tick(tick)
            for index, (parent, child, _) in enumerate(sensors):
                key = (parent, child)
                self._seq[key] = (self._seq.get(key, 0) + 1) & 0xFFFF
                phase = tick / 7.0 + index
                battery = None if (parent, child) == (21, 1) else max(5.0, 95.0 - tick / 200.0 - index * 4)
                sample = TelemetrySample(
                    parent_node_id=parent,
                    child_id=child,
                    sequence=self._seq[key],
                    mode=(SensorMode.STRUCTURAL if index == 1 else SensorMode.ROTATING),
                    acquisition_mode=AcquisitionMode.POLLING,
                    sample_rate_requested_hz=250.0,
                    sample_rate_effective_hz=250.0,
                    window_type="HANN",
                    window_size=512,
                    rms=0.12 + index * 0.08 + 0.025 * math.sin(phase),
                    rms_unit="g",
                    kurtosis=0.2 + 0.1 * math.sin(phase / 2),
                    crest_factor=2.5 + 0.3 * math.cos(phase),
                    peak_frequency_hz=31.5 + index * 18 + 1.5 * math.sin(phase),
                    peak_amplitude=0.02 + random.random() * 0.002,
                    spectral_entropy=(0.65 if index == 1 else 0.45),
                    fft_valid=True,
                    axis="VECTOR",
                    ppv_mm_s=0.4 + index * 0.2,
                    stalta_triggered=False,
                    clipping=False,
                    quality=DataQuality.REAL,
                    battery=BatteryInfo(
                        present=battery is not None,
                        percentage=battery,
                        voltage_v=(3.9 if battery is not None else None),
                        source=("MAX17048" if battery is not None else "NOT_INSTRUMENTED"),
                        valid=battery is not None,
                    ),
                )
                await self.bus.publish(TelemetryReceived(sample))
            if tick % 2 == 0 and (acoustic := self.acoustic.report()) is not None:
                await self.bus.publish(TelemetryReceived(acoustic))
            await asyncio.sleep(0.5)

    def stop(self) -> None:
        self._running = False
