"""A tela do histórico mostra quem fez cada ação."""
from __future__ import annotations

from pico_tui.app import PicoTuiApp
from pico_tui.history.screen import LANES, HistoryScreen
from pico_tui.operators import station_actor
from tests.conftest import settle


async def test_history_screen_shows_the_author_of_an_action(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    app = PicoTuiApp(demo=True, enable_file_log=False, security_mode="off", preferences_path=tmp_path / "tui.json",
                     operator_mode="auto", operators_path=tmp_path / "sem-cadastro.json")
    app.operators._station = station_actor("marcelo", "bancada")
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot, 4)
        app._audit_command("WIRELESS BIND 2 AABBCCDD", app.operators.current())
        await settle(pilot, 3)
        app.history.flush()
        app.action_show_history()
        await settle(pilot, 4)
        screen = app.screen
        assert isinstance(screen, HistoryScreen) and "AUDIT" in LANES
        bind = next(e for e in screen._events if e.kind == "SENSOR_BIND")
        screen._selected = bind.id
        screen._show_details()
        details = str(screen.query_one("#history-details").render())
        assert "Responsável: marcelo, operador, usuário do sistema, não verificado" in details
        assert "Entrada do sensor AABBCCDD autorizada no Node 02" in details
        assert '"actor"' not in details  # o autor já aparece na linha própria, sem repetir o JSON
        timeline = str(screen.query_one("#history-timeline").render())
        assert "AUDIT" in timeline
        assert app.history.store.list_sessions()[0].station == "marcelo@bancada"
