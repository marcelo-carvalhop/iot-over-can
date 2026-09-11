from __future__ import annotations

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import (
    CanFrameReceived,
    CommandAck,
    ConfigurationApplied,
    CrcErrorReceived,
    DtcCleared,
    DtcReceived,
    FragmentReceived,
    GatewayDetected,
    GatewayStatusReceived,
    LogEvent,
    PhysicalNodeReceived,
    SensorStatusReceived,
    TelemetryReceived,
)
from pico_tui.core.models import (
    AcquisitionMode,
    BatteryInfo,
    DataQuality,
    DtcRecord,
    SensorMode,
    Severity,
    TelemetrySample,
)
from pico_tui.protocol.common import first, parse_bool, parse_float, parse_int, parse_key_values
from pico_tui.protocol.legacy_gateway import LegacyGatewayDecoder


class GatewayTextDecoder:
    """Protocolo textual de referência do gateway e compatibilidade Vr1."""

    def __init__(self, bus: EventBus) -> None:
        self.bus = bus
        self.legacy = LegacyGatewayDecoder(bus)

    async def decode(self, line: str) -> None:
        line = line.strip()
        if not line:
            return
        if line.startswith("[") and await self.legacy.decode(line):
            return

        token = line.split(maxsplit=1)[0].upper()
        payload = parse_key_values(line)
        if token in {"PROBE_VERSION", "GW_VERSION"}:
            await self.bus.publish(
                GatewayDetected(
                    firmware_version=payload.get("FIRMWARE", ""),
                    protocol_version=payload.get("PROTOCOL", ""),
                )
            )
            await self.bus.publish(LogEvent("INFO", line, "PROBE"))
        elif token in {"PROBE_STATUS", "GW_STATUS"}:
            await self.bus.publish(GatewayStatusReceived(payload))
            await self.bus.publish(LogEvent("INFO", line, "PROBE"))
        elif token == "NODE":
            parent = parse_int(first(payload, "NODE", "PARENT", "PARENT_NODE"), 0) or 0
            await self.bus.publish(PhysicalNodeReceived(parent, payload))
        elif token == "SENSOR":
            parent = parse_int(first(payload, "NODE", "PARENT", "PARENT_NODE"), 0) or 0
            child = parse_int(first(payload, "CHILD", "CHILD_ID"), 0) or 0
            await self.bus.publish(SensorStatusReceived(parent, child, payload))
        elif token == "TEL":
            await self._telemetry(payload)
        elif token in {"DTC", "DTC_EVENT"}:
            await self._dtc(payload)
        elif token == "DTC_CLEAR":
            parent = parse_int(first(payload, "NODE", "PARENT", "PARENT_NODE"), 0) or 0
            child = parse_int(first(payload, "CHILD", "CHILD_ID"), 0) or 0
            await self.bus.publish(DtcCleared(parent, child, parse_int(payload.get("CODE"))))
        elif token == "CONFIG_APPLIED":
            parent = parse_int(first(payload, "NODE", "PARENT", "PARENT_NODE"), 0) or 0
            child = parse_int(first(payload, "CHILD", "CHILD_ID"), 0) or 0
            await self.bus.publish(ConfigurationApplied(parent, child, payload))
        elif token == "ACK":
            await self.bus.publish(
                CommandAck(
                    command=payload.get("COMMAND", ""),
                    state=payload.get("STATE", "ACK"),
                    transaction_id=payload.get("TX", payload.get("TRANSACTION", "")),
                    payload=payload,
                )
            )
        elif token in {"CAN_RX", "CAN_TX"}:
            await self._can_frame(token, payload)
        elif token == "FRAG":
            await self._fragment(payload)
        elif token == "CRC_ERROR":
            await self.bus.publish(
                CrcErrorReceived(
                    crc_type=payload.get("TYPE", "APPLICATION"),
                    parent_node_id=parse_int(first(payload, "NODE", "PARENT", "PARENT_NODE")),
                    child_id=parse_int(first(payload, "CHILD", "CHILD_ID")),
                    message_type=parse_int(first(payload, "MSG_TYPE", "TYPE_ID")),
                    detail=str(payload),
                )
            )
            await self.bus.publish(LogEvent("ERROR", f"CRC_ERROR {payload}", "GATEWAY"))
        else:
            await self.bus.publish(LogEvent("DEBUG", f"GW_UNPARSED: {line}", "GATEWAY"))

    async def _telemetry(self, payload: dict[str, str]) -> None:
        parent = parse_int(first(payload, "NODE", "PARENT", "PARENT_NODE"), 0) or 0
        child = parse_int(first(payload, "CHILD", "CHILD_ID"), 0) or 0
        quality = _quality(payload)
        battery = _battery(payload)
        fft_valid = parse_bool(first(payload, "FFT_VALID", "FFT"))
        peak_frequency = _scaled(payload, "PEAK_HZ_X10", 10.0, "PEAK_HZ") if fft_valid is not False else None
        peak_amplitude = parse_float(first(payload, "PEAK_AMP", "PEAK_AMPLITUDE")) if fft_valid is not False else None
        entropy = _scaled(payload, "ENTROPY_X1000", 1000.0, "ENT", "ENTROPY") if fft_valid is not False else None

        sample = TelemetrySample(
            parent_node_id=parent,
            child_id=child,
            sensor_timestamp_ms=parse_int(first(payload, "SENSOR_TS_MS", "TS_MS")),
            gateway_timestamp_ms=parse_int(first(payload, "GW_TS_MS", "GATEWAY_TS_MS")),
            sequence=parse_int(first(payload, "SEQ", "SEQUENCE")),
            mode=_sensor_mode(first(payload, "MODE", "FSM")),
            acquisition_mode=_acquisition(first(payload, "ACQ", "ACQUISITION")),
            sample_rate_requested_hz=parse_float(first(payload, "RATE_REQ_HZ", "SAMPLE_RATE_REQUESTED")),
            sample_rate_effective_hz=parse_float(first(payload, "RATE_EFF_HZ", "SAMPLE_RATE_EFFECTIVE")),
            window_type=first(payload, "WINDOW", "WINDOW_TYPE"),
            window_size=parse_int(first(payload, "WIN", "WINDOW_SIZE", "FFT_SIZE")),
            axis=first(payload, "AXIS"),
            fft_valid=fft_valid,
            rms=_scaled(payload, "RMS_MG", 1000.0, "RMS"),
            rms_unit=payload.get("RMS_UNIT", "g"),
            kurtosis=_scaled(payload, "KURT_X100", 100.0, "KURT"),
            crest_factor=_scaled(payload, "CREST_X100", 100.0, "CREST"),
            peak_frequency_hz=peak_frequency,
            peak_amplitude=peak_amplitude,
            spectral_entropy=entropy,
            ppv_mm_s=_scaled(payload, "PPV_UM_S", 1000.0, "PPV_MM_S"),
            stalta_triggered=parse_bool(first(payload, "STA_LTA", "STALTA_TRIGGER")),
            clipping=parse_bool(first(payload, "CLIP", "CLIPPING")),
            dtc_code=parse_int(first(payload, "DTC", "DTC_CODE")),
            dtc_count=parse_int(first(payload, "DTC_COUNT", "ACTIVE_DTC_COUNT")),
            quality=DataQuality.DEGRADED if parse_bool(first(payload, "CLIP", "CLIPPING")) is True else quality,
            battery=battery,
            raw=payload,
        )
        await self.bus.publish(TelemetryReceived(sample))

    async def _dtc(self, payload: dict[str, str]) -> None:
        parent = parse_int(first(payload, "NODE", "PARENT", "PARENT_NODE"), 0) or 0
        child = parse_int(first(payload, "CHILD", "CHILD_ID"), 0) or 0
        severity_raw = str(payload.get("SEVERITY", "INFO")).upper()
        try:
            severity = Severity(severity_raw)
        except ValueError:
            severity_num = parse_int(severity_raw, 0) or 0
            severity = {0: Severity.INFO, 1: Severity.WARNING, 2: Severity.CRITICAL}.get(
                severity_num,
                Severity.INFO,
            )
        record = DtcRecord(
            code=parse_int(payload.get("CODE"), 0) or 0,
            severity=severity,
            symptom=parse_int(payload.get("SYMPTOM")),
            timestamp_ms=parse_int(first(payload, "TS_MS", "TIMESTAMP_MS")),
            raw=payload,
        )
        await self.bus.publish(DtcReceived(parent, child, record))

    async def _can_frame(self, token: str, payload: dict[str, str]) -> None:
        can_id = parse_int(first(payload, "ID", "CAN_ID"), 0) or 0
        data_text = str(payload.get("DATA", "")).replace(" ", "").replace(":", "")
        try:
            data = bytes.fromhex(data_text)
        except ValueError:
            await self.bus.publish(LogEvent("ERROR", f"Payload CAN hexadecimal inválido: {data_text}", "GATEWAY"))
            return
        await self.bus.publish(
            CanFrameReceived(
                can_id=can_id,
                data=data,
                timestamp_ms=parse_int(first(payload, "TS_MS", "TIMESTAMP_MS")),
                direction="RX" if token == "CAN_RX" else "TX",
                fd=parse_bool(payload.get("FD"), True) is not False,
                brs=parse_bool(payload.get("BRS"), True) is not False,
                esi=parse_bool(payload.get("ESI"), False) is True,
            )
        )

    async def _fragment(self, payload: dict[str, str]) -> None:
        parent = parse_int(first(payload, "NODE", "PARENT", "PARENT_NODE"), 0) or 0
        child = parse_int(first(payload, "CHILD", "CHILD_ID"), 0) or 0
        raw_data = payload.get("DATA", "")
        try:
            data = bytes.fromhex(raw_data)
        except ValueError:
            await self.bus.publish(LogEvent("ERROR", "Fragmento com DATA inválido", "GATEWAY"))
            return
        await self.bus.publish(
            FragmentReceived(
                source=(parent, child),
                transfer_type=payload.get("TYPE", "UNKNOWN"),
                transfer_id=parse_int(payload.get("TRANSFER"), 0) or 0,
                fragment_index=parse_int(payload.get("INDEX"), 0) or 0,
                fragment_count=parse_int(payload.get("COUNT"), 0) or 0,
                data=data,
                expected_crc32=parse_int(payload.get("CRC32")),
                metadata={
                    "SAMPLE_RATE_HZ": parse_float(first(payload, "RATE_HZ", "SAMPLE_RATE_HZ")),
                    "FFT_SIZE": parse_int(payload.get("FFT_SIZE")),
                    "WINDOW": payload.get("WINDOW"),
                    "FORMAT": payload.get("FORMAT", "U16_LE"),
                },
            )
        )


