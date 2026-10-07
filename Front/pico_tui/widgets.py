"""Widgets da TUI.

Regra central: nenhum texto é cortado. Os widgets de texto calculam as próprias
linhas para a largura disponível (``build_lines``), sempre com no máximo
``width`` células por linha. Assim a altura informada ao Textual é exata nas
versões 1.x e 8.x e nada depende de rolagem horizontal.
"""
from __future__ import annotations

from collections.abc import Iterable, Sequence

from rich.console import Console
from rich.text import Text
from textual.events import Click
from textual.widget import Widget
from textual.widgets import OptionList, RichLog
from textual.widgets.option_list import Option

from pico_tui import palette
from pico_tui import presentation as pres
from pico_tui.core.models import AppState, ConnectionMode, Incident, PhysicalNode, SensorNode, Severity
from pico_tui.dtc_catalog import dtc_description

_WRAP_CONSOLE = Console(width=400, color_system=None, legacy_windows=False)

LEVEL_COLORS = {
    pres.Level.NO_DATA: palette.STATE_OFFLINE,
    pres.Level.NORMAL: palette.STATE_OK,
    pres.Level.ATTENTION: palette.STATE_WARNING,
    pres.Level.CRITICAL: palette.STATE_CRITICAL,
}


def level_text(level: pres.Level, label: str | None = None, *, bold: bool = False) -> Text:
    """Símbolo + palavra, colorido. Nunca apenas cor."""

    style = LEVEL_COLORS[level] + (" bold" if bold else "")
    text = Text()
    text.append(f"{pres.symbol(level)} {label or pres.level_label(level)}", style=style)
    return text


def wrap_text(text: Text | str, width: int) -> list[Text]:
    """Quebra por palavras; palavras maiores que a largura são divididas."""

    if isinstance(text, str):
        text = Text(text)
    width = max(1, width)
    if not text.plain:
        return [Text("")]
    lines = text.wrap(_WRAP_CONSOLE, width, overflow="fold")
    return [line.copy() for line in lines] or [Text("")]


def _text_key(text: Text) -> tuple:
    return (text.plain, str(text.style), tuple((span.start, span.end, str(span.style)) for span in text.spans))


# ---------------------------------------------------------------------------
# Base: texto com linhas calculadas para a largura
# ---------------------------------------------------------------------------


class WrappedLines(Widget):
    """Widget cujas linhas são montadas pela própria classe para cada largura."""

    DEFAULT_CSS = """
    WrappedLines {
        width: 1fr;
        height: auto;
    }
    """

    def __init__(self, *, id: str | None = None, classes: str | None = None) -> None:
        super().__init__(id=id, classes=classes)
        self._version = 0
        self._cache: tuple[int, int, list[Text]] | None = None

    def build_lines(self, width: int) -> list[Text]:  # pragma: no cover - abstrato
        raise NotImplementedError

    def _lines_for(self, width: int) -> list[Text]:
        width = max(1, width)
        if self._cache and self._cache[0] == width and self._cache[1] == self._version:
            return self._cache[2]
        lines = self.build_lines(width) or [Text("")]
        self._cache = (width, self._version, lines)
        return lines

    def changed(self) -> None:
        self._version += 1
        self.refresh(layout=True)

    def source_text(self) -> str:
        """Texto de origem (antes da quebra), usado nos testes de não fragmentação."""

        return ""

    def get_content_height(self, container, viewport, width: int) -> int:
        return len(self._lines_for(width))

    def render(self) -> Text:
        width = self.content_size.width or self.size.width or 80
        return Text("\n").join(self._lines_for(width))


