"""Requisito 1: nenhum texto fragmentado, em nenhuma tela, de 48 a 160 colunas.

Para cada tela e tamanho de terminal verificamos que:
* nenhum contêiner rolável tem conteúdo mais largo que a área visível
  (rolagem horizontal = texto escondido);
* todo widget de texto da TUI produz linhas que cabem na largura e preserva
  todas as palavras do texto de origem (nada cortado nem perdido);
* os itens das listas de equipamentos cabem na largura da lista.
"""
from __future__ import annotations

import re

import pytest
from rich.text import Text
from textual.containers import ScrollableContainer
from textual.scroll_view import ScrollView

from pico_tui.app import PicoTuiApp
from pico_tui.widgets import StableOptionList, WrappedLines
from tests.conftest import settle

SIZES = [(48, 18), (60, 24), (80, 24), (100, 30), (160, 48)]
SCREENS = ["home", "sensor", "node", "commands", "network", "messages", "wireless", "help"]


def _words(text: str) -> list[str]:
    return [word for word in re.split(r"\s+", text) if word]


async def _open(app: PicoTuiApp, name: str) -> None:
    if name == "sensor":
        app.open_target("sensor:20.01")
    elif name == "node":
        app.open_target("node:20")
    elif name == "commands":
        app.open_commands("network")
    elif name == "network":
        app.action_show_network()
    elif name == "messages":
        app.action_show_messages()
    elif name == "wireless":
        app.action_show_wireless()
    elif name == "help":
        app.action_show_help()


@pytest.mark.parametrize("size", SIZES, ids=lambda size: f"{size[0]}x{size[1]}")
@pytest.mark.parametrize("screen_name", SCREENS)
async def test_screen_has_no_hidden_or_cut_text(tmp_path, size, screen_name) -> None:
    app = PicoTuiApp(demo=True, enable_file_log=False, security_mode="off", preferences_path=tmp_path / "tui.json")
    async with app.run_test(size=size) as pilot:
        await settle(pilot, 6)
        await _open(app, screen_name)
        await settle(pilot, 6)
        screen = app.screen
        checked = 0
        for widget in screen.walk_children(with_self=False):
            if not widget.display or widget.region.width == 0:
                continue
            scrolls = widget.styles.overflow_x in {"auto", "scroll"} or isinstance(widget, (ScrollView, ScrollableContainer))
            if scrolls and widget.max_scroll_x > 0:
                pytest.fail(f"{type(widget).__name__}#{widget.id} rola na horizontal em {size}")
            if isinstance(widget, WrappedLines):
                width = widget.content_size.width
                lines = widget._lines_for(width)
                assert all(line.cell_len <= width for line in lines), f"{widget.id} excede {width} colunas"
                rendered = " ".join(line.plain for line in lines)
                assert _words(rendered) == _words(widget.source_text()) or not widget.source_text(), widget.id
                checked += 1
            if isinstance(widget, StableOptionList) and widget.option_count:
                width = widget.scrollable_content_region.width
                for index in range(widget.option_count):
                    prompt = widget.get_option_at_index(index).prompt
                    if isinstance(prompt, Text):
                        longest = max(line.cell_len for line in prompt.split("\n"))
                        assert longest <= width, f"{widget.id}: item com {longest} colunas em lista de {width}"
        assert checked > 0
