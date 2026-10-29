#!/usr/bin/env python3
"""Liga o painel M5Stack PaperColor físico, pela USB, à bancada virtual.

A USB faz o papel do Wi-Fi: o PaperColor executa o próprio firmware (enlace
HMAC-SHA256, retrato, página, teclado, botões) e troca com o Node virtual os
mesmos datagramas autenticados que trocaria pelo ponto de acesso IOC-NN. A
bancada entrega ao painel as ofertas BLE assinadas dos Nodes que o "ouvem",
anuncia o perfil PANEL em nome dele e mantém um espelho da página para o
Simulation Studio.

Protocolo de linha com o painel (115200, USB CDC):
  painel -> ponte   #IOC HELLO|READY uuid=0x... profile=PANEL ...
                    #IOC JOIN <node> | #IOC LEAVE | #IOC UP <hex> | #IOC LOG <texto>
                    #IOC OFF (desligado pelo operador)
  ponte -> painel   #IOC BRIDGE 1 | #IOC TIME <epoch> | #IOC LINK 0|1
                    #IOC OFFER <hex> | #IOC DOWN <hex> | #IOC KEY <n> | #IOC BUTTON A|B|C|P|PP|H

Uso, com o Studio aberto (python3 Simulacao/studio/server.py ...):
  Front/.venv/bin/python Simulacao/usb_panel_bridge.py --port /dev/ttyACM0 --node 2 --rssi -52
Depois associe o painel pela TUI (F7) ou pelo Studio, como qualquer sensor.
"""
from __future__ import annotations

import argparse
import re
import sys
import time
from collections.abc import Callable

from sim_ctl import discover_socket
from studio.protocol import parse_status, request

UUID_RE = re.compile(r"uuid=(0x[0-9a-fA-F]{16})")
HEX_RE = re.compile(r"^[0-9a-fA-F]+$")
Requester = Callable[[str], str]


class PanelBridge:
    """Ponte entre a linha serial do painel e o socket de controle da bancada."""

    def __init__(self, ser, ask: Requester, node: int, rssi: int, extra: list[tuple[int, int]] | None = None,
                 log: Callable[[str], None] = print) -> None:
        self.ser = ser
        self.ask = ask
        self.node = node
        self.rssi = rssi
        self.extra = extra or []
        self.log = log
        self.uuid: str | None = None
        self.pending = bytearray()
        self.stats = {"up": 0, "down": 0, "offers": 0, "errors": 0}
        self.powered = False

    # ---------------------------------------------------------------- serial
    def write(self, line: str) -> None:
        self.ser.write((line + "\n").encode("ascii"))

    def read_lines(self) -> list[str]:
        waiting = getattr(self.ser, "in_waiting", 0) or 0
        chunk = self.ser.read(waiting or 1)
        if chunk:
            self.pending.extend(chunk)
        if len(self.pending) > 65536:
            self.pending.clear()
        lines = []
        while b"\n" in self.pending:
            raw, _, rest = bytes(self.pending).partition(b"\n")
            self.pending = bytearray(rest)
            text = raw.decode("utf-8", "replace").replace("\x00", "").strip()
            if text:
                lines.append(text)
        return lines

    # -------------------------------------------------------------- bancada
    def attach(self, timeout: float = 15.0) -> str:
        """Põe o painel no modo USB e cria (ou reaproveita) o painel na bancada."""
        deadline = time.monotonic() + timeout
        next_bridge = 0.0
        while self.uuid is None:
            if time.monotonic() > deadline:
                raise RuntimeError("o painel não respondeu: confira a porta e se o firmware é o do PaperColor")
            if time.monotonic() >= next_bridge:
                self.write("#IOC BRIDGE 1")
                next_bridge = time.monotonic() + 1.0
            for line in self.read_lines():
                match = UUID_RE.search(line)
                if line.startswith("#IOC READY") and match:
                    self.uuid = "0x" + match.group(1)[2:].upper()
        uuid = self.uuid
        snapshot = parse_status(self.ask("STATUS"))
        known = next((s for s in snapshot["sensors"] if s["uuid"].upper() == uuid.upper()), None)
        if known is None:
            self.ask(f"SENSOR ADD {uuid} PANEL")
        elif known["profile"] != "PANEL":
            raise RuntimeError(f"{uuid} já existe na bancada com o perfil {known['profile']}")
        elif not known["powered"]:
            self.ask(f"SENSOR POWER {uuid} ON")
        self.ask(f"SENSOR SOURCE {uuid} USB")
        for node, rssi in [(self.node, self.rssi), *self.extra]:
            self.ask(f"SENSOR RSSI {uuid} {node} {rssi}")
        self.write(f"#IOC TIME {int(time.time())}")
        self.powered = True
        self.log(f"[painel] {uuid} na bancada, alcance do Node {self.node} ({self.rssi} dBm)")
        return uuid

    def handle_panel_line(self, line: str) -> None:
        assert self.uuid
        if line.startswith("#IOC UP "):
            payload = line[8:].strip()
            if not HEX_RE.match(payload) or len(payload) % 2 or len(payload) > 4096:
                self.stats["errors"] += 1
                return
            try:
                self.ask(f"PANEL UP {self.uuid} {payload}")
                self.stats["up"] += 1
            except RuntimeError:
                self.stats["errors"] += 1  # sem enlace: o datagrama se perde, como no rádio
        elif line.startswith("#IOC JOIN "):
            node = int(line.split()[2])
            self.ask(f"PANEL JOIN {self.uuid} {node}")
            self.log(f"[painel] entrando no ponto de acesso IOC-{node:02d} (modelo)")
        elif line.startswith("#IOC LEAVE"):
            self.ask(f"PANEL LEAVE {self.uuid}")
        elif line.startswith("#IOC HELLO") and "mode=WIFI" in line:
            # O painel reiniciou: volta ao modo USB.
            self.write("#IOC BRIDGE 1")
        elif line.startswith("#IOC OFF"):
            # Desligado pelo operador: a bancada também o vê desligado (o Node registra STATE=OFF).
            self.ask(f"SENSOR POWER {self.uuid} OFF")
            self.powered = False
            self.log("[painel] desligado pelo botão de energia")
        elif line.startswith("#IOC READY") and not self.powered:
            # Religado pelo operador e de volta ao modo USB.
            self.ask(f"SENSOR POWER {self.uuid} ON")
            self.write(f"#IOC TIME {int(time.time())}")
            self.powered = True
            self.log("[painel] religado")
        elif line.startswith("#IOC LOG "):
            self.log("[painel] " + line[9:])

    def poll(self) -> int:
        assert self.uuid
        reply = request_line(self.ask(f"PANEL POLL {self.uuid}"), "#CTRL OK PANEL POLL ")
        data = reply.partition(" data=")[2]
        count = 0
        for item in filter(None, data.split(";")):
            kind = item.split(" ", 1)[0]
            if kind == "DOWN":
                self.stats["down"] += 1
            elif kind == "OFFER":
                self.stats["offers"] += 1
            self.write("#IOC " + item)
            count += 1
        return count

    def step(self) -> None:
        for line in self.read_lines():
            self.handle_panel_line(line)
        self.poll()

    def detach(self) -> None:
        if self.uuid:
            try:
                self.ask(f"SENSOR POWER {self.uuid} OFF")
            except RuntimeError:
                pass