class FlowLine(WrappedLines):
    """Segmentos indivisíveis distribuídos em linhas (ex.: barra de teclas).

    Cada segmento pode ter uma ação associada; um clique sobre ele a executa.
    """

    def __init__(self, *, gap: int = 3, id: str | None = None, classes: str | None = None) -> None:
        super().__init__(id=id, classes=classes)
        self._gap = gap
        self._segments: list[Text] = []
        self._actions: list[str | None] = []
        self._key: tuple = ()
        self._hit_map: list[list[tuple[int, int, str | None]]] = []

    @property
    def segments(self) -> list[Text]:
        return list(self._segments)

    def set_segments(self, segments: Sequence[Text], actions: Sequence[str | None] | None = None) -> None:
        actions = list(actions) if actions is not None else [None] * len(segments)
        key = tuple(_text_key(segment) for segment in segments) + tuple(actions)
        if key == self._key:
            return
        self._key = key
        self._segments = [segment.copy() for segment in segments]
        self._actions = actions
        self.changed()

    def source_text(self) -> str:
        return " ".join(segment.plain for segment in self._segments)

    def build_lines(self, width: int) -> list[Text]:
        packed = pres.pack_segments([segment.cell_len for segment in self._segments], width, self._gap)
        lines: list[Text] = []
        hit_map: list[list[tuple[int, int, str | None]]] = []
        for indices in packed:
            if len(indices) == 1 and self._segments[indices[0]].cell_len > width:
                wrapped = wrap_text(self._segments[indices[0]], width)
                lines.extend(wrapped)
                hit_map.extend([[(0, width, self._actions[indices[0]])] for _ in wrapped])
                continue
            line = Text()
            spans: list[tuple[int, int, str | None]] = []
            for position, index in enumerate(indices):
                if position:
                    line.append(" " * self._gap)
                start = line.cell_len
                line.append_text(self._segments[index])
                spans.append((start, line.cell_len, self._actions[index]))
            lines.append(line)
            hit_map.append(spans)
        self._hit_map = hit_map
        return lines

    async def on_click(self, event: Click) -> None:
        self._lines_for(self.content_size.width)
        if 0 <= event.y < len(self._hit_map):
            for start, end, action in self._hit_map[event.y]:
                if action and start <= event.x < end:
                    event.stop()
                    await self.app.run_action(action, self.screen)
                    return


class KeyBar(FlowLine):
    """Barra de teclas que quebra em várias linhas em vez de cortar."""

    def __init__(self, hints: Sequence[tuple[str, str, str | None]] = (), **kwargs) -> None:
        super().__init__(gap=2, **kwargs)
        if hints:
            self.set_hints(hints)

    def set_hints(self, hints: Sequence[tuple[str, str, str | None]]) -> None:
        segments = []
        for key, label, _action in hints:
            segment = Text()
            segment.append(key, style=f"bold {palette.ACCENT_FOCUS}")
            segment.append(f" {label}", style=palette.TEXT_SECONDARY)
            segments.append(segment)
        self.set_segments(segments, [action for _key, _label, action in hints])


class Breadcrumb(FlowLine):
    """Caminho de navegação: Início / Módulo 20 / Sensor 20.01."""

    def __init__(self, parts: Sequence[str] = (), **kwargs) -> None:
        super().__init__(gap=1, **kwargs)
        if parts:
            self.set_parts(parts)

    def set_parts(self, parts: Sequence[str], subtitle: str = "") -> None:
        segments: list[Text] = []
        for index, part in enumerate(parts):
            last = index == len(parts) - 1
            segment = Text(part, style=f"bold {palette.TEXT_PRIMARY}" if last else palette.TEXT_MUTED)
            if not last:
                segment.append(" /", style=palette.TEXT_DIM)
            segments.append(segment)
        if subtitle:
            segments.append(Text(f"  {subtitle}", style=palette.TEXT_SECONDARY))
        self.set_segments(segments)


