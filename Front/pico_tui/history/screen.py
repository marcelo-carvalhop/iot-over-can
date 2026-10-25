"""Explorador de sessões: linha do tempo superior e inspeção contextual inferior."""
from __future__ import annotations

import asyncio
import json
import textwrap
import time
from datetime import datetime

from rich.text import Text
from textual import on, work
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.widgets import OptionList, Static

from pico_tui import palette
from pico_tui.core.models import AppState
from pico_tui.history.storage import HistoryEvent, Session
from pico_tui.screens import BaseScreen, section
from pico_tui.widgets import StableOptionList

LANES = ("NETWORK", "CAN", "WIRELESS", "SENSOR", "OPERATOR", "SYSTEM", "CAPTURA")
COLORS = {"INFO": palette.STATE_INFO, "WARNING": palette.STATE_WARNING,
          "CRITICAL": palette.STATE_CRITICAL}
ZOOMS = (60, 300, 900, 1800, 7200, 21600, 86400, 604800)


def hhmm(epoch: float) -> str:
    return datetime.fromtimestamp(epoch).strftime("%d/%m %H:%M:%S")


class HistoryScreen(BaseScreen):
    """Mostra eventos registrados; não sugere telemetria que não foi capturada."""

    BINDINGS = BaseScreen.BINDINGS + [
        Binding("left", "previous_event", "Evento anterior", show=False),
        Binding("right", "next_event", "Próximo evento", show=False),
        Binding("pageup", "pan_left", "Anterior", show=False),
        Binding("pagedown", "pan_right", "Posterior", show=False),
        Binding("plus", "zoom_in", "Ampliar", show=False),
        Binding("minus", "zoom_out", "Reduzir", show=False),
        Binding("u", "reload", "Atualizar sessões", show=False),
        Binding("comma", "previous_evidence", "Arquivos anteriores", show=False),
        Binding("period", "next_evidence", "Mais arquivos", show=False),
    ]

    def __init__(self) -> None:
        super().__init__()
        self._sessions: list[Session] = []
        self._session: Session | None = None
        self._events: list[HistoryEvent] = []
        self._selected: int | None = None
        self._start = 0.0
        self._end = 0.0
        self._zoom = 3
        self._last_width = 0
        self._file_page = 0

    def breadcrumb(self):
        return ("Início", "Histórico"), "Sessões e eventos persistidos"

    def key_hints(self):
        return [("Esc", "Voltar", "screen.back"),
                ("←/→", "Evento", None), ("PgUp/PgDn", "Intervalo", None),
                ("+/-", "Zoom", None), (",/.", "Arquivos", None),
                ("u", "Atualizar", "screen.reload")]

    def compose_body(self) -> ComposeResult:
        with VerticalScroll(id="history-body", classes="body"):
            with section("Sessões registradas", id="history-session-section"):
                yield StableOptionList(id="history-sessions", empty="Nenhuma sessão registrada.")
            with section("Linha do tempo", id="history-timeline-section"):
                yield Static(id="history-timeline")
            with section("Inspeção histórica", id="history-inspection-section"):
                with Horizontal(id="history-inspection-columns", classes="columns"):
                    with Vertical(classes="column column-main"):
                        yield StableOptionList(id="history-events", empty="Nenhum evento neste intervalo.")
                    with Vertical(classes="column column-side"):
                        yield Static(id="history-details")

    def on_mount(self) -> None:
        super().on_mount()
        self._load_sessions()

    def update_body(self, state: AppState) -> None:
        if self.size.width != self._last_width:
            self._last_width = self.size.width
            if self._session is not None:
                self._load_events()
            else:
                self._draw_timeline()

    def _load_sessions(self) -> None:
        try:
            store = self.tui.history.store if self.tui.history else None
            self._sessions = store.list_sessions() if store else []
        except Exception as exc:
            self.query_one("#history-details", Static).update(f"Falha ao consultar histórico: {exc}")
            return
        options = []
        for s in self._sessions:
            label = f"{hhmm(s.started_epoch)}  {s.origin}  "
            label += "EM CURSO" if s.ended_epoch is None else "ENCERRADA"
            options.append((s.id, Text(label, style=palette.TEXT_PRIMARY)))
        self.query_one("#history-sessions", StableOptionList).set_items(options)
        if self._sessions:
            wanted = next((s for s in self._sessions if self._session and s.id == self._session.id), self._sessions[0])
            self._select_session(wanted.id)
        else:
            self.query_one("#history-timeline", Static).update("Sem sessões para exibir.")
            self.query_one("#history-details", Static).update("O histórico começará a ser registrado ao iniciar a TUI.")

    def _select_session(self, sid: str) -> None:
        self._session = next((s for s in self._sessions if s.id == sid), None)
        self._selected = None
        self._file_page = 0
        if self._session is None:
            return
        until = self._session.ended_epoch or time.time()
        self._end = max(self._session.started_epoch + 1, until)
        self._start = max(self._session.started_epoch, self._end - ZOOMS[self._zoom])
        self._load_events()

    def _load_events(self) -> None:
        if self._session is None or self.tui.history is None:
            return
        try:
            store = self.tui.history.store
            self._events = store.list_events(self._session.id, start=self._start,
                                             end=self._end, limit=500)
            total = store.count_events(self._session.id, self._start, self._end)
        except Exception as exc:
            self.query_one("#history-details", Static).update(f"Falha na consulta: {exc}")
            return
        options = []
        for event in self._events:
            line = Text(hhmm(event.epoch) + "  ", style=palette.TEXT_DIM)
            line.append(f"[{event.category}] ", style=COLORS.get(event.severity, palette.TEXT_SECONDARY))
            width = max(15, int(self.size.width * (0.55 if self.size.width >= 100 else 1.0)) - 12)
            words = textwrap.wrap(event.title, width=width, break_long_words=True, break_on_hyphens=False)
            line.append("\n  " + "\n  ".join(words or [""]), style=palette.TEXT_PRIMARY)
            options.append((str(event.id), line))
        self.query_one("#history-events", StableOptionList).set_items(options)
        if self._selected is None or not any(e.id == self._selected for e in self._events):
            self._selected = self._events[0].id if self._events else None
        self._draw_timeline(total)
        self._show_details()

    def _draw_timeline(self, total: int | None = None) -> None:
        widget = self.query_one("#history-timeline", Static)
        if self._session is None:
            widget.update("Escolha uma sessão.")
            return
        cols = max(20, min(120, self.size.width - 26))
        delta = max(self._end - self._start, 1)
        graph = Text()
        graph.append(f"Janela: {hhmm(self._start)} → {hhmm(self._end)}", style=palette.TEXT_SECONDARY)
        if total is not None and total > len(self._events):
            graph.append(f"  (exibindo {len(self._events)} de {total} eventos)", style=palette.STATE_WARNING)
        graph.append("\n")
        try:
            captures = self.tui.history.store.capture_intervals(self._session.id, self._start, self._end)
        except Exception:
            captures = []
        for category in LANES:
            cells = ["·"] * cols
            styles = [palette.TEXT_DIM] * cols
            if category == "CAPTURA":
                for cap in captures:
                    a = max(0, min(cols - 1, int((cap['start'] - self._start) / delta * (cols - 1))))
                    b = max(a, min(cols - 1, int((cap['end'] - self._start) / delta * (cols - 1))))
                    for pos in range(a, b + 1):
                        cells[pos] = "━"
                        styles[pos] = palette.ACCENT_ACTION
            for ev in self._events:
                if ev.category != category:
                    continue
                ix = max(0, min(cols-1, int((ev.epoch - self._start) / delta * (cols-1))))
                selected = ev.id == self._selected
                cells[ix] = "◆" if selected else "●"
                styles[ix] = palette.ACCENT_FOCUS if selected else COLORS.get(ev.severity, palette.STATE_INFO)
            graph.append(f"{category:<9} ", style=palette.TEXT_MUTED)
            for char, style in zip(cells, styles):
                graph.append(char, style=style)
            graph.append("\n")
        graph.append(f"{'':10}{datetime.fromtimestamp(self._start).strftime('%H:%M:%S')}", style=palette.TEXT_DIM)
        graph.append(" " * max(1, cols - 16))
        graph.append(datetime.fromtimestamp(self._end).strftime("%H:%M:%S"), style=palette.TEXT_DIM)
        widget.update(graph)

    def _show_details(self) -> None:
        widget = self.query_one("#history-details", Static)
        event = next((x for x in self._events if x.id == self._selected), None)
        if event is None:
            widget.update("Não há evento selecionado. Navegue para outro intervalo com PgUp/PgDn.")
            return
        detail = Text()
        detail.append(event.title + "\n", style=f"bold {COLORS.get(event.severity, palette.TEXT_PRIMARY)}")
        detail.append(f"Ocorrência: {hhmm(event.epoch)}\n", style=palette.TEXT_SECONDARY)
        detail.append(f"Tipo: {event.kind}  Severidade: {event.severity}\n", style=palette.TEXT_SECONDARY)
        if event.node_id is not None:
            detail.append(f"Node: {event.node_id:02d}\n")
        if event.sensor_id:
            detail.append(f"Sensor lógico: {event.sensor_id}\n")
        snapshot = event.details.get("bus_at_observation")
        if snapshot:
            detail.append("\nRede na observação\n", style=f"bold {palette.TEXT_PRIMARY}")
            detail.append(f"Ocupação: {snapshot['load_percent']:.2f}%  "
                          f"pico: {snapshot['peak_percent']:.2f}%\n", style=palette.TEXT_SECONDARY)
            detail.append(f"Erros CAN RX/TX: {snapshot['rx_errors']}/{snapshot['tx_errors']}\n",
                          style=palette.TEXT_SECONDARY)
        if self._session is not None and self.tui.history is not None:
            aggregate = self.tui.history.store.nearest_bus(self._session.id, event.epoch)
            if aggregate:
                detail.append("\nResumo CAN armazenado (janela amostrada)\n", style=f"bold {palette.TEXT_PRIMARY}")
                detail.append(f"Carga média: {aggregate['load_avg_pct']:.2f}%  "
                              f"pico: {aggregate['peak_max_pct']:.2f}%\n", style=palette.TEXT_SECONDARY)
                detail.append(f"Quadros: {aggregate['frames']}  "
                              f"observações: {aggregate['samples']}\n", style=palette.TEXT_SECONDARY)
        link = event.details.get("link_at_observation")
        if link:
            detail.append("\nEnlace: " + str(link) + "\n", style=palette.TEXT_SECONDARY)
        extra = {k: v for k, v in event.details.items() if k not in ("bus_at_observation", "link_at_observation")}
        if extra:
            detail.append("\nMetadados\n", style=f"bold {palette.TEXT_PRIMARY}")
            detail.append(json.dumps(extra, ensure_ascii=False, indent=2, default=str), style=palette.TEXT_MUTED)
        if self.tui.history:
            store = self.tui.history.store
            try:
                caps = store.capture_for_event(event.id)
                files = store.files_for_event(event.id, limit=1000)
            except Exception as exc:
                detail.append(f"\nEvidências: consulta indisponível ({exc})", style=palette.STATE_WARNING)
            else:
                detail.append("\nEvidências de captura\n", style=f"bold {palette.TEXT_PRIMARY}")
                if not caps:
                    detail.append("Nenhuma captura contextual vinculada a esta ocorrência.\n",
                                  style=palette.TEXT_MUTED)
                for cap in caps:
                    detail.append(f"{cap['id'][:12]} · {cap['scope']} · {cap['state']}",
                                  style=palette.TEXT_SECONDARY)
                    if cap['dropped_records']:
                        detail.append(f" · {cap['dropped_records']} registros perdidos", style=palette.STATE_WARNING)
                    detail.append("\n")
                if caps and not files:
                    detail.append("Arquivos ainda não gravados ou nenhuma amostra no intervalo.\n",
                                  style=palette.TEXT_MUTED)
                if files:
                    begin = self._file_page * 6
                    if begin >= len(files):
                        self._file_page = 0
                        begin = 0
                    detail.append(f"Arquivos {begin + 1}–{min(begin + 6, len(files))} de {len(files)}"
                                  " (vírgula/ponto para navegar)\n", style=palette.TEXT_MUTED)
                for f in files[self._file_page * 6:self._file_page * 6 + 6]:
                    status, rows = store.preview_csv(f, max_rows=2)
                    detail.append(f"{f.kind} {f.sensor_id or 'rede'}: {f.rows} linhas · {status}\n",
                                  style=palette.STATE_INFO if status == 'OK' else palette.STATE_WARNING)
                    detail.append(f"  {f.relative_path}\n", style=palette.TEXT_MUTED)
                    for row in rows:
                        if f.kind == 'TELEMETRY':
                            vals = [f"{name}={row[name]}" for name in ('rms','laeq_db','peak_frequency_hz') if row.get(name)]
                        elif f.kind == 'BUS':
                            vals = [f"carga={row.get('load_percent', '?')}%", f"pico={row.get('peak_percent', '?')}%"]
                        elif f.kind == 'LINK':
                            vals = [f"estado={row.get('state', '?')}", f"RTT={row.get('rtt_ms', '?')} ms"]
                        else:
                            vals = [f"estado={row.get('state','?')}"]
                        detail.append("  " + " · ".join(vals) + "\n", style=palette.TEXT_SECONDARY)
        widget.update(detail)

    @on(OptionList.OptionHighlighted, "#history-sessions")
    def _session_highlighted(self, event: OptionList.OptionHighlighted) -> None:
        if event.option.id is not None and str(event.option.id) != "__empty__":
            self._select_session(str(event.option.id))

    @on(OptionList.OptionHighlighted, "#history-events")
    def _event_highlighted(self, event: OptionList.OptionHighlighted) -> None:
        if event.option.id is None:
            return
        try:
            self._selected = int(str(event.option.id))
        except ValueError:
            return
        self._file_page = 0
        self._draw_timeline()
        self._show_details()

    def action_next_evidence(self) -> None:
        event = next((e for e in self._events if e.id == self._selected), None)
        if event is None or self.tui.history is None:
            return
        total = len(self.tui.history.store.files_for_event(event.id, limit=1000))
        if (self._file_page + 1) * 6 < total:
            self._file_page += 1
            self._show_details()

    def action_previous_evidence(self) -> None:
        if self._file_page > 0:
            self._file_page -= 1
            self._show_details()

    def action_previous_event(self) -> None:
        ids = [e.id for e in self._events]
        if not ids:
            return
        pos = ids.index(self._selected) if self._selected in ids else 0
        self._selected = ids[max(0, pos - 1)]
        self._file_page = 0
        self.query_one("#history-events", StableOptionList).highlighted = max(0, pos - 1)
        self._draw_timeline()
        self._show_details()

    def action_next_event(self) -> None:
        ids = [e.id for e in self._events]
        if not ids:
            return
        pos = ids.index(self._selected) if self._selected in ids else -1
        self._selected = ids[min(len(ids) - 1, pos + 1)]
        self._file_page = 0
        self.query_one("#history-events", StableOptionList).highlighted = min(len(ids) - 1, pos + 1)
        self._draw_timeline()
        self._show_details()

    def _pan(self, direction: int) -> None:
        if self._session is None:
            return
        duration = ZOOMS[self._zoom]
        max_end = self._session.ended_epoch or time.time()
        new_start = max(self._session.started_epoch, self._start + direction * duration * 0.75)
        new_end = min(max_end, new_start + duration)
        self._start = max(self._session.started_epoch, new_end - duration)
        self._end = new_end
        self._selected = None
        self._load_events()

    def action_pan_left(self) -> None:
        self._pan(-1)

    def action_pan_right(self) -> None:
        self._pan(1)

    def _change_zoom(self, step: int) -> None:
        if self._session is None:
            return
        self._zoom = max(0, min(len(ZOOMS)-1, self._zoom + step))
        center = (self._start + self._end) / 2
        duration = ZOOMS[self._zoom]
        self._start = max(self._session.started_epoch, center - duration / 2)
        self._end = min(self._session.ended_epoch or time.time(), self._start + duration)
        self._selected = None
        self._load_events()

    def action_zoom_in(self) -> None:
        self._change_zoom(-1)

    def action_zoom_out(self) -> None:
        self._change_zoom(1)

    @work
    async def action_reload(self) -> None:
        if self.tui.history:
            await asyncio.to_thread(self.tui.history.flush)
        self._load_sessions()
