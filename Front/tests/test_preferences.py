from __future__ import annotations

from pico_tui.preferences import Preferences, load_preferences, save_preferences


def test_round_trip(tmp_path):
    path = tmp_path / "tui.json"
    assert save_preferences(Preferences(show_messages_on_home=True, ascii_symbols=True), path) == ""
    prefs, warning = load_preferences(path)
    assert warning == ""
    assert prefs.show_messages_on_home is True and prefs.ascii_symbols is True
    assert prefs.show_debug_messages is False


def test_missing_or_invalid_file_falls_back_to_defaults(tmp_path):
    prefs, warning = load_preferences(tmp_path / "nao-existe.json")
    assert prefs == Preferences() and warning == ""
    broken = tmp_path / "tui.json"
    broken.write_text("{ isto não é json")
    prefs, warning = load_preferences(broken)
    assert prefs == Preferences() and "ignoradas" in warning
    broken.write_text('{"show_messages_on_home": "sim"}')
    prefs, _ = load_preferences(broken)
    assert prefs.show_messages_on_home is False  # só aceita booleanos
