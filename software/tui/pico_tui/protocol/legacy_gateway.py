from __future__ import annotations

import re

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import (
    CanFrameReceived,
    LegacyHeartbeatReceived,
    LogEvent,
    PhysicalNodeReceived,
    TelemetryReceived,
)
from pico_tui.core.models import DataQuality, SensorMode, TelemetrySample


class LegacyGatewayDecoder:
    """Compatibilidade com as linhas textuais do projeto distribuído da Vr1."""

    HEARTBEAT = re.compile(
        r"(?:\[GW\]|\[NODE\s+(?P<leader_node>\d+)\]\s+\[HEARTBEAT TX\])\s*"
        r"(?:HEARTBEAT\s+)?(?:lider=(?P<leader>\d+)\s+)?rodada=(?P<round>\d+)\s+"
        r"sensores_ativos=(?P<active>\d+)\s+modo=(?P<mode>\d+)\s+periodo=(?P<period>\d+)\s+ms"
    )
    GW_SENSOR = re.compile(
        r"\[GW\]\s+SENSOR\s+sensor=(?P<node>\d+)\s+rodada=(?P<round>\d+)\s+"
        r"valor=0x(?P<value>[0-9A-Fa-f]+)\s+ativo=(?P<active>\d+)"
    )
    NODE_SENSOR = re.compile(
        r"\[NODE\s+(?P<prefix>\d+)\]\s+\[SENSOR TX\]\s+sensor=(?P<node>\d+)\s+"
        r"rodada=(?P<round>\d+)\s+valor=0x(?P<value>[0-9A-Fa-f]+)"
    )
    STATUS = re.compile(
        r"(?:\[GW\]\s+Atualizacao(?: FORCADA)? de status recebida:\s+)?"
        r"\[(?:STATUS|STATUS RX)\]\s+NODE\s+(?P<node>\d+)\s+estado=(?P<state>[A-Z_]+)"
    )
    DISCOVERED = re.compile(r"(?:\[GW\]|\[DESCOBERTA\])\s+No descoberto:\s+NODE\s+(?P<node>\d+)")
    LEADER = re.compile(r"\[GW\]\s+Lider observado:\s+NODE\s+(?P<node>\d+)")
    OLD_FRAME = re.compile(
        r"\[(?P<ms>\d+)\s+ms\]\s+(?P<direction>RX|TX)\s+"
        r"ID=0x(?P<can_id>[0-9A-Fa-f]+)\s+DLC=(?P<dlc>\d+)\s+"
        r"DATA=(?P<data>[0-9A-Fa-f ]*)"
    )

    def __init__(self, bus: EventBus) -> None:
        self.bus = bus

    async def decode(self, line: str) -> bool:
        if match := self.HEARTBEAT.search(line):
            leader = int(match.group("leader") or match.group("leader_node") or 0)
            await self.bus.publish(
                LegacyHeartbeatReceived(
                    leader=leader,
                    round_number=int(match.group("round")),
                    active_sensors=int(match.group("active")),
                    mode=int(match.group("mode")),
                    period_ms=int(match.group("period")),
                )
            )
            return True
        if match := self.GW_SENSOR.search(line):
            await self._sensor(match)
            return True
        if match := self.NODE_SENSOR.search(line):
            await self._sensor(match)
            return True
        if match := self.STATUS.search(line):
            await self.bus.publish(
                PhysicalNodeReceived(
                    parent_node_id=int(match.group("node")),
                    payload={"STATE": match.group("state")},
                )
            )
            return True
        if match := self.DISCOVERED.search(line):
            await self.bus.publish(PhysicalNodeReceived(int(match.group("node")), {"STATE": "ONLINE"}))
            return True
        if match := self.LEADER.search(line):
            await self.bus.publish(PhysicalNodeReceived(int(match.group("node")), {"STATE": "LEADER"}))
            return True
        if match := self.OLD_FRAME.match(line):
            data_text = match.group("data").strip()
            try:
                data = bytes(int(part, 16) for part in data_text.split()) if data_text else b""
            except ValueError:
                data = b""
            await self.bus.publish(
                CanFrameReceived(
                    can_id=int(match.group("can_id"), 16),
                    data=data,
                    timestamp_ms=int(match.group("ms")),
                    direction=match.group("direction"),
                    fd=False,
                    brs=False,
                )
            )
            await self._decode_legacy_payload(data)
            return True
        return False


    async def _decode_legacy_payload(self, data: bytes) -> None:
        if len(data) < 4:
            return
        b0, b1, b2, b3 = data[:4]
        status_names = {
            0x00: "LEADER",
            0x01: "ONLINE",
            0x02: "OFFLINE",
            0x03: "FALHA",
            0x04: "GATEWAY",
            0x05: "AGING",
            0x06: "CONFLICT",
        }
        if b0 == 0x23 and b1 in {0x21, 0x50, 0x51}:
            await self.bus.publish(
                PhysicalNodeReceived(b2, {"STATE": status_names.get(b3, "UNKNOWN")})
            )
            return
        if b0 == 0x22 and b1 == 0x10:
            action_states = {0x00: "OFFLINE", 0x11: "ONLINE", 0x33: "FALHA", 0x44: "ONLINE"}
            if b3 in action_states:
                await self.bus.publish(PhysicalNodeReceived(b2, {"STATE": action_states[b3]}))

    async def _sensor(self, match: re.Match[str]) -> None:
        node = int(match.group("node"))
        value = int(match.group("value"), 16)
        round_number = int(match.group("round"))
        sample = TelemetrySample(
            parent_node_id=node,
            child_id=0,
            sequence=round_number & 0xFFFF,
            mode=SensorMode.UNKNOWN,
            quality=DataQuality.REAL,
            raw={"LEGACY_VALUE": value, "ROUND": round_number},
        )
        await self.bus.publish(TelemetryReceived(sample))
        await self.bus.publish(LogEvent("DEBUG", f"Legacy NODE {node}: valor=0x{value:X}", "GATEWAY"))
