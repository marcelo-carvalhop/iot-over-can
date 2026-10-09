from __future__ import annotations

import re

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import (
    BusStatsReceived,
    CanFrameReceived,
    CommandAck,
    FaultCommandSent,
    FaultEventReceived,
    IdStatsReceived,
    LegacyHeartbeatReceived,
    LogEvent,
    LocalNodeTelemetryReceived,
    NetworkEventReceived,
    PhysicalNodeReceived,
    WirelessCandidateReceived,
    WirelessAssociationReceived,
    WirelessConfigReceived,
    WirelessFailoverReceived,
    WirelessLinkReceived,
)
from pico_tui.protocol.common import parse_key_values


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
        r"(?:\s+failover=(?P<failover>AUTO|MANUAL))?(?:\s+session=(?P<session>SECURE|NONE))?"
    )
    WIRELESS_FAILOVER = re.compile(
        r"\[GW\]\s+WIRELESS_FAILOVER\s+uuid=(?P<uuid>0x[0-9A-Fa-f]{16})\s+from=(?P<from>\d+)\s+"
        r"to=(?P<to>\d+)\s+reason=(?P<reason>[A-Z_]+)(?:\s+rssi=(?P<rssi>-?\d+))?"
    )
    BUS_STATS = re.compile(r"\[GW\]\s+BUS_STATS\s+")
    ID_STATS = re.compile(r"\[GW\]\s+ID_STATS\s+")
    FAULT_TX = re.compile(r"\[GW\]\s+FAULT_TX\s+")
    FAULT_EVENT = re.compile(r"\[GW\]\s+FAULT_EVENT\s+")
    FAULT_ERROR = re.compile(r"\[GW\]\s+(?:FAULT_ERROR|METRICS_ERROR)\s+(?P<detail>.*)")
    # Fatos da coordenação da rede relatados pela Probe 00.
    NETWORK_EVENTS = (
        (re.compile(r"\[GW\]\s+Falha detectada no lider NODE\s+(?P<node>\d+)"), "LEADER_FAILURE_DETECTED", "WARNING",
         "Probe 00: líder Node {node:02d} sem sinal de presença; nova eleição solicitada"),
        (re.compile(r"\[GW\]\s+Requisicao de eleicao observada"), "ELECTION_REQUESTED", "INFO",
         "Eleição solicitada por um módulo da rede"),
        (re.compile(r"\[GW\]\s+Comando enviado:\s+22 00 "), "ELECTION_REQUESTED", "INFO",
         "Eleição solicitada pela Probe 00"),
        (re.compile(r"\[GW\]\s+ELEICAO recebida de NODE\s+(?P<node>\d+)"), "ELECTION_ANNOUNCE", "DEBUG",
         "Node {node:02d} participa da eleição"),
        (re.compile(r"\[GW\]\s+Lider anunciado:\s+NODE\s+(?P<node>\d+)"), "LEADER_ANNOUNCED", "INFO",
         "Node {node:02d} anunciou a liderança"),
        (re.compile(r"\[GW\]\s+JOIN observado de NODE\s+(?P<node>\d+)"), "JOIN_OBSERVED", "INFO",
         "Node {node:02d} pediu para entrar na rede"),
    )
    WIRELESS_LINK = re.compile(
        r"\[GW\]\s+WIRELESS_LINK\s+node=(?P<node>\d+)\s+child=(?P<child>\d+)\s+"
        r"state=(?P<state>[A-Z_]+)\s+rssi=(?P<rssi>-?\d+)\s+age_s=(?P<age>\d+)\s+rx=(?P<rx>\d+)\s+"
        r"auth_fail=(?P<auth>\d+)\s+replay=(?P<replay>\d+)\s+lost=(?P<lost>\d+)\s+"
        r"stream=(?P<stream>ON|OFF)\s+period_ms=(?P<period>\d+)"
        r"(?:\s+rtt_ms=(?P<rtt>-?[0-9.]+)\s+rtt_max_ms=(?P<rtt_max>-?[0-9.]+))?"
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
        if self.BUS_STATS.search(line):
            return await self._bus_stats(line)
        if self.ID_STATS.search(line):
            return await self._id_stats(line)
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
                    failover_auto=(match.group("failover") == "AUTO") if match.group("failover") else None,
                    session_secure=(match.group("session") == "SECURE") if match.group("session") else None,
                )
            )
            return True
        if match := self.WIRELESS_FAILOVER.search(line):
            await self.bus.publish(
                WirelessFailoverReceived(
                    wireless_uuid=match.group("uuid").upper().replace("0X", "0x"),
                    from_node_id=int(match.group("from")),
                    to_node_id=int(match.group("to")),
                    reason=match.group("reason").upper(),
                    rssi_dbm=int(match.group("rssi")) if match.group("rssi") else None,
                )
            )
            return True
        if self.FAULT_TX.search(line):
            payload = parse_key_values(line)
            await self.bus.publish(
                FaultCommandSent(
                    kind=payload.get("KIND", "UNKNOWN").upper(),
                    node_id=_int(payload.get("NODE")),
                    duration_ms=_int(payload.get("DURATION_MS")),
                    seq=_int(payload.get("SEQ")),
                )
            )
            return True
        if self.FAULT_EVENT.search(line):
            payload = parse_key_values(line)
            await self.bus.publish(
                FaultEventReceived(
                    node_id=_int(payload.get("NODE")),
                    kind=payload.get("KIND", "UNKNOWN").upper(),
                    state=payload.get("STATE", "UNKNOWN").upper(),
                    duration_ms=_int(payload.get("DURATION_MS")),
                    seq=_int(payload.get("SEQ")),
                    payload=payload,
                )
            )
            return True
        if match := self.FAULT_ERROR.search(line):
            await self.bus.publish(LogEvent("WARNING", f"Probe 00 recusou o comando: {match.group('detail')}", "ENSAIO"))
            return True
        for pattern, kind, level, template in self.NETWORK_EVENTS:
            if match := pattern.search(line):
                node = int(match.groupdict().get("node") or 0)
                await self.bus.publish(NetworkEventReceived(kind=kind, node_id=node, detail=line))
                await self.bus.publish(LogEvent(level, template.format(node=node), "REDE"))
                if kind == "LEADER_ANNOUNCED":
                    await self.bus.publish(PhysicalNodeReceived(node, {"STATE": "LEADER"}))
                return True
        if match := self.WIRELESS_LINK.search(line):
            rssi = int(match.group("rssi"))
            rtt = float(match.group("rtt")) if match.group("rtt") else -1.0
            rtt_max = float(match.group("rtt_max")) if match.group("rtt_max") else -1.0
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
                    rtt_ms=rtt if rtt >= 0 else None,
                    rtt_max_ms=rtt_max if rtt_max >= 0 else None,
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


    async def _bus_stats(self, line: str) -> bool:
        payload = parse_key_values(line)
        try:
            event = BusStatsReceived(
                probe_ms=int(payload["T_MS"]),
                window_ms=int(payload["WIN_MS"]),
                frames=int(payload["FRAMES"]),
                bits=int(payload["BITS"]),
                load_percent=float(payload["LOAD"]),
                peak_percent=float(payload["PEAK"]),
                rx_errors=_int(payload.get("RX_ERR")),
                tx_errors=_int(payload.get("TX_ERR")),
                error_flags=_int(payload.get("EFLG")),
                rx_queue_peak=_int(payload.get("RX_PEAK")),
                ids=_int(payload.get("IDS")),
                transfers_ok=_int(payload.get("XFER_OK")),
                transfers_err=_int(payload.get("XFER_ERR")),
                own_frames=_int(payload.get("OWN")),
                untracked=_int(payload.get("UNTRACKED")),
            )
        except (KeyError, ValueError):
            return False
        await self.bus.publish(event)
        return True

    async def _id_stats(self, line: str) -> bool:
        payload = parse_key_values(line)
        try:
            event = IdStatsReceived(
                can_id=int(payload["ID"], 0),
                window_ms=int(payload["WIN_MS"]),
                count=int(payload["N"]),
                dlc=int(payload["DLC"]),
                bits=int(payload["BITS"]),
                dt_min_us=int(payload["DT_MIN_US"]),
                dt_avg_us=int(payload["DT_AVG_US"]),
                dt_max_us=int(payload["DT_MAX_US"]),
            )
        except (KeyError, ValueError):
            return False
        await self.bus.publish(event)
        return True

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


def _int(value: object, default: int = 0) -> int:
    try:
        return int(str(value), 0)
    except (TypeError, ValueError):
        return default
