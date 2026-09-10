from __future__ import annotations

import asyncio

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import CanFrameReceived, TelemetryReceived
from pico_tui.protocol.can_id import CanIdFields, decode_can_id, encode_can_id
from pico_tui.protocol.common import parse_key_values
from pico_tui.protocol.crc import crc32_ieee
from pico_tui.protocol.fragments import FragmentReassembler
from pico_tui.protocol.gateway_text import GatewayTextDecoder
from pico_tui.protocol.legacy_gateway import LegacyGatewayDecoder
from pico_tui.protocol.sequence import SequenceTracker


def test_can_id_round_trip() -> None:
    fields = CanIdFields(priority=3, domain=7, parent_node_id=20, child_id=1, message_type=0x42)
    assert decode_can_id(encode_can_id(fields)) == fields


def test_sequence_gap_duplicate_out_of_order_and_rollover() -> None:
    tracker = SequenceTracker()
    key = (20, 1, "TEL")
    assert tracker.update(key, 65534).classification == "FIRST"
    assert tracker.update(key, 65535).classification == "OK"
    assert tracker.update(key, 0).classification == "OK"
    assert tracker.update(key, 2).lost == 1
    assert tracker.update(key, 2).duplicate is True
    assert tracker.update(key, 1).out_of_order is True


def test_fragment_reassembly_out_of_order() -> None:
    payload = b"abcdefghij"
    reassembler = FragmentReassembler()
    crc = crc32_ieee(payload)
    assert reassembler.add(
        (20, 1, "FFT", 7),
        fragment_index=1,
        fragment_count=2,
        data=b"fghij",
        expected_crc32=crc,
    ).status == "PENDING"
    result = reassembler.add(
        (20, 1, "FFT", 7),
        fragment_index=0,
        fragment_count=2,
        data=b"abcde",
        expected_crc32=crc,
    )
    assert result.status == "COMPLETE"
    assert result.payload == payload


def test_key_value_parser_accepts_unquoted_hex_data() -> None:
    payload = parse_key_values("CAN_RX ID=0x123 DATA=01 02 A0 FF FD=YES")
    assert payload["DATA"] == "01 02 A0 FF"
    assert payload["FD"] == "YES"


def test_gateway_scaled_telemetry() -> None:
    async def scenario() -> None:
        bus = EventBus()
        decoder = GatewayTextDecoder(bus)
        events = []
        bus.subscribe(TelemetryReceived, events.append)
        await decoder.decode(
            "TEL node=20 child=1 seq=7 mode=ROTATING acq=DRDY "
            "rms_mg=125 kurt_x100=25 crest_x100=310 peak_hz_x10=500 "
            "entropy_x1000=700 ppv_um_s=450 batt_present=YES batt_mv=3920 batt_pct=88 quality=REAL"
        )
        sample = events[0].sample
        assert sample.logical_id == "20.01"
        assert sample.rms == 0.125
        assert sample.rms_unit == "g"
        assert sample.kurtosis == 0.25
        assert sample.peak_frequency_hz == 50.0
        assert sample.ppv_mm_s == 0.45
        assert sample.battery.voltage_v == 3.92

    asyncio.run(scenario())


def test_legacy_raw_status_frame() -> None:
    async def scenario() -> None:
        bus = EventBus()
        decoder = LegacyGatewayDecoder(bus)
        frames = []
        bus.subscribe(CanFrameReceived, frames.append)
        parsed = await decoder.decode("[120 ms] RX ID=0x080 DLC=4 DATA=23 21 03 01")
        assert parsed is True
        assert frames[0].can_id == 0x080
        assert frames[0].data == bytes.fromhex("23 21 03 01")

    asyncio.run(scenario())


def test_fragment_reassembly_expires_without_new_fragment() -> None:
    key = (20, 1, "FFT", 0x42A1)
    reassembler = FragmentReassembler(timeout_seconds=0.0)
    result = reassembler.add(
        key,
        fragment_index=0,
        fragment_count=2,
        data=b"partial",
    )
    assert result.status == "PENDING"
    assert reassembler.expire() == [key]
    assert reassembler.active_count == 0


def test_spectrum_chart_uses_frequency_axis_and_magnitude_unit() -> None:
    from pico_tui.core.models import SpectrumSample
    from pico_tui.screens import _spectrum_chart, _spectrum_metadata, _spectrum_peaks

    values = [0.001] * 64
    values[9] = 0.011725
    spectrum = SpectrumSample(
        parent_node_id=1,
        child_id=1,
        magnitudes=values,
        sample_rate_hz=1000.0,
        fft_size=512,
        window_type="HANN",
        magnitude_unit="raw",
    )
    chart = _spectrum_chart(spectrum)
    metadata = _spectrum_metadata(spectrum)
    peaks = _spectrum_peaks(spectrum)
    assert "Magnitude [raw]" in chart
    assert "123.05 Hz" in chart
    assert "Δf: 1.9531 Hz" in metadata
    assert "17.578 Hz" in peaks
