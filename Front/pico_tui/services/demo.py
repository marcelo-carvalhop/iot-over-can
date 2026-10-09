from __future__ import annotations

import asyncio
import math
import random

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import (
    BusStatsReceived,
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
    AcquisitionMode,
    BatteryInfo,
    DataQuality,
    SensorMode,
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
)
DEMO_LEADER = 23


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
            await asyncio.sleep(0.5)

    def stop(self) -> None:
        self._running = False
