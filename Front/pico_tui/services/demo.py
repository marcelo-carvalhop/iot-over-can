from __future__ import annotations

import asyncio
import math
import random

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import (
    GatewayDetected,
    GatewayStatusReceived,
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


class DemoProducer:
    def __init__(self, bus: EventBus) -> None:
        self.bus = bus
        self._running = True
        self._seq: dict[tuple[int, int], int] = {}

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
