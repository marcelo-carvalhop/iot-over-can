"""Gerencia o ciclo de vida da bancada ao iniciar a propria TUI.

Sem servidor remoto. Usa o mesmo protocolo serial da Probe em um PTY Linux.
"""
from __future__ import annotations

import os
import sys
import threading
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator

from sim_bridge import SimBridge


class SimStartupError(RuntimeError):
    pass


@contextmanager
def local_simulation(binary: Path, record: Path | None = None) -> Iterator[str]:
    binary = binary.expanduser().resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise SimStartupError(f"Executavel de simulacao ausente: {binary}. Compile com Codigo/scripts/build_network_sim.sh")
    bridge = SimBridge(binary, record)
    errors: list[BaseException] = []

    def worker() -> None:
        try:
            bridge.serve()
        except BaseException as exc:
            errors.append(exc)
            bridge.ready.set()

    thread = threading.Thread(target=worker, daemon=True, name="iot-over-can-sim-bridge")
    thread.start()
    try:
        if not bridge.ready.wait(15):
            raise SimStartupError("A ponte nao sinalizou inicializacao em 15 segundos")
        if errors:
            raise SimStartupError(f"Falha da ponte: {errors[0]}")
        if bridge.proc is None or bridge.proc.poll() is not None or bridge.slave is None:
            raise SimStartupError("Processo de simulacao nao iniciou")
        port = os.ttyname(bridge.slave)
        print(f"[sim] Probe virtual: {port}", file=sys.stderr, flush=True)
        print(f"[sim] Controle: {bridge.socket_path}", file=sys.stderr, flush=True)
        yield port
    finally:
        bridge.stop.set()
        thread.join(timeout=5)
        if thread.is_alive():
            raise SimStartupError("A ponte nao encerrou dentro do limite")
