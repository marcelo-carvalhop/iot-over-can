"""Operador na TUI: login, bloqueio sem identificação e autoria das ações no histórico."""
from __future__ import annotations

import pytest
from textual.widgets import Button, Input

from pico_tui.app import PicoTuiApp
from pico_tui.core.events import WirelessAssociationReceived
from pico_tui.dialogs import OperatorScreen
from pico_tui.operators import ROLE_OBSERVER, OperatorRegistry, station_actor
from tests.conftest import settle

FAST = (16, 1, 1)
ANA_PASSWORD = "senha-da-ana-1"
VISITA_PASSWORD = "senha-visita-1"


class FakeClient:
    """Porta serial de mentira: guarda o que a TUI mandou para a rede."""

    is_open = True

    def __init__(self) -> None:
        self.lines: list[str] = []

    def write_line(self, line: str) -> None:
        self.lines.append(line)

    def close(self) -> None:
        self.is_open = False


class Clock:
    def __init__(self) -> None:
        self.now = 5000.0

    def __call__(self) -> float:
        return self.now


@pytest.fixture(autouse=True)
def in_tmp_dir(tmp_path, monkeypatch):
    """O histórico da TUI fica em ./logs: cada teste usa o seu."""

    monkeypatch.chdir(tmp_path)


@pytest.fixture
def operators_file(tmp_path):
    registry = OperatorRegistry(tmp_path / "operators.json", cost=FAST)
    registry.add("ana", ANA_PASSWORD, display="Ana Souza")
    registry.add("visita", VISITA_PASSWORD, role=ROLE_OBSERVER)
    return registry.path


def make_app(tmp_path, prefs_file, *, mode="auto", operators=None, **kwargs) -> PicoTuiApp:
    app = PicoTuiApp(
        enable_file_log=False, security_mode="off", preferences_path=prefs_file, operator_mode=mode,
        operators_path=operators if operators is not None else tmp_path / "sem-cadastro.json", **kwargs,
    )
    app.operators._station = station_actor("marcelo", "bancada")
    return app


def audit_events(app: PicoTuiApp) -> list:
    app.history.flush()
    return [e for e in app.history.store.list_events(app.session_id) if e.category == "AUDIT"]


def kinds(app: PicoTuiApp) -> list[str]:
    return [e.kind for e in audit_events(app)]


async def test_nothing_that_changes_the_network_leaves_before_someone_logs_in(tmp_path, prefs_file, operators_file):
    app = make_app(tmp_path, prefs_file, operators=operators_file)
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot)
        client = app.serial_client = FakeClient()

        assert app._send_raw("STATUS") is True  # consultar é livre
        assert app._send_raw("WIRELESS BIND 2 AABBCCDD") is False
        assert client.lines == ["STATUS"]

        assert app.operator_login("ana", ANA_PASSWORD).ok
        assert app._send_raw("WIRELESS BIND 2 AABBCCDD") is True
        assert client.lines == ["STATUS", "WIRELESS BIND 2 AABBCCDD"]
        await settle(pilot)

        events = {e.kind: e for e in audit_events(app)}
        assert kinds(app) == ["SESSION_START", "COMMAND_DENIED", "LOGIN", "SENSOR_BIND"]
        denied, bind = events["COMMAND_DENIED"], events["SENSOR_BIND"]
        assert denied.actor is None and denied.details["outcome"] == "DENIED" and "Identifique-se" in denied.details["reason"]
        assert bind.actor == "ana" and bind.details["actor"]["display"] == "Ana Souza"
        assert bind.details["command"] == "WIRELESS BIND 2 AABBCCDD" and bind.node_id == 2
        assert "Entrada do sensor AABBCCDD autorizada no Node 02" in bind.title
        assert app.history.store.verify_audit_chain() == (4, None)


async def test_login_dialog_opens_with_o_and_signs_the_operator_in(tmp_path, prefs_file, operators_file):
    app = make_app(tmp_path, prefs_file, operators=operators_file)
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot)
        assert "Operador sem login" in app.screen.query_one("#status-band").source_text()
        await pilot.press("o")
        await settle(pilot, 2)
        assert isinstance(app.screen, OperatorScreen)

        app.screen.query_one("#operator-name", Input).value = "ana"
        app.screen.query_one("#operator-password", Input).value = "senha-errada-1"
        app.screen.query_one("#operator-ok", Button).press()
        await settle(pilot, 2)
        assert isinstance(app.screen, OperatorScreen) and app.operators.current() is None
        assert app.screen.query_one("#operator-password", Input).value == ""  # a senha digitada não fica na tela
        assert "incorretos" in str(app.screen.query_one("#operator-error").render())

        app.screen.query_one("#operator-password", Input).value = ANA_PASSWORD
        app.screen.query_one("#operator-ok", Button).press()
        await settle(pilot, 3)
        assert not isinstance(app.screen, OperatorScreen)
        assert app.operators.current().id == "ana"
        assert "Operador Ana Souza" in app.screen.query_one("#status-band").source_text()

        failed = [e for e in audit_events(app) if e.kind == "LOGIN_FAILED"]
        assert len(failed) == 1 and failed[0].actor is None
        assert failed[0].details["attempted"] == "ana"
        assert "senha-errada-1" not in repr(failed[0].details) + failed[0].title


