from __future__ import annotations

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import ConnectionModeDetected, LogEvent, RawLineReceived
from pico_tui.core.models import ConnectionMode
from pico_tui.protocol.gateway_text import GatewayTextDecoder
from pico_tui.protocol.sensor_direct import SensorDirectDecoder


class DecoderRouter:
    """Detecta o protocolo e encaminha as linhas em ordem ao decoder adequado."""

    GATEWAY_PREFIXES = (
        "GW_",
        "NODE ",
        "SENSOR ",
        "CAN_RX ",
        "CAN_TX ",
        "FRAG ",
        "CRC_ERROR ",
        "ACK ",
        "DTC_CLEAR ",
        "[GW]",
        "[NODE ",
        "[STATUS",
        "[DESCOBERTA]",
    )
    SENSOR_PREFIXES = (
        "EDGE DSP",
        "ASCII SERIAL",
        "VERSION ",
        "STATUS ",
        "CONFIG_APPLIED ",
        "STAGED ",
        "FFT ",
        "DTC_EVENT ",
        "NET_EVENT ",
        "WARN ",
        "ERR ",
        "OK ",
        "NOTE ",
        "PONG ",
        "EDGE>",
    )

    def __init__(self, bus: EventBus, requested_mode: str = "auto") -> None:
        self.bus = bus
        self.requested_mode = requested_mode
        self.gateway = GatewayTextDecoder(bus)
        self.sensor = SensorDirectDecoder(bus)
        self.mode = self._forced_mode()

    def _forced_mode(self) -> ConnectionMode:
        return {
            "gateway": ConnectionMode.GATEWAY_CAN,
            "gateway_can": ConnectionMode.GATEWAY_CAN,
            "sensor": ConnectionMode.SENSOR_DIRECT,
            "sensor_direct": ConnectionMode.SENSOR_DIRECT,
        }.get(self.requested_mode.lower(), ConnectionMode.UNKNOWN)

    def reset(self) -> None:
        self.mode = self._forced_mode()
        self.sensor.reset()

    async def decode(self, line: str) -> None:
        await self.bus.publish(RawLineReceived(line))
        detected = self._detect(line)
        if self.mode == ConnectionMode.UNKNOWN and detected != ConnectionMode.UNKNOWN:
            self.mode = detected
            await self.bus.publish(ConnectionModeDetected(self.mode))

        mode = self.mode if self.mode != ConnectionMode.UNKNOWN else detected
        if mode == ConnectionMode.GATEWAY_CAN:
            await self.gateway.decode(line)
        elif mode == ConnectionMode.SENSOR_DIRECT:
            await self.sensor.decode(line)
        else:
            await self.bus.publish(LogEvent("DEBUG", f"AUTO_UNPARSED: {line}", "SERIAL"))

    def _detect(self, line: str) -> ConnectionMode:
        stripped = line.strip()
        upper = stripped.upper()
        if upper.startswith(self.GATEWAY_PREFIXES):
            return ConnectionMode.GATEWAY_CAN
        if upper.startswith("TEL "):
            return (
                ConnectionMode.GATEWAY_CAN
                if (" NODE=" in f" {upper}" or " CHILD=" in f" {upper}")
                else ConnectionMode.SENSOR_DIRECT
            )
        if upper.startswith("DTC "):
            return ConnectionMode.GATEWAY_CAN if " NODE=" in f" {upper}" else ConnectionMode.SENSOR_DIRECT
        if upper.startswith(self.SENSOR_PREFIXES):
            return ConnectionMode.SENSOR_DIRECT
        return ConnectionMode.UNKNOWN