def request_line(output: str, prefix: str) -> str:
    for line in output.splitlines():
        if line.startswith(prefix):
            return line
    raise RuntimeError("resposta inesperada da bancada: " + output.strip()[:120])


def parse_extra(values: list[str]) -> list[tuple[int, int]]:
    result = []
    for value in values:
        node, _, rssi = value.partition(":")
        result.append((int(node), int(rssi)))
    return result


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Painel PaperColor físico (USB) na bancada virtual")
    ap.add_argument("--port", required=True, help="Porta USB do PaperColor (ex.: /dev/ttyACM0)")
    ap.add_argument("--socket", help="Socket CONTROL; omitir se houver uma única simulação ativa")
    ap.add_argument("--node", type=int, default=1, help="Node virtual que ouve o painel (padrão: 1)")
    ap.add_argument("--rssi", type=int, default=-55, help="RSSI modelado (padrão: -55 dBm)")
    ap.add_argument("--also", action="append", default=[], metavar="NODE:RSSI",
                    help="Outro Node ao alcance, para observar a reassociação (repetível)")
    ap.add_argument("--poll-ms", type=int, default=40, help="Intervalo de consulta à bancada (padrão: 40 ms)")
    args = ap.parse_args(argv)
    try:
        import serial  # pyserial, já usado pela TUI
    except ImportError:
        print("pyserial ausente: use o Python da TUI (Front/.venv/bin/python)", file=sys.stderr)
        return 2
    socket_path = args.socket or discover_socket()

    def ask(command: str) -> str:
        return request(socket_path, command, timeout=10)

    with serial.Serial(args.port, 115200, timeout=0.02) as ser:
        ser.dtr = True
        bridge = PanelBridge(ser, ask, args.node, args.rssi, parse_extra(args.also))
        bridge.attach()
        print("[painel] ponte ativa; Ctrl+C encerra e desliga o painel na bancada", flush=True)
        last_report = time.monotonic()
        try:
            while True:
                bridge.step()
                time.sleep(args.poll_ms / 1000)
                if time.monotonic() - last_report > 30:
                    last_report = time.monotonic()
                    s = bridge.stats
                    print(f"[painel] datagramas: {s['up']} subindo, {s['down']} descendo; ofertas {s['offers']}; "
                          f"descartes {s['errors']}", flush=True)
        except KeyboardInterrupt:
            pass
        finally:
            bridge.detach()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