class StatusBand(FlowLine):
    """Faixa de condição do sistema: neutra quando normal, âmbar/vermelha quando não."""

    LEVEL_CLASSES = {
        pres.Level.NO_DATA: "band-offline",
        pres.Level.NORMAL: "band-normal",
        pres.Level.ATTENTION: "band-attention",
        pres.Level.CRITICAL: "band-critical",
    }

    def __init__(self, **kwargs) -> None:
        super().__init__(gap=3, **kwargs)
        self._level: pres.Level | None = None

    def show(self, state: AppState, *, security_label: str, port: str, compact: bool = False) -> pres.SystemCondition:
        """Atualiza a faixa.

        ``compact`` (telas estreitas ou baixas) mantém apenas a condição e, se
        houver, o motivo de bloqueio; o restante continua na tela inicial,
        seção Conexão. Assim a faixa ocupa uma ou duas linhas num celular.
        """

        condition = pres.system_condition(state)
        if condition.level != self._level:
            for css_class in self.LEVEL_CLASSES.values():
                self.remove_class(css_class)
            self.add_class(self.LEVEL_CLASSES[condition.level])
            self._level = condition.level
        headline = Text(f"{pres.symbol(condition.level)} ", style="bold")
        headline.append(condition.headline, style="bold")
        segments = [headline]
        if compact:
            if not condition.connected:
                segments.append(Text("F3 ou p conecta"))
            elif security_label not in {"desligada", "com YubiKey"}:
                segments.append(Text(f"Segurança {security_label}"))
            self.set_segments(segments)
            return condition
        if condition.connected:
            where = pres.mode_label(state.connection_mode)
            if port and port != "DEMO":
                where += f" em {port}"
            segments.append(Text(where))
            if state.connection_mode != ConnectionMode.SENSOR_DIRECT and state.gateway.arbitration_bitrate:
                segments.append(Text(f"CAN {state.gateway.arbitration_bitrate // 1000} kbit/s"))
            counts = []
            if condition.modules_total:
                counts.append(f"{condition.modules_ok} de {condition.modules_total} módulos")
            if condition.sensors_total:
                counts.append(f"{condition.sensors_ok} de {condition.sensors_total} sensores")
            if counts:
                segments.append(Text(" e ".join(counts) + " normais"))
        else:
            segments.append(Text("Pressione F3 ou p para conectar"))
        segments.append(Text(f"Segurança {security_label}"))
        self.set_segments(segments)
        return condition


# ---------------------------------------------------------------------------
# Campos rótulo/valor
# ---------------------------------------------------------------------------


Field = tuple[str, "Text | str"]


class Fields(WrappedLines):
    """Lista rótulo → valor.

    Em largura suficiente, rótulo e valor ficam lado a lado; o valor quebra
    dentro da própria coluna. Em telas estreitas, o valor passa para a linha
    de baixo, recuado. Nada é truncado.
    """

    MAX_LABEL = 24
    MIN_VALUE = 16

    def __init__(self, fields: Sequence[Field] = (), **kwargs) -> None:
        super().__init__(**kwargs)
        self._fields: list[tuple[str, Text]] = []
        self._key: tuple = ()
        if fields:
            self.set_fields(fields)

    def set_fields(self, fields: Iterable[Field]) -> None:
        normalized: list[tuple[str, Text]] = []
        for label, value in fields:
            value_text = value.copy() if isinstance(value, Text) else Text(str(value), style=palette.TEXT_PRIMARY)
            normalized.append((label, value_text))
        key = tuple((label, _text_key(value)) for label, value in normalized)
        if key == self._key:
            return
        self._key = key
        self._fields = normalized
        self.changed()

    def source_text(self) -> str:
        return " ".join(f"{label} {value.plain}" for label, value in self._fields)

    def build_lines(self, width: int) -> list[Text]:
        if not self._fields:
            return [Text("")]
        label_width = min(self.MAX_LABEL, max(len(label) for label, _ in self._fields))
        side_by_side = width >= label_width + 2 + self.MIN_VALUE
        lines: list[Text] = []
        for label, value in self._fields:
            if side_by_side:
                value_width = width - label_width - 2
                label_lines = wrap_text(Text(label, style=palette.TEXT_MUTED), label_width)
                value_lines = wrap_text(value, value_width)
                for row in range(max(len(label_lines), len(value_lines))):
                    line = Text()
                    label_part = label_lines[row] if row < len(label_lines) else Text("")
                    line.append_text(label_part)
                    line.append(" " * (label_width - label_part.cell_len + 2))
                    if row < len(value_lines):
                        line.append_text(value_lines[row])
                    lines.append(line)
            else:
                lines.extend(wrap_text(Text(label, style=palette.TEXT_MUTED), width))
                for value_line in wrap_text(value, max(1, width - 2)):
                    line = Text("  ")
                    line.append_text(value_line)
                    lines.append(line)
        return lines


