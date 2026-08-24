from __future__ import annotations

import asyncio
import struct

from pico_tui.core.event_bus import EventBus
from pico_tui.core.models import ConnectionMode
from pico_tui.core.state_store import StateStore
from pico_tui.protocol.crc import crc32_ieee
from pico_tui.protocol.router import DecoderRouter
from pico_tui.services.controller import DomainController


def test_direct_sensor_updates_canonical_state() -> None:
    async def scenario() -> None:
        bus = EventBus()
        state = StateStore()
        DomainController(bus, state)
        router = DecoderRouter(bus, "auto")
        await router.decode("VERSION PROTOCOL=3 NODE_UUID=0x10A4")
        await router.decode(
            "STATUS NET=DISCOVERY MODE=ROTATING WINDOW=HANN RATE_HZ=250.00 "
            "STALTA=4.000 GAIN=1.000 DTC=0x0000 MPU=YES SIM=NO TELEMETRY=ON "
            "PERIOD_MS=500 DRDY_IRQ=12 DRDY_MISSED=0"
        )
        await router.decode(
            "TEL MODE=ROTATING RMS=0.12345 KURT=0.01230 CREST=3.2100 "
            "PEAK_HZ=50.000 PEAK_AMP=0.019531 ENT=0.0000 PPV_MM_S=0.4123 "
            "STA_LTA=NO CLIP=NO DTC=0x0000"
        )
        snapshot = state.snapshot()
        sensor = snapshot.nodes[1].sensors[1]
        assert snapshot.connection_mode == ConnectionMode.SENSOR_DIRECT
        assert sensor.wireless_uuid == "0x10A4"
        assert sensor.configuration.sample_rate_requested_hz == 250.0
        assert sensor.health.drdy_irq_count == 12
        assert sensor.latest_telemetry is not None
        assert sensor.latest_telemetry.rms == 0.12345
        assert sensor.latest_telemetry.rms_unit == "m/s²"

    asyncio.run(scenario())


def test_gateway_sequence_gap_updates_loss_statistics() -> None:
    async def scenario() -> None:
        bus = EventBus()
        state = StateStore()
        DomainController(bus, state)
        router = DecoderRouter(bus, "gateway")
        await router.decode("TEL node=20 child=1 seq=10 mode=ROTATING quality=REAL rms_mg=100")
        await router.decode("TEL node=20 child=1 seq=13 mode=ROTATING quality=REAL rms_mg=110")
        sensor = state.snapshot().nodes[20].sensors[1]
        assert sensor.rx_count == 2
        assert sensor.lost_count == 2
        assert round(sensor.loss_percent, 2) == 50.0

    asyncio.run(scenario())


def test_fft_fragments_create_spectrum() -> None:
    async def scenario() -> None:
        bus = EventBus()
        state = StateStore()
        DomainController(bus, state)
        router = DecoderRouter(bus, "gateway")
        payload = struct.pack("<4H", 1, 2, 300, 4)
        crc = crc32_ieee(payload)
        await router.decode(
            f"FRAG NODE=20 CHILD=1 TYPE=FFT TRANSFER=7 INDEX=1 COUNT=2 CRC32=0x{crc:08X} "
            f"FORMAT=U16_LE FFT_SIZE=8 DATA={payload[4:].hex()}"
        )
        await router.decode(
            f"FRAG NODE=20 CHILD=1 TYPE=FFT TRANSFER=7 INDEX=0 COUNT=2 CRC32=0x{crc:08X} "
            f"FORMAT=U16_LE FFT_SIZE=8 DATA={payload[:4].hex()}"
        )
        sensor = state.snapshot().nodes[20].sensors[1]
        assert sensor.latest_fft is not None
        assert sensor.latest_fft.magnitudes == [1.0, 2.0, 300.0, 4.0]

    asyncio.run(scenario())


def test_polling_status_and_final_telemetry_fields() -> None:
    async def scenario() -> None:
        bus = EventBus()
        state = StateStore()
        DomainController(bus, state)
        router = DecoderRouter(bus, "sensor")
        await router.decode("VERSION PROTOCOL=4 NODE_UUID=0x10A4")
        await router.decode(
            "STATUS NET=DISCOVERY MODE=STRUCTURAL ACQ=POLLING WINDOW=HANN WINDOW_SIZE=512 "
            "RATE_HZ=1000.00 STALTA=4.000 GAIN=1.000 DTC=0x0000 DTC_COUNT=0 MPU=YES "
            "SIM=NO TELEMETRY=OFF PERIOD_MS=1000 BATT_PCT=255 BATT_MV=65535 DRDY_IRQ=0 DRDY_MISSED=0"
        )
        await router.decode(
            "TEL MODE=STRUCTURAL ACQ=POLLING WIN=512 AXIS=VECTOR FFT_VALID=YES RMS=0.07136 "
            "KURT=2.19474 CREST=3.9580 PEAK_HZ=17.578 PEAK_AMP=0.011725 ENT=0.9221 "
            "PPV_MM_S=1.6673 STA_LTA=NO CLIP=NO BATT_PCT=255 BATT_MV=65535 DTC=0x0000 DTC_COUNT=0"
        )
        sensor = state.snapshot().nodes[1].sensors[1]
        sample = sensor.latest_telemetry
        assert sensor.acquisition_mode.value == "POLLING"
        assert sensor.health.drdy_enabled is False
        assert sensor.health.drdy_irq_count == 0
        assert sample is not None
        assert sample.axis == "VECTOR"
        assert sample.fft_valid is True
        assert sample.window_size == 512
        assert sample.battery.valid is False
        assert sample.battery.percentage is None
        assert sample.dtc_count == 0

    asyncio.run(scenario())


