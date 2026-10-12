from __future__ import annotations

import asyncio
import inspect
import logging
import time
from collections import defaultdict
from collections.abc import Awaitable, Callable
from typing import Any, TypeVar

from pico_tui.core.events import Event, LogEvent

logger = logging.getLogger(__name__)

E = TypeVar("E", bound=Event)
EventHandler = Callable[[Any], Awaitable[None] | None]


class EventBus:
    """Barramento assíncrono simples e tolerante a falhas de handlers.

    A falha de um handler não interrompe os demais nem quem publicou. Ela vai
    para o ``logging`` e, como ``LogEvent`` de erro, para as mensagens da TUI e
    o registro da sessão: no máximo uma vez a cada ``FAILURE_REPORT_S`` segundos
    para o mesmo handler e tipo de exceção, para um defeito repetitivo não
    inundar a tela. Falhas ao tratar o próprio ``LogEvent`` só vão para o
    ``logging``, o que evita recursão.
    """

    FAILURE_REPORT_S = 30.0

    def __init__(self) -> None:
        self._handlers: dict[type[Event], list[EventHandler]] = defaultdict(list)
        self._failures_reported: dict[tuple[str, str], float] = {}

    def subscribe(self, event_type: type[E], handler: EventHandler) -> None:
        handlers = self._handlers[event_type]
        if handler not in handlers:
            handlers.append(handler)

    def unsubscribe(self, event_type: type[E], handler: EventHandler) -> None:
        handlers = self._handlers.get(event_type, [])
        if handler in handlers:
            handlers.remove(handler)

    async def publish(self, event: Event) -> None:
        handlers: list[EventHandler] = []
        for registered_type, registered_handlers in self._handlers.items():
            if isinstance(event, registered_type):
                handlers.extend(registered_handlers)

        awaitables: list[Awaitable[None]] = []
        waiting: list[EventHandler] = []
        failures: list[tuple[EventHandler, Exception]] = []
        for handler in tuple(handlers):
            try:
                result = handler(event)
                if inspect.isawaitable(result):
                    awaitables.append(result)
                    waiting.append(handler)
            except Exception as exc:
                logger.exception("Falha no handler %r", handler)
                failures.append((handler, exc))

        if awaitables:
            results = await asyncio.gather(*awaitables, return_exceptions=True)
            for handler, result in zip(waiting, results, strict=True):
                if isinstance(result, Exception):
                    logger.error("Falha em handler assíncrono: %s", result)
                    failures.append((handler, result))

        if failures and not isinstance(event, LogEvent):
            await self._report(event, failures)

    async def _report(self, event: Event, failures: list[tuple[EventHandler, Exception]]) -> None:
        now = time.monotonic()
        for handler, exc in failures:
            name = getattr(handler, "__qualname__", repr(handler))
            key = (name, type(exc).__name__)
            if now - self._failures_reported.get(key, -self.FAILURE_REPORT_S) < self.FAILURE_REPORT_S:
                continue
            self._failures_reported[key] = now
            await self.publish(
                LogEvent("ERROR", f"Falha interna ao tratar {type(event).__name__} em {name}: {exc}", "INTERNO")
            )

    def clear(self) -> None:
        self._handlers.clear()
