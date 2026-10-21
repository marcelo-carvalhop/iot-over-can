#!/usr/bin/env python3
"""Controle da bancada por socket local, sem privilegios de hardware."""
from __future__ import annotations

import argparse
import os
import socket
import stat
import tempfile
from pathlib import Path


def discover_socket() -> str:
    root = Path(tempfile.gettempdir())
    live: list[str] = []
    for candidate in root.glob("ioc-sim-*/control.sock"):
        try:
            info = candidate.stat()
            if info.st_uid != os.getuid() or not stat.S_ISSOCK(info.st_mode):
                continue
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as conn:
                conn.settimeout(0.25)
                conn.connect(str(candidate))
            live.append(str(candidate))
        except (OSError, TimeoutError):
            continue
    if len(live) == 1:
        return live[0]
    if not live:
        raise SystemExit("Nenhum simulador ativo encontrado; use --socket ou inicie a TUI com --sim")
    raise SystemExit("Multiplos simuladores ativos; escolha um com --socket: " + ", ".join(live))


def main() -> None:
    ap = argparse.ArgumentParser(description="Controle interativo da bancada virtual")
    ap.add_argument("--socket", help="Caminho CONTROL impresso pela ponte. Sem ele, busca unica sessao local.")
    ap.add_argument("command", nargs="+", help='Ex.: STATUS; NODE 4 ADD')
    args = ap.parse_args()
    command = " ".join(args.command)
    path = args.socket or discover_socket()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(25)
        client.connect(path)
        client.sendall((command + "\n").encode("utf-8"))
        client.shutdown(socket.SHUT_WR)
        received = bytearray()
        while True:
            block = client.recv(8192)
            if not block:
                break
            received.extend(block)
    print(received.decode("utf-8", errors="replace"), end="")
    if b"#CTRL ERROR" in received:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
