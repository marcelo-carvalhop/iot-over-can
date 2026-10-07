from pico_tui import presentation as pres
from pico_tui.core.models import AppState


def test_empty_state_condition_is_disconnected_without_errors():
    condition = pres.system_condition(AppState())
    assert condition.level == pres.Level.NO_DATA
    assert condition.headline == "Desconectado"
    assert condition.modules_total == 0 and condition.sensors_total == 0
    assert condition.attention == []
