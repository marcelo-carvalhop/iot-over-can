from __future__ import annotations

import re

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import (
    CommandAck,
    ConfigurationApplied,
    DirectSensorStatusReceived,
    DirectSensorVersionReceived,
    DtcCleared,
    DtcReceived,
    LogEvent,
    SensorStatusReceived,
    SpectrumReceived,
    TelemetryReceived,
)
from pico_tui.core.models import (
    AcquisitionMode,
    BatteryInfo,
    DataQuality,
    DtcRecord,
    SensorMode,
    Severity,
    SpectrumSample,
    TelemetrySample,
)
from pico_tui.dtc_catalog import dtc_severity
from pico_tui.protocol.common import parse_bool, parse_float, parse_int, parse_key_values


class SensorDirectDecoder:
    """Decoder do console ASCII do Pico 2 W na baseline POLLING."""

    DEFAULT_PARENT = 1
    DEFAULT_CHILD = 1

    def __init__(self, bus: EventBus) -> None:
        self.bus = bus
        self.uuid = ""
        self.protocol_version = ""
        self.acquisition_mode = AcquisitionMode.UNKNOWN
        self._last_status: dict[str, str] = {}

    def reset(self) -> None:
        self.uuid = ""
        self.protocol_version = ""
        self.acquisition_mode = AcquisitionMode.UNKNOWN
        self._last_status.clear()

    async def decode(self, line: str) -> None:
        line = line.strip()
        if not line or line in {"EDGE>", "EDGE> "}:
            return
        token = line.split(maxsplit=1)[0].upper()

        if token == "VERSION":
            await self._version(line)
        elif token == "STATUS":
            await self._status(line)
        elif token == "CONFIG_APPLIED":
            await self._config_applied(line)
        elif token == "STAGED":
            await self.bus.publish(CommandAck(command=line, state="STAGED", payload=parse_key_values(line)))
            await self.bus.publish(LogEvent("INFO", line, "CONFIG"))
        elif token == "TEL":
            await self._telemetry(line)
        elif token == "FFT":
            await self._fft(line)
        elif token == "DTC_EVENT":
            await self._dtc_event(line)
        elif token == "NET_EVENT":
            payload = parse_key_values(line)
            await self.bus.publish(
                SensorStatusReceived(
                    self.DEFAULT_PARENT,
                    self.DEFAULT_CHILD,
                    {"NET": payload.get("STATE", "UNKNOWN")},
                )
            )
            await self.bus.publish(LogEvent("INFO", line, "SENSOR"))
        elif token == "WARN":
            await self._warning(line)
        elif token == "ERR":
            await self._error(line)
        elif token == "OK":
            await self._ok(line)
        elif token == "DTC":
            await self._dtc_status(line)
        elif token == "NET":
            payload = parse_key_values(line)
            await self.bus.publish(
                SensorStatusReceived(
                    self.DEFAULT_PARENT,
                    self.DEFAULT_CHILD,
                    {"NET": payload.get("STATE", "UNKNOWN")},
                )
            )
            await self.bus.publish(LogEvent("INFO", line, "SENSOR"))
        elif token in {"NOTE", "PONG"}:
            await self.bus.publish(LogEvent("INFO", line, "SENSOR"))
        else:
            # Banner, HELP e extensões desconhecidas permanecem disponíveis.
            level = "DEBUG" if token.isupper() else "INFO"
            await self.bus.publish(LogEvent(level, line, "SENSOR"))

    async def _version(self, line: str) -> None:
        payload = parse_key_values(line)
        self.protocol_version = payload.get("PROTOCOL", "")
        self.uuid = payload.get("NODE_UUID", payload.get("UUID", ""))
        await self.bus.publish(
            DirectSensorVersionReceived(
                protocol_version=self.protocol_version,
                wireless_uuid=self.uuid,
            )
        )
        await self.bus.publish(LogEvent("INFO", line, "SENSOR"))

    async def _status(self, line: str) -> None:
        payload = parse_key_values(line)
        self._last_status = dict(payload)
        simulated = parse_bool(payload.get("SIM"), False) is True
        explicit_acq = payload.get("ACQ", payload.get("ACQUISITION"))
        if simulated:
            self.acquisition_mode = AcquisitionMode.SIM
        elif explicit_acq:
            self.acquisition_mode = _acquisition(explicit_acq)
        elif self.acquisition_mode == AcquisitionMode.UNKNOWN:
            # A baseline final utiliza POLLING. Sem campo explícito, não inferimos DRDY.
            self.acquisition_mode = AcquisitionMode.POLLING if payload.get("MPU", "NO").upper() == "YES" else AcquisitionMode.IDLE
        payload["ACQUISITION"] = self.acquisition_mode.value
        await self.bus.publish(DirectSensorStatusReceived(payload))

        dtc_code = parse_int(payload.get("DTC"), 0) or 0
        dtc_count = parse_int(payload.get("DTC_COUNT"), 0) or 0
        if dtc_code == 0 and dtc_count == 0:
            await self.bus.publish(DtcCleared(self.DEFAULT_PARENT, self.DEFAULT_CHILD, None))
        elif dtc_code:
            await self.bus.publish(
                DtcReceived(
                    self.DEFAULT_PARENT,
                    self.DEFAULT_CHILD,
                    DtcRecord(code=dtc_code, severity=dtc_severity(dtc_code), raw=payload),
                )
            )
        await self.bus.publish(LogEvent("INFO", line, "SENSOR"))

    async def _config_applied(self, line: str) -> None:
        payload = parse_key_values(line)
        if "ACQ" in payload:
            self.acquisition_mode = _acquisition(payload["ACQ"])
        await self.bus.publish(
            ConfigurationApplied(
                self.DEFAULT_PARENT,
                self.DEFAULT_CHILD,
                payload,
            )
        )
        await self.bus.publish(CommandAck(command="APPLY", state="APPLIED", payload=payload))
        await self.bus.publish(LogEvent("INFO", line, "CONFIG"))

    async def _telemetry(self, line: str) -> None:
        payload = parse_key_values(line)
        simulated = parse_bool(self._last_status.get("SIM"), False) is True
        explicit_acq = payload.get("ACQ", payload.get("ACQUISITION"))
        acquisition = _acquisition(explicit_acq) if explicit_acq else self.acquisition_mode
        if simulated or acquisition == AcquisitionMode.SIM:
            acquisition = AcquisitionMode.SIM

        clipping = parse_bool(payload.get("CLIP"))
        quality = DataQuality.SIMULATED if acquisition == AcquisitionMode.SIM else DataQuality.REAL
        if clipping is True:
            quality = DataQuality.DEGRADED

        fft_valid = parse_bool(payload.get("FFT_VALID"))
        peak_frequency = parse_float(payload.get("PEAK_HZ")) if fft_valid is not False else None
        peak_amplitude = parse_float(payload.get("PEAK_AMP")) if fft_valid is not False else None
        entropy = parse_float(payload.get("ENT")) if fft_valid is not False else None

        battery = _battery_from_payload(payload)
        sample = TelemetrySample(
            parent_node_id=self.DEFAULT_PARENT,
            child_id=self.DEFAULT_CHILD,
            sensor_timestamp_ms=parse_int(payload.get("TS_MS")),
            sequence=parse_int(payload.get("SEQ")),
            mode=_sensor_mode(payload.get("MODE")),
            acquisition_mode=acquisition,
            sample_rate_requested_hz=parse_float(self._last_status.get("RATE_HZ")),
            sample_rate_effective_hz=parse_float(
                self._last_status.get("RATE_EFF_HZ", self._last_status.get("RATE_HZ"))
            ),
            window_type=self._last_status.get("WINDOW"),
            window_size=parse_int(payload.get("WIN"), parse_int(self._last_status.get("WINDOW_SIZE"), 512)),
            axis=payload.get("AXIS"),
            fft_valid=fft_valid,
            rms=parse_float(payload.get("RMS")),
            rms_unit=payload.get("RMS_UNIT", "m/s²"),
            kurtosis=parse_float(payload.get("KURT")),
            crest_factor=parse_float(payload.get("CREST")),
            peak_frequency_hz=peak_frequency,
            peak_amplitude=peak_amplitude,
            spectral_entropy=entropy,
            ppv_mm_s=parse_float(payload.get("PPV_MM_S")),
            stalta_triggered=parse_bool(payload.get("STA_LTA")),
            clipping=clipping,
            dtc_code=parse_int(payload.get("DTC")),
            dtc_count=parse_int(payload.get("DTC_COUNT")),
            quality=quality,
            battery=battery,
            raw=payload,
        )
        await self.bus.publish(TelemetryReceived(sample))

    async def _fft(self, line: str) -> None:
        payload = parse_key_values(line)
        valid = parse_bool(payload.get("VALID"), False) is True
        if not valid:
            await self.bus.publish(LogEvent("INFO", "FFT indisponível para o modo atual", "FFT"))
            return
        values = _parse_fft_values(payload.get("VALUES", ""))
        declared_bins = parse_int(payload.get("BINS"))
        if declared_bins is not None and declared_bins != len(values):
            await self.bus.publish(
                LogEvent(
                    "WARNING",
                    f"FFT declarou {declared_bins} bins, mas forneceu {len(values)} valores",
                    "FFT",
                )
            )
        sample = SpectrumSample(
            parent_node_id=self.DEFAULT_PARENT,
            child_id=self.DEFAULT_CHILD,
            magnitudes=values,
            sample_rate_hz=parse_float(self._last_status.get("RATE_EFF_HZ", self._last_status.get("RATE_HZ"))),
            fft_size=parse_int(self._last_status.get("WINDOW_SIZE"), len(values) * 2),
            window_type=self._last_status.get("WINDOW"),
            magnitude_unit=payload.get("UNIT", "raw"),
            valid=True,
        )
        await self.bus.publish(SpectrumReceived(sample))
        await self.bus.publish(LogEvent("INFO", f"FFT recebida: {len(values)} bins", "FFT"))

    async def _dtc_event(self, line: str) -> None:
        payload = parse_key_values(line)
        code = parse_int(payload.get("CODE"), 0) or 0
        severity_value = parse_int(payload.get("SEVERITY"))
        severity = (
            {0: Severity.INFO, 1: Severity.WARNING, 2: Severity.CRITICAL}.get(severity_value, dtc_severity(code))
            if severity_value is not None
            else dtc_severity(code)
        )
        record = DtcRecord(
            code=code,
            symptom=parse_int(payload.get("SYMPTOM")),
            severity=severity,
            timestamp_ms=parse_int(payload.get("TS_MS")),
            raw=payload,
        )
        await self.bus.publish(DtcReceived(self.DEFAULT_PARENT, self.DEFAULT_CHILD, record))
        await self.bus.publish(LogEvent(severity.value, line, "DTC"))

    async def _dtc_status(self, line: str) -> None:
        payload = parse_key_values(line)
        code = parse_int(payload.get("ACTIVE", payload.get("CODE")), 0) or 0
        count = parse_int(payload.get("COUNT", payload.get("DTC_COUNT")), 0) or 0
        if code == 0 and count == 0:
            await self.bus.publish(DtcCleared(self.DEFAULT_PARENT, self.DEFAULT_CHILD, None))
        elif code:
            await self.bus.publish(
                DtcReceived(
                    self.DEFAULT_PARENT,
                    self.DEFAULT_CHILD,
                    DtcRecord(code=code, severity=dtc_severity(code), raw=payload),
                )
            )
        await self.bus.publish(LogEvent("INFO", line, "DTC"))

    async def _ok(self, line: str) -> None:
        payload = parse_key_values(line)
        action = line.split(maxsplit=1)[1] if len(line.split(maxsplit=1)) == 2 else ""
        action_upper = action.upper()
        state = "ACK"
        if action_upper.startswith("STAGED"):
            state = "STAGED"
        elif action_upper.startswith("APPLY_QUEUED") or "APPLY_QUEUED" in action_upper:
            state = "QUEUED"
        elif action_upper.startswith("APPLY"):
            # ACK de fila/recebimento; aplicação real vem em CONFIG_APPLIED.
            state = "QUEUED"

        if "TELEMETRY" in payload:
            self._last_status["TELEMETRY"] = payload["TELEMETRY"]
        if "PERIOD_MS" in payload:
            self._last_status["PERIOD_MS"] = payload["PERIOD_MS"]
        if "SIMULATE" in payload:
            enabled = parse_bool(payload["SIMULATE"], False) is True
            self._last_status["SIM"] = "YES" if enabled else "NO"
            self.acquisition_mode = AcquisitionMode.SIM if enabled else AcquisitionMode.POLLING
        if payload.get("ACQ", "").upper() == "POLLING_RESTARTED" or "POLLING_RESTARTED" in action_upper:
            self.acquisition_mode = AcquisitionMode.POLLING
            self._last_status["ACQ"] = "POLLING"
        if "DTC" in action_upper and ("CLEAR" in action_upper or payload.get("DTC", "").upper() in {"CLEAR", "CLEARED"}):
            await self.bus.publish(DtcCleared(self.DEFAULT_PARENT, self.DEFAULT_CHILD, None))
        if "NET_WIFI" in payload or "STATE" in payload:
            net_state = payload.get("STATE", "UNKNOWN")
            self._last_status["NET"] = net_state
            await self.bus.publish(
                SensorStatusReceived(
                    self.DEFAULT_PARENT,
                    self.DEFAULT_CHILD,
                    {"NET": net_state, "WIFI": payload.get("NET_WIFI", "UNKNOWN")},
                )
            )

        # ACKs podem atualizar apenas estados auxiliares; nunca promovem configuração estagiada.
        if any(key in payload for key in {"TELEMETRY", "PERIOD_MS", "SIMULATE", "ACQ", "NET_WIFI"}):
            snapshot = dict(self._last_status)
            snapshot["ACQUISITION"] = self.acquisition_mode.value
            await self.bus.publish(DirectSensorStatusReceived(snapshot))
        await self.bus.publish(CommandAck(command=action, state=state, payload=payload))
        await self.bus.publish(LogEvent("INFO", line, "SENSOR"))

    async def _error(self, line: str) -> None:
        upper = line.upper()
        if "DRDY DISABLED IN POLLING BASELINE" in upper or ("ACQ DRDY" in upper and "POLLING" in upper):
            await self.bus.publish(CommandAck(command="ACQ DRDY", state="REJECTED", payload={"REASON": line[4:]}))
            await self.bus.publish(LogEvent("WARNING", line, "COMMAND"))
            return
        if "I2C_POLLING_READ_FAILED" in upper:
            # Falha de leitura é defeito do sensor/barramento; POLLING continua sendo o modo nominal.
            self.acquisition_mode = AcquisitionMode.POLLING
            await self.bus.publish(
                SensorStatusReceived(
                    self.DEFAULT_PARENT,
                    self.DEFAULT_CHILD,
                    {"ACQUISITION": AcquisitionMode.POLLING.value, "QUALITY": DataQuality.DEGRADED.value},
                )
            )
        await self.bus.publish(CommandAck(command=line[4:], state="FAILED", payload={"REASON": line[4:]}))
        await self.bus.publish(LogEvent("ERROR", line, "SENSOR"))

    async def _warning(self, line: str) -> None:
        if "ENTERING_STABLE_I2C_POLLING_MODE" in line or "STILL_USING_I2C_POLLING_MODE" in line:
            self.acquisition_mode = AcquisitionMode.POLLING
            await self.bus.publish(
                SensorStatusReceived(
                    self.DEFAULT_PARENT,
                    self.DEFAULT_CHILD,
                    {"ACQUISITION": self.acquisition_mode.value},
                )
            )
            await self.bus.publish(LogEvent("INFO", line, "SENSOR"))
            return
        await self.bus.publish(LogEvent("WARNING", line, "SENSOR"))


