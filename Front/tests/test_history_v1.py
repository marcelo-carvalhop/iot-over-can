"""Regressões da persistência, redução de métricas e isolamento de sessões."""
from __future__ import annotations

import asyncio

from pico_tui.core.event_bus import EventBus
from pico_tui.core.events import (
    BusStatsReceived, DtcCleared, NetworkEventReceived,
    WirelessAssociationReceived, WirelessLinkReceived,
)
from pico_tui.history.service import HistoryService
from pico_tui.history.storage import HistoryStore


def test_records_session_events_and_network_snapshot(tmp_path):
    store = HistoryStore(tmp_path / "history.sqlite3")
    bus = EventBus()
    history = HistoryService(bus, store, "s-1", origin="TEST")

    async def publish():
        await bus.publish(BusStatsReceived(1000, 1000, 12, 1200, 1.2, 3.1, rx_errors=1))
        await bus.publish(WirelessAssociationReceived(1, 1, "abcd", "VIBRATION", "BOUND", -44))
        await bus.publish(WirelessAssociationReceived(1, 1, "abcd", "VIBRATION", "BOUND", -44))
        await bus.publish(WirelessLinkReceived(1, 1, "SECURE", rtt_ms=2.5))
        await bus.publish(NetworkEventReceived("ELECTION_REQUESTED", 1))
        await bus.publish(DtcCleared(1, 1, 4))
    asyncio.run(publish())
    history.close()
    sessions = store.list_sessions()
    assert len(sessions) == 1 and sessions[0].ended_epoch is not None
    events = store.list_events("s-1")
    assert [e.kind for e in events] == ["ASSOCIATION", "LINK_STATE", "ELECTION_REQUESTED", "DTC_CLEARED"]
    assert events[0].details["bus_at_observation"]["load_percent"] == 1.2
    assert store.nearest_bus("s-1", sessions[0].ended_epoch) is not None
    assert store.count_events("s-1") == 4


def test_sessions_are_separate_and_database_survives_reopening(tmp_path):
    path = tmp_path / "history.sqlite3"
    store = HistoryStore(path)
    bus1 = EventBus()
    one = HistoryService(bus1, store, "one")
    asyncio.run(bus1.publish(NetworkEventReceived("JOIN_OBSERVED", 2)))
    one.close()
    bus2 = EventBus()
    two = HistoryService(bus2, store, "two")
    asyncio.run(bus2.publish(NetworkEventReceived("ELECTION_REQUESTED", 3)))
    two.close()
    fresh = HistoryStore(path)
    assert fresh.count_events("one") == 1
    assert fresh.count_events("two") == 1
    assert fresh.list_sessions()[0].id == "two"


def test_repeated_link_reports_do_not_pollute_timeline(tmp_path):
    bus = EventBus()
    store = HistoryStore(tmp_path / "db.sqlite")
    history = HistoryService(bus, store, "s")
    async def publish():
        for _ in range(10):
            await bus.publish(WirelessLinkReceived(1, 1, "SECURE"))
        await bus.publish(WirelessLinkReceived(1, 1, "OFFERING"))
    asyncio.run(publish())
    history.close()
    assert store.count_events("s") == 2
