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
    WirelessCandidateReceived,
    WirelessAssociationReceived,
    WirelessConfigReceived,
    WirelessLinkReceived,
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
    WIRELESS_CANDIDATE = re.compile(
        r"\[GW\]\s+WIRELESS_CANDIDATE\s+reporter=(?P<reporter>\d+)\s+"
        r"uuid=(?P<uuid>0x[0-9A-Fa-f]{16})\s+profile=(?P<profile>[A-Z0-9_]+)\s+"
        r"rssi=(?P<rssi>-?\d+)\s+protocol=(?P<protocol>\d+)"
    )
    WIRELESS_ASSOC = re.compile(
        r"\[GW\]\s+WIRELESS_ASSOC\s+node=(?P<node>\d+)\s+child=(?P<child>\d+)\s+"
        r"uuid=(?P<uuid>0x[0-9A-Fa-f]{16})\s+profile=(?P<profile>[A-Z0-9_]+)\s+"
        r"state=(?P<state>[A-Z_]+)\s+rssi=(?P<rssi>-?\d+)\s+protocol=(?P<protocol>\d+)"
    )
    WIRELESS_LINK = re.compile(
        r"\[GW\]\s+WIRELESS_LINK\s+node=(?P<node>\d+)\s+child=(?P<child>\d+)\s+"
        r"state=(?P<state>[A-Z_]+)\s+rssi=(?P<rssi>-?\d+)\s+age_s=(?P<age>\d+)\s+rx=(?P<rx>\d+)\s+"
        r"auth_fail=(?P<auth>\d+)\s+replay=(?P<replay>\d+)\s+lost=(?P<lost>\d+)\s+"
        r"stream=(?P<stream>ON|OFF)\s+period_ms=(?P<period>\d+)"
    )
    WIRELESS_CONFIG = re.compile(
        r"\[GW\]\s+WIRELESS_CONFIG\s+node=(?P<node>\d+)\s+child=(?P<child>\d+)\s+"
        r"status=(?P<status>[A-Z]+)\s+mode=(?P<mode>[A-Z]+)\s+rate_req_hz=(?P<rate_req>[-0-9.]+)\s+"
        r"rate_eff_hz=(?P<rate_eff>[-0-9.]+)\s+window=(?P<window>[A-Z]+)\s+"
        r"window_size=(?P<window_size>\d+)\s+stalta=(?P<stalta>[-0-9.]+)\s+gain=(?P<gain>[-0-9.]+)"
    )
    CONTROL_RX = re.compile(
        r"\[GW\]\s+CONTROLE RX\s+(?P<b0>[0-9A-Fa-f]{1,2})\s+"
        r"(?P<b1>[0-9A-Fa-f]{1,2})\s+(?P<b2>[0-9A-Fa-f]{1,2})\s+"
        r"(?P<b3>[0-9A-Fa-f]{1,2})"
    )
    ROUTINE_MAINTENANCE = (
        re.compile(r"\[STATUS TX\]\s+Requisicao global de status enviada"),
        re.compile(r"\[STATUS TX\]\s+node=\d+\s+status=\d+"),
        re.compile(r"\[NODE\s+\d+\]\s+\[STATUS TX\]\s+codigo=\d+"),
        re.compile(r"\[REPLICA TX\]\s+node=\d+\s+status=\d+"),
        re.compile(r"\[NODE\s+\d+\]\s+\[HB\]\s+modo=\d+\s+periodo=\d+\s+ms"),
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
        if match := self.CONTROL_RX.search(line):
            data = bytes(int(match.group(name), 16) for name in ("b0", "b1", "b2", "b3"))
            b0, b1, _, _ = data
            known_control = (
                (b0 == 0x23 and b1 in {0x21, 0x41, 0x50, 0x51, 0x52})
                or (b0 == 0x22 and b1 in {0x00, 0x10, 0x20, 0x30, 0x40})
            )
            if known_control:
                await self._decode_legacy_payload(data)
                await self.bus.publish(LogEvent("DEBUG", line, "CAN_MAINT"))
                return True
        if any(pattern.search(line) for pattern in self.ROUTINE_MAINTENANCE):
            await self.bus.publish(LogEvent("DEBUG", line, "CAN_MAINT"))
            return True
        if match := self.WIRELESS_CANDIDATE.search(line):
            await self.bus.publish(
                WirelessCandidateReceived(
                    reporter_node_id=int(match.group("reporter")),
                    wireless_uuid=match.group("uuid").upper().replace("0X", "0x"),
                    profile_id=match.group("profile").upper(),
                    rssi_dbm=int(match.group("rssi")),
                    protocol_version=match.group("protocol"),
                )
            )
            return True
        if match := self.WIRELESS_ASSOC.search(line):
            await self.bus.publish(
                WirelessAssociationReceived(
                    parent_node_id=int(match.group("node")),
                    child_id=int(match.group("child")),
                    wireless_uuid=match.group("uuid").upper().replace("0X", "0x"),
                    profile_id=match.group("profile").upper(),
                    state=match.group("state").upper(),
                    rssi_dbm=int(match.group("rssi")),
                    protocol_version=match.group("protocol"),
                )
            )
            return True
        if match := self.WIRELESS_LINK.search(line):
            rssi = int(match.group("rssi"))
            await self.bus.publish(
                WirelessLinkReceived(
                    parent_node_id=int(match.group("node")),
                    child_id=int(match.group("child")),
                    state=match.group("state").upper(),
                    rssi_dbm=rssi if rssi != 0 else None,  # 0 = não medido pelo Node
                    session_age_s=int(match.group("age")),
                    rx_datagrams=int(match.group("rx")),
                    auth_failures=int(match.group("auth")),
                    replay_drops=int(match.group("replay")),
                    lost_datagrams=int(match.group("lost")),
                    stream_enabled=match.group("stream") == "ON",
                    stream_period_ms=int(match.group("period")) or None,
                )
            )
            return True
        if match := self.WIRELESS_CONFIG.search(line):
            await self.bus.publish(
                WirelessConfigReceived(
                    parent_node_id=int(match.group("node")),
                    child_id=int(match.group("child")),
                    status=match.group("status").upper(),
                    payload={
                        "MODE": match.group("mode").upper(),
                        "RATE_REQ_HZ": match.group("rate_req"),
                        "RATE_EFF_HZ": match.group("rate_eff"),
                        "WINDOW": match.group("window").upper(),
                        "WINDOW_SIZE": match.group("window_size"),
                        "STALTA": match.group("stalta"),
                        "GAIN": match.group("gain"),
                    },
                )
            )
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
