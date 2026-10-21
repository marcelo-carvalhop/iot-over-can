"""Integração da tela histórica na TUI e nas dimensões compactas."""
from __future__ import annotations

import pytest

from pico_tui.app import PicoTuiApp
from pico_tui.history.screen import HistoryScreen
from tests.conftest import settle


@pytest.mark.parametrize("size", [(48, 18), (80, 24), (120, 36)])
async def test_history_screen_opens_and_navigates(tmp_path, monkeypatch, size):
    monkeypatch.chdir(tmp_path)
    app = PicoTuiApp(demo=True, enable_file_log=False,
                     security_mode="off", preferences_path=tmp_path / "tui.json")
    async with app.run_test(size=size) as pilot:
        await settle(pilot, 4)
        app.action_show_history()
        await settle(pilot, 4)
        assert isinstance(app.screen, HistoryScreen)
        assert app.screen.query_one("#history-timeline") is not None
        assert app.screen.query_one("#history-details") is not None
        await pilot.press("pageup")
        await pilot.press("pagedown")
        assert isinstance(app.screen, HistoryScreen)