def _sensor_mode(value: object) -> SensorMode:
    try:
        return SensorMode(str(value or "UNKNOWN").upper())
    except ValueError:
        return SensorMode.UNKNOWN


def _acquisition(value: object) -> AcquisitionMode:
    normalized = str(value or "UNKNOWN").upper()
    aliases = {
        "SIMULATED": "SIM",
        "SIMULATION": "SIM",
        "STOPPED": "IDLE",
        "OFF": "IDLE",
        "POLLING_RESTARTED": "POLLING",
    }
    normalized = aliases.get(normalized, normalized)
    try:
        return AcquisitionMode(normalized)
    except ValueError:
        return AcquisitionMode.UNKNOWN


def _battery_from_payload(payload: dict[str, str]) -> BatteryInfo:
    pct_raw = parse_int(payload.get("BATT_PCT"))
    mv_raw = parse_int(payload.get("BATT_MV"))
    if pct_raw in {None, 255} and mv_raw in {None, 65535}:
        return BatteryInfo(present=False, source="NOT_INSTRUMENTED", valid=False)
    pct = float(pct_raw) if pct_raw is not None and 0 <= pct_raw <= 100 else None
    voltage = mv_raw / 1000.0 if mv_raw is not None and 0 <= mv_raw < 65535 else None
    valid = pct is not None or voltage is not None
    return BatteryInfo(
        present=valid,
        percentage=pct,
        voltage_v=voltage,
        source=payload.get("BATT_SOURCE", "ADC" if valid else "NOT_INSTRUMENTED"),
        valid=valid,
    )


def _parse_fft_values(raw: str) -> list[float]:
    if not raw.strip():
        return []
    values: list[float] = []
    for token in re.split(r"[\s,;]+", raw.strip()):
        if not token:
            continue
        value = parse_float(token)
        if value is not None:
            values.append(value)
    return values
