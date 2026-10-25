#!/usr/bin/env python3
"""Ponte local entre sim_network --interactive, uma PTY e comandos Unix socket.

Nao utiliza sockets TCP externos; a PTY e a TUI veem o protocolo textual
original da Probe 00, sem os prefixos de tempo da bancada virtual.
"""
from __future__ import annotations

import argparse
import os
import pty
import queue
import re
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import tty
from pathlib import Path

STAMP = re.compile(r"^@([0-9]+) (.*)$")


class SimBridge:
    def __init__(self, binary: Path, record: Path | None = None) -> None:
        self.binary = binary
        self.record = record
        self.master: int | None = None
        self.slave: int | None = None
        self.proc: subprocess.Popen[str] | None = None
        self.control_queue: queue.Queue[str] = queue.Queue()
        self.write_lock = threading.Lock()
        self.control_lock = threading.Lock()
        self.stop = threading.Event()
        self.ready = threading.Event()
        self.dropped_lines = 0
        self.runtime_dir: Path | None = None
        self.socket_path: Path | None = None
        self.server: socket.socket | None = None

    def _send(self, line: str) -> None:
        if self.proc is None or self.proc.stdin is None:
            raise RuntimeError("Simulador encerrado")
        with self.write_lock:
            self.proc.stdin.write(line.rstrip("\r\n") + "\n")
            self.proc.stdin.flush()

    def _write_pty_line(self, message: bytes, *, timeout: float = 0.75) -> bool:
        """Entrega uma linha inteira, inclusive quando os.write() é parcial.

        A PTY é não bloqueante para não travar a simulação se a TUI parar
        de consumir. Só abandona a mensagem depois do prazo informado.
        """
        assert self.master is not None
        deadline = time.monotonic() + timeout
        offset = 0
        while offset < len(message) and not self.stop.is_set():
            try:
                written = os.write(self.master, message[offset:])
                if written > 0:
                    offset += written
                    continue
            except (BlockingIOError, InterruptedError):
                pass
            except OSError:
                break
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            try:
                _, writable, _ = select.select([], [self.master], [], remaining)
            except (OSError, ValueError):
                break
            if not writable:
                break
        return offset == len(message)

    def _sim_output(self) -> None:
        assert self.proc is not None and self.proc.stdout is not None
        fp = self.record.open("w", encoding="utf-8", buffering=1) if self.record else None
        try:
            for raw in self.proc.stdout:
                raw = raw.rstrip("\r\n")
                m = STAMP.match(raw)
                if m:
                    if fp:
                        fp.write(raw + "\n")
                    message = (m.group(2) + "\r\n").encode("utf-8")
                    assert self.master is not None
                    if not self._write_pty_line(message):
                        # A PTY estava indisponível por mais que o prazo. O log
                        # permanece completo, e o descarte é contabilizado.
                        self.dropped_lines += 1
                elif raw.startswith("#CTRL"):
                    if raw not in ("#CTRL OK READY mode=INTERACTIVE protocol=1",) and not raw.startswith("#CTRL INFO"):
                        if raw != "#CTRL OK PROBE":
                            self.control_queue.put(raw)
                    if raw.startswith("#CTRL ERROR"):
                        print(raw, file=sys.stderr, flush=True)
                elif raw.startswith("# t="):
                    # Anotacoes dos testes sao enviadas somente ao log interno.
                    pass
            self.stop.set()
        finally:
            if fp:
                fp.close()

    def _pty_input(self) -> None:
        assert self.master is not None
        pending = b""
        while not self.stop.is_set():
            readable, _, _ = select.select([self.master], [], [], 0.2)
            if not readable:
                continue
            try:
                chunk = os.read(self.master, 4096)
            except BlockingIOError:
                continue
            except OSError:
                break
            if not chunk:
                continue
            pending += chunk.replace(b"\r", b"\n")
            if len(pending) > 65536:
                pending = b""
            while b"\n" in pending:
                raw, pending = pending.split(b"\n", 1)
                command = raw.decode("utf-8", errors="replace").strip()
                if command:
                    try:
                        self._send("PROBE " + command)
                    except (BrokenPipeError, RuntimeError):
                        self.stop.set()
                        break

    def _handle_control(self, conn: socket.socket) -> None:
        conn.settimeout(30)
        data = b""
        while b"\n" not in data and len(data) <= 16384:
            part = conn.recv(4096)
            if not part:
                break
            data += part
        if len(data) > 16384:
            conn.sendall(b"#CTRL ERROR comando muito longo\n")
            return
        line = data.partition(b"\n")[0].decode("utf-8", errors="replace").strip()
        if not line:
            # Probe de disponibilidade pode conectar e fechar sem enviar dados.
            return
        is_multiline = line.split()[0].upper() in {"STATUS", "HELP"}
        with self.control_lock:
            # Respostas de comandos antigos (ex.: da PTY) nao sao confundidas.
            while True:
                try:
                    self.control_queue.get_nowait()
                except queue.Empty:
                    break
            self._send(line)
            if line.split()[0].upper() == "PROBE":
                # O ACK operacional da Probe aparece na TUI. A confirmacao
                # aqui atesta somente que o comando entrou no stdin simulado.
                conn.sendall(b"#CTRL OK PROBE DISPATCHED (verifique ACK na TUI)\n")
                return
            while not self.stop.is_set():
                try:
                    response = self.control_queue.get(timeout=20)
                except queue.Empty:
                    conn.sendall(b"#CTRL ERROR tempo limite sem resposta\n")
                    return
                conn.sendall((response + "\n").encode("utf-8"))
                if response.startswith("#CTRL ERROR") or (not is_multiline and response.startswith("#CTRL OK")) or response == "#CTRL END":
                    return

    def serve(self) -> int:
        self.runtime_dir = Path(tempfile.mkdtemp(prefix="ioc-sim-"))
        os.chmod(self.runtime_dir, 0o700)
        self.socket_path = self.runtime_dir / "control.sock"
        self.master, self.slave = pty.openpty()
        tty.setraw(self.slave)
        os.set_blocking(self.master, False)
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server.bind(str(self.socket_path))
        os.chmod(self.socket_path, 0o600)
        self.server.listen(5)
        self.server.settimeout(0.5)
        self.proc = subprocess.Popen(
            [str(self.binary), "--interactive"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            bufsize=1,
        )
        # stderr e drenado para impedir bloqueio do processo em buffer cheio.
        def stderr_worker() -> None:
            assert self.proc is not None and self.proc.stderr is not None
            for text in self.proc.stderr:
                print("[sim]", text.rstrip(), file=sys.stderr, flush=True)

        jobs = [
            threading.Thread(target=self._sim_output, daemon=True),
            threading.Thread(target=self._pty_input, daemon=True),
            threading.Thread(target=stderr_worker, daemon=True),
        ]
        for job in jobs:
            job.start()
        self.ready.set()
        print(f"PORT={os.ttyname(self.slave)}", flush=True)
        print(f"CONTROL={self.socket_path}", flush=True)
        print("A TUI pode conectar com --mode probe --port <PORT>.", flush=True)
        try:
            while not self.stop.is_set():
                try:
                    conn, _ = self.server.accept()
                except socket.timeout:
                    continue
                with conn:
                    try:
                        self._handle_control(conn)
                    except (OSError, RuntimeError) as err:
                        print(f"[bridge] comando falhou: {err}", file=sys.stderr, flush=True)
        except KeyboardInterrupt:
            pass
        finally:
            self.stop.set()
            if self.proc and self.proc.poll() is None:
                try:
                    self._send("QUIT")
                    self.proc.wait(timeout=2)
                except (OSError, subprocess.TimeoutExpired):
                    self.proc.terminate()
                    try:
                        self.proc.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        self.proc.kill()
                        self.proc.wait()
            if self.server:
                self.server.close()
            # Encerrar leitores antes de fechar os pipes usados nas threads.
            for job in jobs:
                job.join(timeout=2)
            if self.proc:
                for pipe in (self.proc.stdin, self.proc.stdout, self.proc.stderr):
                    if pipe and not pipe.closed:
                        pipe.close()
            for fd in (self.master, self.slave):
                if fd is not None:
                    os.close(fd)
            if self.runtime_dir:
                shutil.rmtree(self.runtime_dir, ignore_errors=True)
            if self.dropped_lines:
                print(f"[bridge] ATENCAO: {self.dropped_lines} mensagens para a PTY descartadas; dados completos em --record.", file=sys.stderr)
        return self.proc.returncode if self.proc and self.proc.returncode is not None else 0


def main() -> None:
    parser = argparse.ArgumentParser(description="Bancada virtual IoT-over-CAN, com porta pseudo-serial local")
    parser.add_argument("--binary", type=Path, default=Path("/tmp/ioc_sim_network"))
    parser.add_argument("--record", type=Path, help="Grava mensagens da Probe no formato de replay da TUI")
    args = parser.parse_args()
    binary = args.binary.expanduser().resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error(f"Executavel ausente: {binary}. Compile antes com build_network_sim.sh")
    raise SystemExit(SimBridge(binary, args.record).serve())


if __name__ == "__main__":
    main()
