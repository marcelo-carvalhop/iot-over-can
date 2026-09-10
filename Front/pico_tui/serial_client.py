"""Transporte serial bloqueante isolado em thread.

Suporta o console USB CDC do Pico, o gateway textual e pseudo-terminais
usados nos testes. A camada remove o prompt EDGE> e o eco do console direto,
mas pode desativar esse comportamento quando o gateway é detectado.
"""
from __future__ import annotations

import collections
import errno
import os
import select
import threading
import time
from dataclasses import dataclass
from typing import Callable, Optional, Protocol

import serial
from serial.tools import list_ports

PROMPT = b"EDGE> "


@dataclass(slots=True)
class PortInfo:
    device: str
    description: str
    hwid: str = ""

    def label(self) -> str:
        """Rótulo de porta em texto literal; o OptionList correspondente usa markup=False."""
        detail = f" — {self.description}" if self.description else ""
        hwid = f" [{self.hwid}]" if self.hwid else ""
        return f"{self.device}{detail}{hwid}"


def list_available_ports() -> list[PortInfo]:
    ports: list[PortInfo] = []
    for item in sorted(list_ports.comports(), key=lambda p: p.device):
        ports.append(
            PortInfo(
                device=item.device,
                description=item.description or "porta serial",
                hwid=item.hwid or "",
            )
        )
    return ports


class _Backend(Protocol):
    @property
    def is_open(self) -> bool: ...
    def read(self, size: int) -> bytes: ...
    def write(self, data: bytes) -> int: ...
    def flush(self) -> None: ...
    def close(self) -> None: ...


class _PySerialBackend:
    def __init__(self, instance: serial.SerialBase) -> None:
        self.instance = instance

    @property
    def is_open(self) -> bool:
        return bool(self.instance.is_open)

    def read(self, size: int) -> bytes:
        return self.instance.read(size)

    def write(self, data: bytes) -> int:
        return self.instance.write(data)

    def flush(self) -> None:
        self.instance.flush()

    def close(self) -> None:
        self.instance.close()


class _PosixFdBackend:
    """Fallback para PTYs em ambientes onde pyserial falha no tcflush.

    Não substitui pyserial em hardware real; é acionado apenas quando a
    abertura convencional falha com ENOTTY em uma plataforma POSIX.
    """

    def __init__(self, path: str, timeout: float) -> None:
        self.timeout = timeout
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self._open = True

    @property
    def is_open(self) -> bool:
        return self._open

    def read(self, size: int) -> bytes:
        if not self._open:
            return b""
        readable, _, _ = select.select([self.fd], [], [], self.timeout)
        if not readable:
            return b""
        try:
            return os.read(self.fd, size)
        except BlockingIOError:
            return b""

    def write(self, data: bytes) -> int:
        if not self._open:
            raise OSError("porta fechada")
        total = 0
        while total < len(data):
            _, writable, _ = select.select([], [self.fd], [], self.timeout)
            if not writable:
                continue
            total += os.write(self.fd, data[total:])
        return total

    def flush(self) -> None:
        return

    def close(self) -> None:
        if self._open:
            self._open = False
            os.close(self.fd)