async def test_wrong_name_is_not_recorded_so_a_password_typed_in_the_name_field_does_not_leak(tmp_path, prefs_file, operators_file):
    app = make_app(tmp_path, prefs_file, operators=operators_file)
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot)
        assert not app.operator_login("minha-senha-secreta", "x").ok
        await settle(pilot)
        (failed,) = [e for e in audit_events(app) if e.kind == "LOGIN_FAILED"]
        assert "minha-senha-secreta" not in repr(failed.details) + failed.title
        assert all("minha-senha-secreta" not in line for line in app.log_messages)


async def test_observer_can_look_but_every_change_is_refused_and_recorded(tmp_path, prefs_file, operators_file):
    app = make_app(tmp_path, prefs_file, operators=operators_file)
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot)
        client = app.serial_client = FakeClient()
        assert app.operator_login("visita", VISITA_PASSWORD).ok
        assert app._send_raw("STATUS") is True
        assert app._send_raw("RESET") is False
        assert client.lines == ["STATUS"]
        await settle(pilot)
        denied = [e for e in audit_events(app) if e.kind == "COMMAND_DENIED"]
        assert len(denied) == 1 and denied[0].actor == "visita" and "observador" in denied[0].details["reason"]
        assert "(observador)" in app.operators.label()


async def test_session_expires_when_idle_and_the_expiry_is_recorded(tmp_path, prefs_file, operators_file):
    app = make_app(tmp_path, prefs_file, operators=operators_file, operator_idle_s=600.0)
    clock = app.operators._clock = Clock()
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot)
        client = app.serial_client = FakeClient()
        app.operator_login("ana", ANA_PASSWORD)
        clock.now += 599
        assert app._send_raw("RESET") is True  # enviar um comando também conta como atividade
        clock.now += 601
        assert app._send_raw("RESET") is False
        assert client.lines == ["RESET"]
        await settle(pilot)
        assert kinds(app) == ["SESSION_START", "LOGIN", "SENSOR_RESET", "SESSION_EXPIRED", "COMMAND_DENIED"]
        expired = next(e for e in audit_events(app) if e.kind == "SESSION_EXPIRED")
        assert expired.actor == "ana" and expired.details["reason"] == "idle"


async def test_keyboard_activity_keeps_the_session_alive(tmp_path, prefs_file, operators_file):
    app = make_app(tmp_path, prefs_file, operators=operators_file, operator_idle_s=600.0)
    clock = app.operators._clock = Clock()
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot)
        app.operator_login("ana", ANA_PASSWORD)
        clock.now += 500
        assert app.operators.seconds_left() == pytest.approx(100)
        await pilot.press("f2")
        assert app.operators.seconds_left() == pytest.approx(600)


async def test_session_ends_when_the_operator_is_disabled_in_the_registry(tmp_path, prefs_file, operators_file):
    app = make_app(tmp_path, prefs_file, operators=operators_file)
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot)
        app.serial_client = FakeClient()
        app.operator_login("ana", ANA_PASSWORD)
        OperatorRegistry(operators_file, cost=FAST).set_enabled("ana", False)  # a CLI, em outro terminal
        assert app._send_raw("RESET") is False
        await settle(pilot)
        assert "SESSION_REVOKED" in kinds(app)


async def test_logout_is_recorded_and_commands_are_blocked_again(tmp_path, prefs_file, operators_file):
    app = make_app(tmp_path, prefs_file, operators=operators_file)
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot)
        app.serial_client = FakeClient()
        app.operator_login("ana", ANA_PASSWORD)
        await app._execute_internal_command("logout")
        assert app._send_raw("RESET") is False
        await settle(pilot)
        assert kinds(app) == ["SESSION_START", "LOGIN", "LOGOUT", "COMMAND_DENIED"]


async def test_without_a_registry_the_system_user_answers_for_the_commands(tmp_path, prefs_file):
    app = make_app(tmp_path, prefs_file)
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot)
        app.serial_client = FakeClient()
        assert app._send_raw("WIRELESS UNBIND 2 AABBCCDD") is True
        await settle(pilot)
        events = audit_events(app)
        assert [e.kind for e in events] == ["SESSION_START", "SENSOR_UNBIND"]
        assert events[1].actor == "sys:marcelo" and events[1].details["actor"]["verified"] is False
        assert app.history.store.list_sessions()[0].station == "marcelo@bancada"
        assert "marcelo (sistema)" in app.screen.query_one("#status-band").source_text()


