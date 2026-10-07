"""Preferências de interface do operador (não sensíveis).

Arquivo: ``~/.config/iot-over-can/tui.json`` (separado de ``security.json``).
Falhas de leitura ou gravação nunca impedem a TUI de abrir: a aplicação
volta aos valores padrão e informa o problema no registro de mensagens.
"""
from __future__ import annotations

import json
from dataclasses import asdict, dataclass, fields
from pathlib import Path

DEFAULT_PATH = Path.home() / ".config" / "iot-over-can" / "tui.json"


@dataclass(slots=True)
class Preferences:
    # Mostra o painel "Mensagens recentes" na tela inicial.
    show_messages_on_home: bool = False
    # Inclui mensagens de depuração (DEBUG) na tela de mensagens.
    show_debug_messages: bool = False
    # Usa símbolos ASCII em vez de ● ▲ ■ ○ (consoles sem Unicode).
    ascii_symbols: bool = False


def load_preferences(path: str | Path | None = None) -> tuple[Preferences, str]:
    """Lê as preferências. Retorna (preferências, aviso); aviso vazio se tudo certo."""

    target = Path(path) if path else DEFAULT_PATH
    prefs = Preferences()
    if not target.exists():
        return prefs, ""
    try:
        data = json.loads(target.read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            raise ValueError("o arquivo deve conter um objeto JSON")
    except (OSError, ValueError) as exc:
        return prefs, f"Preferências ignoradas ({target}): {exc}"
    for item in fields(Preferences):
        value = data.get(item.name)
        if isinstance(value, bool):
            setattr(prefs, item.name, value)
    return prefs, ""


def save_preferences(prefs: Preferences, path: str | Path | None = None) -> str:
    """Grava as preferências. Retorna mensagem de erro, ou vazio em caso de sucesso."""

    target = Path(path) if path else DEFAULT_PATH
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        tmp = target.with_suffix(".tmp")
        tmp.write_text(json.dumps(asdict(prefs), indent=2, ensure_ascii=False), encoding="utf-8")
        tmp.replace(target)
    except OSError as exc:
        return f"Não foi possível salvar preferências em {target}: {exc}"
    return ""