class SerialClient:
    def __init__(self, port: str, baudrate: int = 115200, read_timeout: float = 0.2):
        self.port = port
        self.baudrate = baudrate
        self.read_timeout = read_timeout
        self._backend: Optional[_Backend] = None
        self._stop_event = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._write_lock = threading.Lock()
        self._pending_echoes: collections.deque[str] = collections.deque()
        self._echo_lock = threading.Lock()
        self._filter_console_echo = True
        self._disconnect_reported = False
        self._last_write_monotonic = 0.0

    @property
    def is_open(self) -> bool:
        return self._backend is not None and self._backend.is_open

    def set_console_echo_filter(self, enabled: bool) -> None:
        self._filter_console_echo = enabled
        if not enabled:
            with self._echo_lock:
                self._pending_echoes.clear()

    def open(self) -> None:
        self.close()
        self._disconnect_reported = False
        try:
            if "://" in self.port:
                instance = serial.serial_for_url(
                    self.port,
                    baudrate=self.baudrate,
                    timeout=self.read_timeout,
                    write_timeout=1.0,
                )
            else:
                instance = serial.Serial(
                    port=self.port,
                    baudrate=self.baudrate,
                    bytesize=serial.EIGHTBITS,
                    parity=serial.PARITY_NONE,
                    stopbits=serial.STOPBITS_ONE,
                    timeout=self.read_timeout,
                    write_timeout=1.0,
                )
            self._backend = _PySerialBackend(instance)
        except (serial.SerialException, OSError) as exc:
            # pyserial 3.5 pode falhar ao executar tcflush em alguns PTYs
            # de contêiner. O fallback é limitado a ENOTTY e caminhos POSIX.
            error_number = getattr(exc, "errno", None)
            args = getattr(exc, "args", ())
            if error_number is None and args and isinstance(args[0], int):
                error_number = args[0]
            if os.name == "posix" and self.port.startswith("/dev/") and error_number in {errno.ENOTTY, 25}:
                self._backend = _PosixFdBackend(self.port, self.read_timeout)
            else:
                raise

    def start_reading(
        self,
        on_line: Callable[[str], None],
        on_error: Callable[[str], None],
        on_disconnect: Callable[[], None],
    ) -> None:
        self._stop_event.clear()
        self._thread = threading.Thread(
            target=self._read_loop,
            args=(on_line, on_error, on_disconnect),
            daemon=True,
            name="serial-reader",
        )
        self._thread.start()

    def _read_loop(
        self,
        on_line: Callable[[str], None],
        on_error: Callable[[str], None],
        on_disconnect: Callable[[], None],
    ) -> None:
        backend = self._backend
        if backend is None:
            return
        buffer = b""
        while not self._stop_event.is_set():
            try:
                chunk = backend.read(512)
                if not chunk:
                    time.sleep(0.005)
                    continue
                buffer += chunk
                if self._filter_console_echo and PROMPT in buffer:
                    buffer = buffer.replace(PROMPT, b"")
                # Normaliza CRLF, CR isolado e LF.
                buffer = buffer.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
                while b"\n" in buffer:
                    raw, buffer = buffer.split(b"\n", 1)
                    text = raw.decode("utf-8", errors="replace").strip()
                    if not text:
                        continue
                    if self._filter_console_echo and self._consume_echo_if_pending(text):
                        continue
                    on_line(text)
                if self._filter_console_echo and buffer in {b"EDGE>", b"EDGE> "}:
                    buffer = b""
            except (serial.SerialException, OSError) as exc:
                if self._stop_event.is_set():
                    return
                on_error(str(exc))
                self._report_disconnect(on_disconnect)
                return
        self._report_disconnect(on_disconnect, only_if_open=False)

    def _report_disconnect(self, callback: Callable[[], None], *, only_if_open: bool = True) -> None:
        if self._disconnect_reported:
            return
        if only_if_open and self._stop_event.is_set():
            return
        self._disconnect_reported = True
        callback()

    def _consume_echo_if_pending(self, text: str) -> bool:
        with self._echo_lock:
            if self._pending_echoes and self._pending_echoes[0].strip().casefold() == text.strip().casefold():
                self._pending_echoes.popleft()
                return True
        return False

    def write_line(self, text: str) -> None:
        if not self.is_open or self._backend is None:
            raise RuntimeError("porta serial desconectada")
        if self._filter_console_echo:
            with self._echo_lock:
                self._pending_echoes.append(text)
        data = (text.rstrip("\r\n") + "\r\n").encode("utf-8")
        with self._write_lock:
            now = time.monotonic()
            wait = 0.015 - (now - self._last_write_monotonic)
            if wait > 0:
                time.sleep(wait)
            self._backend.write(data)
            self._backend.flush()
            self._last_write_monotonic = time.monotonic()

    def write_raw(self, data: bytes) -> None:
        if not self.is_open or self._backend is None:
            raise RuntimeError("porta serial desconectada")
        with self._write_lock:
            self._backend.write(data)
            self._backend.flush()

    def close(self) -> None:
        self._stop_event.set()
        backend = self._backend
        self._backend = None
        if backend is not None:
            try:
                backend.close()
            except Exception:
                pass
        if self._thread is not None and self._thread is not threading.current_thread():
            self._thread.join(timeout=1.0)
        self._thread = None
        with self._echo_lock:
            self._pending_echoes.clear()
