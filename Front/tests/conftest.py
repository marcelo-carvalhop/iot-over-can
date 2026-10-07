from __future__ import annotations

import asyncio
import json
import os
from pathlib import Path

import pytest

from tests.fake_firmware import FakeFirmware


@pytest.fixture
def security_file(tmp_path: Path) -> Path:
    """security.json válido (0600) com token de manutenção, como em produção."""

    path = tmp_path / "security.json"
    path.write_text(json.dumps({"device_admin_token": "0x1234"}))
    if os.name == "posix":
        path.chmod(0o600)
    return path


@pytest.fixture
def prefs_file(tmp_path: Path) -> Path:
    return tmp_path / "tui.json"


@pytest.fixture
async def fake_fw():
    firmware = FakeFirmware()
    firmware.start()
    await asyncio.sleep(0.15)
    yield firmware
    firmware.stop()


async def settle(pilot, rounds: int = 4, delay: float = 0.12) -> None:
    for _ in range(rounds):
        await asyncio.sleep(delay)
        await pilot.pause()


async def wait_for(pilot, predicate, timeout: float = 3.0) -> bool:
    """Espera uma condição assíncrona (respostas do firmware, ciclo de 0,5 s da interface)."""

    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    while loop.time() < deadline:
        if predicate():
            return True
        await asyncio.sleep(0.05)
        await pilot.pause()
    return predicate()