async def test_required_mode_without_operators_blocks_changes(tmp_path, prefs_file):
    app = make_app(tmp_path, prefs_file, mode="required")
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot)
        client = app.serial_client = FakeClient()
        assert app._send_raw("RESET") is False and client.lines == []
        await settle(pilot)
        denied = [e for e in audit_events(app) if e.kind == "COMMAND_DENIED"]
        assert "Nenhum operador cadastrado" in denied[0].details["reason"]


async def test_off_mode_changes_nothing_in_the_old_behaviour(tmp_path, prefs_file, operators_file):
    app = make_app(tmp_path, prefs_file, mode="off", operators=operators_file)
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot)
        client = app.serial_client = FakeClient()
        assert app._send_raw("RESET") is True and client.lines == ["RESET"]
        await settle(pilot)
        assert audit_events(app) == [] and app.operators.kind == "off"
        assert "Operador" not in app.screen.query_one("#status-band").source_text()


async def test_the_answer_to_a_bind_is_credited_to_who_authorized_it(tmp_path, prefs_file, operators_file):
    app = make_app(tmp_path, prefs_file, operators=operators_file)
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot)
        app.serial_client = FakeClient()
        app.operator_login("ana", ANA_PASSWORD)
        app._send_raw("WIRELESS BIND 2 AABBCCDD")
        await app.bus.publish(WirelessAssociationReceived(2, 1, "AABBCCDD", "VIBRATION", "BOUND", -44))
        await app.bus.publish(WirelessAssociationReceived(2, 1, "AABBCCDD", "VIBRATION", "LOST", -90))
        await settle(pilot)
        app.history.flush()
        association = [e for e in app.history.store.list_events(app.session_id) if e.kind == "ASSOCIATION"]
        assert [(e.details["state"], e.actor) for e in association] == [("BOUND", "ana"), ("LOST", None)]


async def test_passwords_and_tokens_never_reach_the_history_or_the_messages(tmp_path, prefs_file, operators_file):
    app = make_app(tmp_path, prefs_file, operators=operators_file)
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot)
        client = app.serial_client = FakeClient()
        app.operator_login("ana", ANA_PASSWORD)
        assert app._send_raw("NET WIFI PROVISION laboratorio minha-senha-wifi")
        assert client.lines == ["NET WIFI PROVISION laboratorio minha-senha-wifi"]  # o firmware recebe o comando inteiro
        await settle(pilot)
        history = repr([(e.title, e.details) for e in audit_events(app)])
        assert "minha-senha-wifi" not in history and ANA_PASSWORD not in history
        assert not any("minha-senha-wifi" in line or ANA_PASSWORD in line for line in app.log_messages)
        provision = [e for e in audit_events(app) if e.kind == "WIFI_PROVISION"]
        assert provision and provision[0].details["command"] == "NET WIFI PROVISION laboratorio ***"


async def test_alarm_limits_need_an_identified_operator_and_are_recorded(tmp_path, prefs_file, operators_file):
    app = make_app(tmp_path, prefs_file, operators=operators_file)
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot)
        app.set_alarm_limits("02.01", {"rms": 0.5})
        assert app.state_store.alarm_limits() == {}  # sem login, nada muda
        app.operator_login("ana", ANA_PASSWORD)
        app.set_alarm_limits("02.01", {"rms": 0.5})
        assert app.state_store.alarm_limits() == {"02.01": {"rms": 0.5}}
        app.set_alarm_limits("02.01", {"rms": 0.8})
        await settle(pilot)
        events = [e for e in audit_events(app) if e.kind.startswith("ALARM_LIMITS")]
        assert [e.kind for e in events] == ["ALARM_LIMITS_DENIED", "ALARM_LIMITS", "ALARM_LIMITS"]
        assert events[2].actor == "ana" and events[2].details["before"] == {"rms": 0.5} and events[2].details["after"] == {"rms": 0.8}
        assert events[2].sensor_id == "02.01"


async def test_operator_command_describes_who_is_identified(tmp_path, prefs_file, operators_file):
    app = make_app(tmp_path, prefs_file, operators=operators_file)
    async with app.run_test(size=(140, 45)) as pilot:
        await settle(pilot)
        await app._execute_internal_command("operador")
        assert any("Ninguém identificado" in line for line in app.log_messages)
        app.operator_login("ana", ANA_PASSWORD)
        await app._execute_internal_command("operador")
        assert any("Operador: Ana Souza" in line for line in app.log_messages)


async def test_narrow_screens_still_warn_that_nobody_is_logged_in(tmp_path, prefs_file, operators_file):
    app = make_app(tmp_path, prefs_file, operators=operators_file)
    async with app.run_test(size=(60, 30)) as pilot:
        await settle(pilot)
        assert "sem login" in app.screen.query_one("#status-band").source_text()
