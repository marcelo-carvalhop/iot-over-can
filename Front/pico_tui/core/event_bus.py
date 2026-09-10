from __future__ import annotations

import asyncio
import inspect
import logging
from collections import defaultdict
from collections.abc import Awaitable, Callable
from typing import Any, TypeVar

from pico_tui.core.events import Event

logger = logging.getLogger(__name__)

E = TypeVar("E", bound=Event)
EventHandler = Callable[[Any], Awaitable[None] | None]


class EventBus:
    """Barramento assíncrono simples e tolerante a falhas de handlers."""

    def __init__(self) -> None:
        self._handlers: dict[type[Event], list[EventHandler]] = defaultdict(list)

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
        for handler in tuple(handlers):
            try:
                result = handler(event)
                if inspect.isawaitable(result):
                    awaitables.append(result)
            except Exception:
                logger.exception("Falha no handler %r", handler)

        if awaitables:
            results = await asyncio.gather(*awaitables, return_exceptions=True)
            for result in results:
                if isinstance(result, Exception):
                    logger.error("Falha em handler assíncrono: %s", result)

    def clear(self) -> None:
        self._handlers.clear()