def test_seismic_fft_invalid_is_expected_and_hides_metrics() -> None:
    async def scenario() -> None:
        bus = EventBus()
        state = StateStore()
        DomainController(bus, state)
        router = DecoderRouter(bus, "sensor")
        await router.decode(
            "STATUS NET=BOUND MODE=SEISMIC ACQ=POLLING WINDOW=HANN WINDOW_SIZE=512 RATE_HZ=1000 "
            "STALTA=4 GAIN=1 DTC=0 DTC_COUNT=0 MPU=YES SIM=NO TELEMETRY=ON PERIOD_MS=1000 "
            "BATT_PCT=255 BATT_MV=65535 DRDY_IRQ=0 DRDY_MISSED=0"
        )
        await router.decode(
            "TEL MODE=SEISMIC ACQ=POLLING WIN=512 AXIS=VECTOR FFT_VALID=NO RMS=0.1 KURT=0.2 "
            "CREST=3 PEAK_HZ=0 PEAK_AMP=0 ENT=0 PPV_MM_S=1 STA_LTA=YES CLIP=NO "
            "BATT_PCT=255 BATT_MV=65535 DTC=0 DTC_COUNT=0"
        )
        sample = state.snapshot().nodes[1].sensors[1].latest_telemetry
        assert sample is not None
        assert sample.fft_valid is False
        assert sample.peak_frequency_hz is None
        assert sample.peak_amplitude is None
        assert sample.spectral_entropy is None
        assert sample.quality.value == "REAL"

    asyncio.run(scenario())


def test_configuration_changes_only_after_config_applied() -> None:
    async def scenario() -> None:
        bus = EventBus()
        state = StateStore()
        DomainController(bus, state)
        router = DecoderRouter(bus, "sensor")
        await router.decode(
            "STATUS NET=DISCOVERY MODE=ROTATING ACQ=POLLING WINDOW=HANN WINDOW_SIZE=512 RATE_HZ=250 "
            "STALTA=4 GAIN=1 DTC=0 DTC_COUNT=0 MPU=YES SIM=NO TELEMETRY=OFF PERIOD_MS=1000 "
            "BATT_PCT=255 BATT_MV=65535 DRDY_IRQ=0 DRDY_MISSED=0"
        )
        await router.decode("OK STAGED MODE=STRUCTURAL")
        await router.decode("OK APPLY_QUEUED")
        assert state.snapshot().nodes[1].sensors[1].configuration.mode.value == "ROTATING"
        await router.decode(
            "CONFIG_APPLIED MODE=STRUCTURAL RATE_REQ=1000.00 RATE_EFF=1000.00 "
            "WINDOW=HANN WINDOW_REQ=512 WINDOW_EFF=512 ACQ=POLLING"
        )
        cfg = state.snapshot().nodes[1].sensors[1].configuration
        assert cfg.mode.value == "STRUCTURAL"
        assert cfg.sample_rate_requested_hz == 1000.0
        assert cfg.sample_rate_effective_hz == 1000.0
        assert cfg.window_size == 512
        assert cfg.transaction_state == "APPLIED"

    asyncio.run(scenario())


def test_acq_drdy_rejection_does_not_degrade_polling_sensor() -> None:
    async def scenario() -> None:
        bus = EventBus()
        state = StateStore()
        DomainController(bus, state)
        router = DecoderRouter(bus, "sensor")
        await router.decode(
            "STATUS NET=DISCOVERY MODE=STRUCTURAL ACQ=POLLING WINDOW=HANN WINDOW_SIZE=512 RATE_HZ=1000 "
            "STALTA=4 GAIN=1 DTC=0 DTC_COUNT=0 MPU=YES SIM=NO TELEMETRY=OFF PERIOD_MS=1000 "
            "BATT_PCT=255 BATT_MV=65535 DRDY_IRQ=0 DRDY_MISSED=0"
        )
        await router.decode("ERR DRDY disabled in polling baseline. Use ACQ POLLING.")
        sensor = state.snapshot().nodes[1].sensors[1]
        assert sensor.acquisition_mode.value == "POLLING"
        assert sensor.status.value == "ONLINE"
        assert sensor.quality.value == "REAL"

    asyncio.run(scenario())
