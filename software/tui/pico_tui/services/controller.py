from __future__ import annotations

import struct

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import (
    CanFrameReceived,
    ConnectionClosed,
    ConnectionModeDetected,
    ConnectionOpened,
    ConfigurationApplied,
    CrcErrorReceived,
    DirectSensorStatusReceived,
    DirectSensorVersionReceived,
    DtcCleared,
    DtcReceived,
    FragmentReceived,
    GatewayDetected,
    GatewayStatusReceived,
    LegacyHeartbeatReceived,
    LogEvent,
    PhysicalNodeReceived,
    SensorStatusReceived,
    SpectrumReceived,
    TelemetryReceived,
    TransferCompleted,
    TransferFailed,
)
from pico_tui.core.models import (
    AcquisitionMode,
    CanFrameRecord,
    ConnectionState,
    DataQuality,
    DtcRecord,
    NodeStatus,
    SensorMode,
    SpectrumSample,
)
from pico_tui.core.state_store import StateStore
from pico_tui.dtc_catalog import dtc_severity
from pico_tui.protocol.can_id import decode_can_id
from pico_tui.protocol.common import parse_bool, parse_float, parse_int
from pico_tui.protocol.fragments import FragmentReassembler
from pico_tui.protocol.sequence import SequenceTracker


class DomainController:
    """Aplica eventos de protocolo ao estado canônico da aplicação."""

    def __init__(self, bus: EventBus, state: StateStore) -> None:
        self.bus = bus
        self.state = state
        self.sequence = SequenceTracker()
        self.fragments = FragmentReassembler(timeout_seconds=5.0)
        self._fragment_meta: dict[tuple[object, ...], dict[str, object]] = {}
        self._direct_uuid = ""
        self._subscribe()

    def _subscribe(self) -> None:
        for event_type, handler in (
            (ConnectionOpened, self._connection_opened),
            (ConnectionClosed, self._connection_closed),
            (ConnectionModeDetected, self._mode_detected),
            (GatewayDetected, self._gateway_detected),
            (GatewayStatusReceived, self._gateway_status),
            (PhysicalNodeReceived, self._physical_node),
            (SensorStatusReceived, self._sensor_status),
            (DirectSensorVersionReceived, self._direct_version),
            (DirectSensorStatusReceived, self._direct_status),
            (ConfigurationApplied, self._configuration_applied),
            (TelemetryReceived, self._telemetry),
            (SpectrumReceived, self._spectrum),
            (DtcReceived, self._dtc),
            (DtcCleared, self._dtc_cleared),
            (CrcErrorReceived, self._crc_error),
            (CanFrameReceived, self._can_frame),
            (LegacyHeartbeatReceived, self._legacy_heartbeat),
            (FragmentReceived, self._fragment),
        ):
            self.bus.subscribe(event_type, handler)

    async def _connection_opened(self, event: ConnectionOpened) -> None:
        self.sequence.reset()
        self.fragments.clear()
        self._fragment_meta.clear()
        self.state.set_connection(ConnectionState.SYNCHRONIZING, port=event.port)

    async def _connection_closed(self, event: ConnectionClosed) -> None:
        self.state.set_connection(ConnectionState.DISCONNECTED)

    async def _mode_detected(self, event: ConnectionModeDetected) -> None:
        self.state.set_connection(ConnectionState.READY, mode=event.mode)

    async def _gateway_detected(self, event: GatewayDetected) -> None:
        self.state.update_gateway(
            firmware_version=event.firmware_version,
            protocol_version=event.protocol_version,
        )
        self.state.update_node(0, node_type="GATEWAY", status=NodeStatus.ONLINE)

    async def _gateway_status(self, event: GatewayStatusReceived) -> None:
        payload = event.payload
        self.state.update_gateway(
            node_id=parse_int(payload.get("NODE"), 0) or 0,
            uptime_ms=parse_int(payload.get("UPTIME_MS"), parse_int(payload.get("UPTIME"))),
            can_state=payload.get("CAN", payload.get("CAN_STATE", "UNKNOWN")),
            wifi_state=payload.get("WIFI", payload.get("WIFI_STATE", "UNKNOWN")),
            arbitration_bitrate=parse_int(payload.get("ARB"), parse_int(payload.get("ARBITRATION_BITRATE"))),
            data_bitrate=parse_int(payload.get("DATA"), parse_int(payload.get("DATA_BITRATE"))),
        )
        self.state.update_network(
            bus_off=parse_bool(payload.get("BUS_OFF"), False) is True,
            error_passive=parse_bool(payload.get("ERROR_PASSIVE"), False) is True,
            error_warning=parse_bool(payload.get("ERROR_WARNING"), False) is True,
            utilization_percent=parse_float(payload.get("UTILIZATION")),
        )
        self.state.update_node(
            0,
            node_type="GATEWAY",
            can_state=payload.get("CAN", "UNKNOWN"),
            wifi_state=payload.get("WIFI", "UNKNOWN"),
            status=NodeStatus.ONLINE,
        )

    async def _physical_node(self, event: PhysicalNodeReceived) -> None:
        payload = event.payload
        raw_state = str(payload.get("STATE", payload.get("STATUS", "ONLINE"))).upper()
        self.state.update_node(
            event.parent_node_id,
            node_type=payload.get("TYPE", "CAN_NODE"),
            firmware_version=payload.get("FIRMWARE", ""),
            protocol_version=payload.get("PROTOCOL", ""),
            can_state=payload.get("CAN", raw_state),
            wifi_state=payload.get("WIFI", "UNKNOWN"),
            uptime_ms=parse_int(payload.get("UPTIME_MS")),
            status=_node_status(raw_state),
        )

    async def _sensor_status(self, event: SensorStatusReceived) -> None:
        payload = event.payload
        sensor = self.state.ensure_sensor(
            event.parent_node_id,
            event.child_id,
            wireless_uuid=payload.get("UUID", payload.get("NODE_UUID", "")),
        )
        values: dict[str, object] = {}
        if "STATE" in payload or "STATUS" in payload:
            values["status"] = _node_status(payload.get("STATE", payload.get("STATUS", "UNKNOWN")))
        if "MODE" in payload:
            values["sensor_mode"] = _sensor_mode(payload["MODE"])
        if "ACQ" in payload or "ACQUISITION" in payload:
            values["acquisition_mode"] = _acquisition(payload.get("ACQ", payload.get("ACQUISITION")))
        if "QUALITY" in payload:
            values["quality"] = _quality(payload["QUALITY"])
        if "UUID" in payload or "NODE_UUID" in payload:
            values["wireless_uuid"] = payload.get("UUID", payload.get("NODE_UUID", sensor.wireless_uuid))
        self.state.update_sensor(event.parent_node_id, event.child_id, **values)
        if "NET" in payload:
            self.state.update_sensor_health(event.parent_node_id, event.child_id, network_state=str(payload["NET"]))

    async def _direct_version(self, event: DirectSensorVersionReceived) -> None:
        self._direct_uuid = event.wireless_uuid
        self.state.update_node(1, node_type="DIRECT_SENSOR_HOST", status=NodeStatus.ONLINE)
        self.state.ensure_sensor(1, 1, wireless_uuid=event.wireless_uuid)
        self.state.update_sensor(1, 1, protocol_version=event.protocol_version)

    async def _direct_status(self, event: DirectSensorStatusReceived) -> None:
        payload = event.payload
        simulated = parse_bool(payload.get("SIM"), False) is True
        quality = DataQuality.SIMULATED if simulated else DataQuality.REAL
        mode = _sensor_mode(payload.get("MODE"))
        acquisition = _acquisition(payload.get("ACQUISITION", payload.get("ACQ")))
        self.state.ensure_sensor(1, 1, wireless_uuid=self._direct_uuid)
        self.state.update_sensor(
            1,
            1,
            status=NodeStatus.ONLINE,
            acquisition_mode=acquisition,
            sensor_mode=mode,
            quality=quality,
            reported_dtc_count=parse_int(payload.get("DTC_COUNT")),
        )
        period_ms = parse_float(payload.get("PERIOD_MS"))
        self.state.update_sensor_configuration(
            1,
            1,
            mode=mode,
            sample_rate_requested_hz=parse_float(payload.get("RATE_HZ")),
            sample_rate_effective_hz=parse_float(payload.get("RATE_EFF_HZ"), parse_float(payload.get("RATE_HZ"))),
            window_type=payload.get("WINDOW"),
            window_size=parse_int(payload.get("WINDOW_SIZE"), 512),
            stalta_threshold=parse_float(payload.get("STALTA")),
            calibration_gain=parse_float(payload.get("GAIN")),
            telemetry_rate_hz=(1000.0 / period_ms if period_ms not in {None, 0} else None),
            simulation_enabled=simulated,
        )
        self.state.update_sensor_health(
            1,
            1,
            mpu_present=parse_bool(payload.get("MPU")),
            drdy_irq_count=parse_int(payload.get("DRDY_IRQ")),
            drdy_missed_count=parse_int(payload.get("DRDY_MISSED")),
            drdy_enabled=acquisition == AcquisitionMode.DRDY,
            network_state=str(payload.get("NET", "UNKNOWN")),
            telemetry_enabled=str(payload.get("TELEMETRY", "OFF")).upper() == "ON",
            telemetry_period_ms=parse_int(payload.get("PERIOD_MS")),
        )
        dtc_code = parse_int(payload.get("DTC"), 0) or 0
        dtc_count = parse_int(payload.get("DTC_COUNT"), 0) or 0
        if dtc_code == 0 and dtc_count == 0:
            self.state.clear_dtc(1, 1, None)
        elif dtc_code:
            self.state.add_dtc(1, 1, DtcRecord(dtc_code, dtc_severity(dtc_code), raw=payload))

    async def _configuration_applied(self, event: ConfigurationApplied) -> None:
        payload = event.payload
        mode = _sensor_mode(payload.get("MODE"))
        acquisition = _acquisition(payload.get("ACQ", payload.get("ACQUISITION")))
        window_type = payload.get("WINDOW", payload.get("WINDOW_TYPE"))
        window_size = _first_int(payload, "WINDOW_SIZE", "WINDOW_EFF", "WIN")
        # Alguns firmwares usam WINDOW_REQ/EFF para o tamanho da janela.
        if window_type and _as_int(window_type) is not None:
            window_size = _as_int(window_type)
            window_type = None
        self.state.update_sensor_configuration(
            event.parent_node_id,
            event.child_id,
            mode=mode,
            sample_rate_requested_hz=_first_float(payload, "RATE_REQ", "RATE_REQ_HZ", "RATE_HZ"),
            sample_rate_effective_hz=_first_float(payload, "RATE_EFF", "RATE_EFF_HZ"),
            window_type=window_type,
            window_size=window_size,
            stalta_threshold=parse_float(payload.get("STALTA")),
            calibration_gain=parse_float(payload.get("GAIN")),
            transaction_state="APPLIED",
        )
        values: dict[str, object] = {"status": NodeStatus.ONLINE}
        if mode != SensorMode.UNKNOWN:
            values["sensor_mode"] = mode
        if acquisition != AcquisitionMode.UNKNOWN:
            values["acquisition_mode"] = acquisition
        self.state.update_sensor(event.parent_node_id, event.child_id, **values)
        self.state.update_sensor_health(
            event.parent_node_id,
            event.child_id,
            drdy_enabled=acquisition == AcquisitionMode.DRDY if acquisition != AcquisitionMode.UNKNOWN else None,
        )

    async def _telemetry(self, event: TelemetryReceived) -> None:
        sample = event.sample
        if sample.sequence is not None:
            key = (sample.parent_node_id, sample.child_id, "TEL")
            result = self.sequence.update(key, sample.sequence)
            self.state.record_sequence(
                sample.parent_node_id,
                sample.child_id,
                sequence=sample.sequence,
                lost=result.lost,
                duplicate=result.duplicate,
                out_of_order=result.out_of_order,
            )
            if result.lost:
                await self.bus.publish(
                    LogEvent("WARNING", f"{sample.logical_id}: perda estimada de {result.lost} mensagem(ns)", "SEQUENCE")
                )
            elif result.duplicate:
                await self.bus.publish(LogEvent("WARNING", f"{sample.logical_id}: sequência duplicada {sample.sequence}", "SEQUENCE"))
            elif result.out_of_order:
                await self.bus.publish(LogEvent("WARNING", f"{sample.logical_id}: sequência fora de ordem {sample.sequence}", "SEQUENCE"))
        self.state.update_telemetry(sample)
        self.state.update_sensor(
            sample.parent_node_id,
            sample.child_id,
            reported_dtc_count=sample.dtc_count,
        )
        if sample.dtc_code in {None, 0} and sample.dtc_count == 0:
            self.state.clear_dtc(sample.parent_node_id, sample.child_id, None)
        elif sample.dtc_code not in {None, 0}:
            code = sample.dtc_code or 0
            self.state.add_dtc(
                sample.parent_node_id,
                sample.child_id,
                DtcRecord(code, dtc_severity(code), raw=sample.raw),
            )

    async def _spectrum(self, event: SpectrumReceived) -> None:
        self.state.update_spectrum(event.sample)

    async def _dtc(self, event: DtcReceived) -> None:
        self.state.add_dtc(event.parent_node_id, event.child_id, event.record)

    async def _dtc_cleared(self, event: DtcCleared) -> None:
        self.state.clear_dtc(event.parent_node_id, event.child_id, event.code)

    async def _crc_error(self, event: CrcErrorReceived) -> None:
        self.state.increment_network(crc_errors=1)

    async def _can_frame(self, event: CanFrameReceived) -> None:
        if event.direction == "RX":
            self.state.increment_network(frames_rx=1, bytes_rx=len(event.data))
        else:
            self.state.increment_network(frames_tx=1, bytes_tx=len(event.data))
        self.state.add_can_frame(
            CanFrameRecord(
                can_id=event.can_id,
                data=event.data,
                timestamp_ms=event.timestamp_ms,
                direction=event.direction,
                fd=event.fd,
                brs=event.brs,
                esi=event.esi,
            )
        )
        try:
            fields = decode_can_id(event.can_id)
            await self.bus.publish(
                LogEvent(
                    "DEBUG",
                    f"CAN {event.direction} 0x{event.can_id:08X} {fields.logical_id} "
                    f"type=0x{fields.message_type:02X} data={event.data.hex(' ')}",
                    "CAN",
                )
            )
        except ValueError:
            await self.bus.publish(LogEvent("ERROR", f"CAN ID inválido: 0x{event.can_id:X}", "CAN"))

    async def _legacy_heartbeat(self, event: LegacyHeartbeatReceived) -> None:
        self.state.update_node(event.leader, node_type="LEGACY_CAN_NODE", status=NodeStatus.ONLINE, legacy_last_round=event.round_number)
        self.state.set_last_action(
            f"Heartbeat líder={event.leader} rodada={event.round_number} período={event.period_ms} ms"
        )

    async def _fragment(self, event: FragmentReceived) -> None:
        key = (*event.source, event.transfer_type, event.transfer_id)
        self._fragment_meta.setdefault(key, {}).update(event.metadata)
        result = self.fragments.add(
            key,
            fragment_index=event.fragment_index,
            fragment_count=event.fragment_count,
            data=event.data,
            expected_crc32=event.expected_crc32,
        )
        self.state.update_network(active_transfers=self.fragments.active_count)
        if result.status == "COMPLETE":
            metadata = self._fragment_meta.pop(key, {})
            await self.bus.publish(
                TransferCompleted(event.source, event.transfer_type, event.transfer_id, result.payload or b"", metadata)
            )
            await self._consume_transfer(event.source, event.transfer_type, event.transfer_id, result.payload or b"", metadata)
        elif result.status == "FAILED":
            self._fragment_meta.pop(key, None)
            await self.bus.publish(TransferFailed(event.source, event.transfer_type, event.transfer_id, result.reason))
        elif result.status == "DUPLICATE":
            await self.bus.publish(LogEvent("WARNING", f"Fragmento duplicado transfer={event.transfer_id}", "FRAGMENT"))

    async def expire_fragment_transfers(self) -> None:
        """Expira transferências incompletas mesmo quando nenhum novo fragmento chega."""

        for key in self.fragments.expire():
            self._fragment_meta.pop(key, None)
            parent_node_id, child_id, transfer_type, transfer_id = key
            await self.bus.publish(
                TransferFailed(
                    (int(parent_node_id), int(child_id)),
                    str(transfer_type),
                    int(transfer_id),
                    "timeout de remontagem",
                )
            )
        self.state.update_network(active_transfers=self.fragments.active_count)

    async def _consume_transfer(
        self,
        source: tuple[int, int],
        transfer_type: str,
        transfer_id: int,
        payload: bytes,
        metadata: dict[str, object],
    ) -> None:
        if transfer_type.upper() != "FFT":
            return
        fmt = str(metadata.get("FORMAT", "U16_LE")).upper()
        try:
            if fmt == "F32_LE":
                if len(payload) % 4:
                    raise ValueError("payload F32 não múltiplo de 4")
                magnitudes = list(struct.unpack(f"<{len(payload) // 4}f", payload))
                unit = "float"
            else:
                if len(payload) % 2:
                    raise ValueError("payload U16 não múltiplo de 2")
                magnitudes = [float(v) for v in struct.unpack(f"<{len(payload) // 2}H", payload)]
                unit = "u16"
        except (struct.error, ValueError) as exc:
            await self.bus.publish(TransferFailed(source, transfer_type, transfer_id, str(exc)))
            return
        sample = SpectrumSample(
            parent_node_id=source[0],
            child_id=source[1],
            magnitudes=magnitudes,
            transfer_id=transfer_id,
            sample_rate_hz=_as_float(metadata.get("SAMPLE_RATE_HZ")),
            fft_size=_as_int(metadata.get("FFT_SIZE")) or len(magnitudes) * 2,
            window_type=str(metadata.get("WINDOW") or ""),
            magnitude_unit=unit,
        )
        await self.bus.publish(SpectrumReceived(sample))