def _scaled(payload: dict[str, str], scaled_name: str, divisor: float, *direct_names: str) -> float | None:
    scaled = parse_float(payload.get(scaled_name))
    if scaled is not None:
        return scaled / divisor
    return parse_float(first(payload, *direct_names))


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


def _quality(payload: dict[str, str]) -> DataQuality:
    if parse_bool(payload.get("SIM"), False):
        return DataQuality.SIMULATED
    value = str(first(payload, "QUALITY", "DATA_QUALITY") or "REAL").upper()
    try:
        return DataQuality(value)
    except ValueError:
        return DataQuality.UNKNOWN


def _battery(payload: dict[str, str]) -> BatteryInfo:
    pct = parse_int(first(payload, "BATT_PCT", "BATTERY_PCT"))
    mv = parse_int(first(payload, "BATT_MV", "BATTERY_MV"))
    explicitly_present = parse_bool(payload.get("BATT_PRESENT"))
    if (pct in {None, 255} and mv in {None, 65535}) or explicitly_present is False:
        return BatteryInfo(present=False, source="NOT_INSTRUMENTED", valid=False)
    percentage = float(pct) if pct is not None and 0 <= pct <= 100 else None
    voltage = mv / 1000.0 if mv is not None and 0 <= mv < 65535 else None
    valid = percentage is not None or voltage is not None
    return BatteryInfo(
        present=valid,
        percentage=percentage,
        voltage_v=voltage,
        source=payload.get("BATT_SOURCE", "UNKNOWN" if valid else "NOT_INSTRUMENTED"),
        valid=valid,
        charging=parse_bool(payload.get("CHARGING")),
    )
