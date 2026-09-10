from __future__ import annotations

import re

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import (
    CanFrameReceived,
    CommandAck,
    LegacyHeartbeatReceived,
    LogEvent,
    LocalNodeTelemetryReceived,
    PhysicalNodeReceived,
)


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
    COMMAND_ACK = re.compile(
        r"\[GW\]\s+CMD_ACK\s+node=(?P<node>\d+)\s+subcmd=0x(?P<subcmd>[0-9A-Fa-f]+)\s+"
        r"action=0x(?P<action>[0-9A-Fa-f]+)\s+result=(?P<result>APPLIED|REJECTED)"
    )
    CAPABILITIES = re.compile(
        r"\[NODE\s+(?P<node>\d+)\]\s+CAPS=(?P<caps>[A-Z0-9_,]+)\s+"
        r"LOCAL_PROFILE=(?P<profile>[A-Z0-9_]+)"
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
        if match := self.COMMAND_ACK.search(line):
            node = int(match.group("node"))
            subcmd = int(match.group("subcmd"), 16)
            action = int(match.group("action"), 16)
            result = match.group("result")
            await self.bus.publish(
                CommandAck(
                    command=f"CAN Node {node:02d} subcmd=0x{subcmd:02X} action=0x{action:02X}",
                    state=result,
                    payload={"TARGET": f"node:{node}", "NODE": node, "SUBCMD": subcmd, "ACTION": action},
                )
            )
            await self.bus.publish(LogEvent("INFO", line, "CAN_ACK"))
            return True
        if match := self.CAPABILITIES.search(line):
            await self.bus.publish(
                PhysicalNodeReceived(
                    parent_node_id=int(match.group("node")),
                    payload={
                        "STATE": "ONLINE",
                        "TYPE": "CAN_NODE",
                        "CAPS": match.group("caps"),
                        "LOCAL_PROFILE": match.group("profile"),
                    },
                )
            )
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
        if b0 == 0x23 and b1 == 0x52 and len(data) >= 6:
            action = data[4]
            result = "APPLIED" if data[5] == 0x00 else "REJECTED"
            await self.bus.publish(
                CommandAck(
                    command=f"CAN Node {b2:02d} subcmd=0x{b3:02X} action=0x{action:02X}",
                    state=result,
                    payload={"TARGET": f"node:{b2}", "NODE": b2, "SUBCMD": b3, "ACTION": action},
                )
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
        enabled_group = match.groupdict().get("active")
        enabled = None if enabled_group is None else enabled_group not in {"0", "OFF", "FALSE"}
        await self.bus.publish(
            LocalNodeTelemetryReceived(
                parent_node_id=node,
                value=value,
                round_number=round_number,
                enabled=enabled,
                profile_id="DEMO_BYTE",
            )
        )
        await self.bus.publish(
            LogEvent(
                "DEBUG",
                f"Node CAN {node}: LOCAL_SENSOR_DEMO_VALUE=0x{value:02X}",
                "CAN_NODE",
            )
        )