def _node_status(value: object) -> NodeStatus:
    normalized = str(value or "UNKNOWN").upper()
    aliases = {
        "ATIVO": NodeStatus.ONLINE,
        "LIDER": NodeStatus.ONLINE,
        "LEADER": NodeStatus.ONLINE,
        "GATEWAY": NodeStatus.ONLINE,
        "DESATIVADO": NodeStatus.OFFLINE,
        "FALHA": NodeStatus.LOST,
        "ENTRANDO": NodeStatus.AGING,
    }
    if normalized in aliases:
        return aliases[normalized]
    try:
        return NodeStatus(normalized)
    except ValueError:
        return NodeStatus.UNKNOWN


def _sensor_mode(value: object) -> SensorMode:
    try:
        return SensorMode(str(value or "UNKNOWN").upper())
    except ValueError:
        return SensorMode.UNKNOWN


def _acquisition(value: object) -> AcquisitionMode:
    normalized = str(value or "UNKNOWN").upper()
    normalized = {"SIMULATED": "SIM", "STOPPED": "IDLE", "POLLING_RESTARTED": "POLLING"}.get(normalized, normalized)
    try:
        return AcquisitionMode(normalized)
    except ValueError:
        return AcquisitionMode.UNKNOWN


def _quality(value: object) -> DataQuality:
    try:
        return DataQuality(str(value or "UNKNOWN").upper())
    except ValueError:
        return DataQuality.UNKNOWN


def _as_float(value: object) -> float | None:
    try:
        return float(value) if value is not None else None
    except (TypeError, ValueError):
        return None


def _as_int(value: object) -> int | None:
    try:
        return int(value) if value is not None else None
    except (TypeError, ValueError):
        return None


def _first_float(payload: dict[str, object], *keys: str) -> float | None:
    for key in keys:
        value = _as_float(payload.get(key))
        if value is not None:
            return value
    return None


def _first_int(payload: dict[str, object], *keys: str) -> int | None:
    for key in keys:
        value = _as_int(payload.get(key))
        if value is not None:
            return value
    return None
