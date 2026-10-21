"""Tela de Histórico expõe capturas e evidências no tema industrial existente."""
from __future__ import annotations

import pytest
from pico_tui.app import PicoTuiApp
from pico_tui.core.events import DtcReceived, TelemetryReceived
from pico_tui.core.models import DtcRecord, Severity, TelemetrySample
from pico_tui.history.screen import HistoryScreen
from tests.conftest import settle


@pytest.mark.parametrize('size', [(48,18),(80,24),(120,36)])
async def test_capture_timeline_and_evidence_panel(tmp_path, monkeypatch, size):
    monkeypatch.chdir(tmp_path)
    app = PicoTuiApp(demo=True, enable_file_log=False,
                     security_mode='off',preferences_path=tmp_path/'tui.json')
    async with app.run_test(size=size) as pilot:
        await settle(pilot,4)
        await app.bus.publish(TelemetryReceived(TelemetrySample(1,1,rms=1.6)))
        await app.bus.publish(DtcReceived(1,1,DtcRecord(2,severity=Severity.CRITICAL)))
        await app.bus.publish(TelemetryReceived(TelemetrySample(1,1,rms=3.2)))
        if app.history:
            await __import__('asyncio').to_thread(app.history.flush)
        if app.captures:
            await __import__('asyncio').to_thread(app.captures.flush)
        app.action_show_history()
        await settle(pilot,6)
        assert isinstance(app.screen,HistoryScreen)
        timeline = app.screen.query_one('#history-timeline')
        assert timeline is not None
        details = app.screen.query_one('#history-details')
        assert details is not None
        await pilot.press('pageup')
        await pilot.press('pagedown')