class Paragraphs(WrappedLines):
    """Parágrafos com recuo de continuação (listas de intercorrências, quadros CAN)."""

    def __init__(self, empty: str = "", **kwargs) -> None:
        super().__init__(**kwargs)
        self._items: list[Text] = []
        self._empty = empty
        self._key: tuple = ()
        self._indent = 2

    def set_items(self, items: Sequence[Text], *, indent: int = 2) -> None:
        key = (indent,) + tuple(_text_key(item) for item in items)
        if key == self._key:
            return
        self._key = key
        self._items = [item.copy() for item in items]
        self._indent = indent
        self.changed()

    def source_text(self) -> str:
        return " ".join(item.plain for item in self._items) if self._items else self._empty

    def build_lines(self, width: int) -> list[Text]:
        if not self._items:
            return wrap_text(Text(self._empty, style=palette.TEXT_MUTED), width)
        lines: list[Text] = []
        indent = min(self._indent, max(0, width // 4))
        for item in self._items:
            first, *rest = wrap_text(item, width)
            lines.append(first)
            if rest:
                remainder = Text(" ").join(rest)
                for continuation in wrap_text(remainder, max(1, width - indent)):
                    line = Text(" " * indent)
                    line.append_text(continuation)
                    lines.append(line)
        return lines


# ---------------------------------------------------------------------------
# Construção de conteúdo
# ---------------------------------------------------------------------------


def incident_items(
    incidents: Iterable[Incident],
    active_dtcs: dict | None = None,
    *,
    limit: int = 12,
) -> list[Text]:
    """DTCs ativos primeiro; depois o histórico mais recente."""

    items: list[Text] = []
    for record in sorted((active_dtcs or {}).values(), key=lambda item: item.code):
        if not record.active:
            continue
        level = pres.severity_level(record.severity)
        line = Text(f"{pres.symbol(level)} ", style=LEVEL_COLORS[level])
        line.append(f"Ativo  DTC 0x{record.code:04X}", style=f"bold {LEVEL_COLORS[level]}")
        line.append(f"  {dtc_description(record.code)}", style=palette.TEXT_PRIMARY)
        items.append(line)
    for incident in list(incidents)[::-1][:limit]:
        level = pres.severity_level(incident.severity)
        if incident.severity == Severity.INFO:
            level = pres.Level.NORMAL
        color = LEVEL_COLORS[level] if incident.severity != Severity.INFO else palette.STATE_INFO
        line = Text(f"{pres.symbol(level)} ", style=color)
        line.append(pres.fmt_clock(incident.last_wall_time), style=palette.TEXT_DIM)
        line.append(f"  {incident.message}", style=palette.TEXT_PRIMARY)
        if incident.count > 1:
            line.append(f"  ({incident.count} vezes desde {pres.fmt_clock(incident.first_wall_time)})", style=palette.TEXT_MUTED)
        items.append(line)
    return items


def node_title(node: PhysicalNode) -> str:
    if node.node_type == "DIRECT_SENSOR_HOST":
        return "Conexão direta USB"
    return f"Módulo {node.parent_node_id:02d}"


def hanging_lines(segments: Sequence[Text], width: int, *, indent: int = 0, gap: int = 2) -> list[Text]:
    """Empacota segmentos em linhas com recuo; nenhuma linha passa de ``width``."""

    width = max(8, width)
    available = max(4, width - indent)
    packed = pres.pack_segments([segment.cell_len for segment in segments], available, gap)
    lines: list[Text] = []
    for indices in packed:
        if len(indices) == 1 and segments[indices[0]].cell_len > available:
            for piece in wrap_text(segments[indices[0]], available):
                line = Text(" " * indent)
                line.append_text(piece)
                lines.append(line)
            continue
        line = Text(" " * indent)
        for position, index in enumerate(indices):
            if position:
                line.append(" " * gap)
            line.append_text(segments[index])
        lines.append(line)
    return lines


def _join_lines(lines: Sequence[Text]) -> Text:
    return Text("\n").join(lines)


def node_prompt(node: PhysicalNode, width: int = 80) -> Text:
    condition = pres.node_condition(node)
    color = LEVEL_COLORS[condition.level]
    head = [Text(f"{pres.symbol(condition.level)} ", style=color) + Text(node_title(node), style=f"bold {palette.TEXT_PRIMARY}")]
    if node.node_type != "DIRECT_SENSOR_HOST":
        head.append(Text(pres.role_label(node.role), style=palette.TEXT_SECONDARY))
    head.append(Text(pres.level_label(condition.level), style=color))
    lines = hanging_lines(head, width, indent=0)
    detail = condition.reasons[0] if condition.reasons else ""
    if not detail and node.node_type != "DIRECT_SENSOR_HOST":
        count = len(node.sensors)
        sensors = "sem sensores sem fio" if count == 0 else ("1 sensor sem fio" if count == 1 else f"{count} sensores sem fio")
        detail_segments = [Text(sensors + ",", style=palette.TEXT_MUTED), Text(f"último contato {pres.fmt_age(pres.node_age(node))}", style=palette.TEXT_MUTED)]
        lines += hanging_lines(detail_segments, width, indent=2, gap=1)
    elif detail:
        lines += hanging_lines([Text(detail, style=palette.TEXT_MUTED)], width, indent=2)
    return _join_lines(lines)


def _sensor_metric_segments(sensor: SensorNode) -> list[Text]:
    sample = sensor.latest_telemetry
    if sample is None:
        return []
    parts = []
    if sample.rms is not None:
        parts.append(f"RMS {pres.fmt_number(sample.rms, 3, sample.rms_unit)}")
    if sample.peak_frequency_hz is not None and sample.fft_valid is not False:
        parts.append(f"pico {pres.fmt_number(sample.peak_frequency_hz, 1, 'Hz')}")
    if sample.ppv_mm_s is not None:
        parts.append(f"PPV {pres.fmt_number(sample.ppv_mm_s, 2, 'mm/s')}")
    if sample.battery.valid and sample.battery.percentage is not None:
        parts.append(f"bateria {sample.battery.percentage:.0f}%")
    return [Text(part, style=palette.TEXT_MUTED) for part in parts]


def sensor_prompt(sensor: SensorNode, width: int = 80, *, indent: int = 2) -> Text:
    condition = pres.sensor_condition(sensor)
    color = LEVEL_COLORS[condition.level]
    head = [
        Text(f"{pres.symbol(condition.level)} ", style=color) + Text(f"Sensor {sensor.logical_id}", style=f"bold {palette.TEXT_PRIMARY}"),
        Text(pres.profile_label(sensor.profile_id), style=palette.TEXT_SECONDARY),
        Text(pres.level_label(condition.level), style=color),
    ]
    lines = hanging_lines(head, width, indent=indent)
    if condition.reasons:
        lines += hanging_lines([Text(condition.reasons[0], style=palette.TEXT_MUTED)], width, indent=indent + 2)
    else:
        metrics = _sensor_metric_segments(sensor)
        if metrics:
            lines += hanging_lines(metrics, width, indent=indent + 2, gap=3)
    return _join_lines(lines)


def _is_header(item_id: str) -> bool:
    """Itens "__empty__" e "group:*" são rótulos, não opções selecionáveis."""

    return item_id == "__empty__" or item_id.startswith("group:")


class StableOptionList(OptionList):
    """OptionList que atualiza o conteúdo sem perder o item realçado."""

    def __init__(self, *, empty: str = "Nada para mostrar.", **kwargs) -> None:
        super().__init__(**kwargs)
        self._empty = empty
        self._ids: tuple[str, ...] = ()
        self._prompts: dict[str, tuple] = {}

    def set_items(self, items: Sequence[tuple[str, Text]]) -> None:
        if not items:
            items = [("__empty__", Text(self._empty, style=palette.TEXT_MUTED))]
        ids = tuple(item_id for item_id, _ in items)
        if ids == self._ids:
            for item_id, prompt in items:
                key = _text_key(prompt)
                if self._prompts.get(item_id) != key:
                    self._prompts[item_id] = key
                    self.replace_option_prompt(item_id, prompt)
            return
        highlighted_id = None
        if self.highlighted is not None and self.option_count:
            try:
                highlighted_id = self.get_option_at_index(self.highlighted).id
            except Exception:
                highlighted_id = None
        self.clear_options()
        self.add_options([Option(prompt, id=item_id, disabled=_is_header(item_id)) for item_id, prompt in items])
        self._ids = ids
        self._prompts = {item_id: _text_key(prompt) for item_id, prompt in items}
        selectable = [index for index, item_id in enumerate(ids) if not _is_header(item_id)]
        if selectable:
            target = ids.index(highlighted_id) if highlighted_id in ids else selectable[0]
            self.highlighted = target

    @property
    def highlighted_id(self) -> str | None:
        if self.highlighted is None or not self.option_count:
            return None
        option_id = self.get_option_at_index(self.highlighted).id
        return None if option_id is None or _is_header(option_id) else option_id


    @property
    def prompt_width(self) -> int:
        """Largura útil para montar os itens (reserva 1 coluna para a barra de rolagem)."""

        width = self.scrollable_content_region.width or self.content_size.width
        return max(20, (width or 80) - 1)


class EquipmentList(StableOptionList):
    """Módulos CAN e, logo abaixo de cada um, seus sensores sem fio."""

    def show(self, state: AppState) -> None:
        width = self.prompt_width
        items: list[tuple[str, Text]] = []
        for node_id, node in sorted(state.nodes.items()):
            items.append((f"node:{node_id:02d}", node_prompt(node, width)))
            for _child_id, sensor in sorted(node.sensors.items()):
                items.append((f"sensor:{sensor.logical_id}", sensor_prompt(sensor, width)))
        self.set_items(items)


class MessageLog(RichLog):
    """Registro de mensagens operacionais (texto quebra; nunca rola na horizontal)."""

    LEVEL_STYLES = {
        "DEBUG": palette.TEXT_DIM,
        "INFO": palette.STATE_INFO,
        "WARNING": palette.STATE_WARNING,
        "ERROR": palette.STATE_CRITICAL,
        "CRITICAL": palette.STATE_CRITICAL,
    }
    LEVEL_WORDS = {"DEBUG": "depuração", "INFO": "info", "WARNING": "aviso", "ERROR": "erro", "CRITICAL": "crítico"}

    def __init__(self, **kwargs) -> None:
        super().__init__(markup=False, wrap=True, highlight=False, max_lines=1000, auto_scroll=True, min_width=1, **kwargs)

    def add_entry(self, entry: MessageEntry) -> None:
        line = Text()
        line.append(entry.clock, style=palette.TEXT_DIM)
        line.append(f"  {self.LEVEL_WORDS.get(entry.level, entry.level.lower())}", style=self.LEVEL_STYLES.get(entry.level, palette.TEXT_PRIMARY))
        line.append(f"  {entry.source}", style=palette.TEXT_MUTED)
        line.append(f"  {entry.message}", style=palette.TEXT_PRIMARY)
        self.write(line)


class MessageEntry:
    __slots__ = ("clock", "level", "message", "source")

    def __init__(self, clock: str, level: str, source: str, message: str) -> None:
        self.clock = clock
        self.level = level.upper()
        self.source = source
        self.message = message
