"""PaperColor pela USB na bancada virtual, de ponta a ponta.

Um PaperColor de mentira, escrito aqui em Python a partir da especificação
(oferta, HELLO/CHALLENGE/CONFIRM, envelope com HMAC-SHA256, mensagens do
painel), fala com a ponte usb_panel_bridge.py exatamente como o firmware fala
pela USB; do outro lado está o executável real da bancada, com o firmware dos
Nodes. Se a ponte, o relé da bancada ou o tap do Node quebrarem, a sessão, o
retrato ou a marca não chegam.
"""
from __future__ import annotations

import hashlib
import hmac
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Simulacao"))
from sim_bridge import SimBridge
from studio.protocol import parse_panel_page, parse_status, request
from usb_panel_bridge import PanelBridge

MASTER = bytes(range(32))  # chave pública da bancada (build_network_sim.sh)
UUID = 0x504300000000BEEF


def mac(key: bytes, message: bytes) -> bytes:
    return hmac.new(key, message, hashlib.sha256).digest()


def seal(key: bytes, kind: int, counter: int, payload: bytes) -> bytes:
    header = b"IL" + bytes([6, kind]) + struct.pack("<I", counter)
    return header + payload + mac(key, header + payload)[:8]


class FakePaperColor:
    """O lado do painel no protocolo de linha da USB."""

    def __init__(self) -> None:
        self.uuid = UUID.to_bytes(8, "big")
        self.dev = mac(MASTER, b"ioc-dev-v1" + self.uuid)
        self.out = bytearray()
        self.buffer = bytearray()
        self.node = 0
        self.link = False
        self.session: bytes | None = None
        self.nonce = b""
        self.tx = 0
        self.child = 0
        self.frames = 0
        self.snapshot_end = False
        self.acked: set[int] = set()
        self.pings = 0
        self.next_hello = 0.0
        self.time_set = False

    # Interface de pyserial usada pela ponte.
    @property
    def in_waiting(self) -> int:
        return len(self.out)

    def read(self, n: int) -> bytes:
        chunk = bytes(self.out[:n])
        del self.out[:n]
        return chunk

    def write(self, data: bytes) -> int:
        self.buffer.extend(data)
        while b"\n" in self.buffer:
            raw, _, rest = bytes(self.buffer).partition(b"\n")
            self.buffer = bytearray(rest)
            self.handle(raw.decode())
        return len(data)

    def emit(self, line: str) -> None:
        self.out.extend((line + "\n").encode())

    def up(self, datagram: bytes) -> None:
        self.emit("#IOC UP " + datagram.hex())

    def data_up(self, inner: bytes) -> None:
        assert self.session
        self.tx += 1
        self.up(seal(self.session, 0x10, self.tx, inner))

    def mark(self, seq: int, text: str) -> None:
        body = struct.pack("<HBBIB", seq, 2, 0, 0, len(text)) + text.encode()
        self.data_up(b"\x55\xaa\x33" + body)

    def handle(self, line: str) -> None:
        cmd, _, arg = line.removeprefix("#IOC ").partition(" ")
        if cmd == "BRIDGE":
            self.emit(f"#IOC READY uuid=0x{UUID:016X} profile=PANEL fw=teste")
        elif cmd == "TIME":
            self.time_set = int(arg) > 1_700_000_000
        elif cmd == "LINK":
            self.link = arg == "1"
        elif cmd == "OFFER":
            mfg = bytes.fromhex(arg)
            if (len(mfg) == 27 and mfg[2:4] == b"IO" and mfg[6:14] == self.uuid and not self.node
                    and mac(self.dev, b"ioc-offer-v1" + mfg[2:19])[:8] == mfg[19:27]):
                self.node = mfg[5]
                self.emit(f"#IOC JOIN {self.node}")
        elif cmd == "DOWN":
            self.datagram(bytes.fromhex(arg))

    def datagram(self, d: bytes) -> None:
        kind, payload, tag = d[3], d[8:-8], d[-8:]
        if kind == 0x02:  # CHALLENGE
            assert mac(self.dev, d[:-8])[:8] == tag
            node, child, node_nonce, sensor_nonce = payload[0], payload[1], payload[2:10], payload[10:18]
            assert sensor_nonce == self.nonce
            self.session = mac(self.dev, b"ioc-sess-v1" + self.uuid + bytes([node]) + self.nonce + node_nonce)
            self.child = child
            self.tx = 1
            self.up(seal(self.session, 0x03, 1, node_nonce + self.nonce))
            self.data_up(b"\x55\xaa\x31\x01\x01\x00\x00")  # HELLO do painel: retrato, sem sono
            return
        if kind != 0x11 or not self.session:
            return
        assert mac(self.session, d[:-8])[:8] == tag, "DATA_DOWN com tag inválida"
        if payload[:2] != b"\x55\xaa":
            return
        if payload[2] == 0x14:  # PING
            self.pings += 1
            self.data_up(b"\x55\xaa\x15" + bytes(8) + payload[11:15])
        elif payload[2] == 0x30:  # FRAMES
            self.frames += payload[12]
            self.snapshot_end |= bool(payload[3] & 0x08)
        elif payload[2] == 0x34:  # MARK_ACK
            self.acked.add(struct.unpack("<H", payload[3:5])[0])

    def tick(self) -> None:
        if self.link and not self.session and time.monotonic() >= self.next_hello:
            self.next_hello = time.monotonic() + 1.0
            self.nonce = os.urandom(8)
            hello = self.uuid + bytes([0x04, 6]) + self.nonce
            self.up(seal(self.dev, 0x01, 0, hello))


@unittest.skipUnless(shutil.which("c++") and shutil.which("cc"), "compiladores C/C++ indisponíveis")
class UsbPanelBridgeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temp.name) / "sim_network"
        subprocess.run([str(ROOT / "Codigo/scripts/build_network_sim.sh"), str(cls.binary)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.bridge = SimBridge(cls.binary)
        cls.thread = threading.Thread(target=cls.bridge.serve, daemon=True)
        cls.thread.start()
        if not cls.bridge.ready.wait(20):
            raise RuntimeError("bancada não iniciou")
        cls.socket = str(cls.bridge.socket_path)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.bridge.stop.set()
        cls.thread.join(timeout=10)
        cls.temp.cleanup()

    def ask(self, command: str) -> str:
        return request(self.socket, command, timeout=10)

    def run_until(self, bridge: PanelBridge, fake: FakePaperColor, condition, seconds: float) -> bool:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            fake.tick()
            bridge.step()
            if condition():
                return True
            time.sleep(0.02)
        return False

    def test_physical_panel_reaches_the_virtual_node_over_usb(self) -> None:
        fake = FakePaperColor()
        bridge = PanelBridge(fake, self.ask, node=2, rssi=-50, extra=[(3, -70)], log=lambda _line: None)
        uuid = bridge.attach()
        self.assertEqual(uuid, f"0x{UUID:016X}")
        self.assertTrue(fake.time_set)
        sensors = parse_status(self.ask("STATUS"))["sensors"]
        self.assertIn(("PANEL", uuid), [(s["profile"], s["uuid"]) for s in sensors])

        # O Node 2 só aceita associar um candidato ouvido há pouco.
        self.run_until(bridge, fake, lambda: False, 2.5)
        self.ask(f"PROBE WIRELESS BIND 2 {uuid} AUTO")
        self.assertTrue(self.run_until(bridge, fake, lambda: fake.session is not None and fake.snapshot_end, 25),
                        "sem sessão autenticada ou sem retrato pela USB")
        self.assertEqual(fake.node, 2)
        self.assertGreater(fake.frames, 10)
        # Ao vivo: o tap continua entregando quadros, e o Node mantém a sessão (PING/PONG).
        before = fake.frames
        self.assertTrue(self.run_until(bridge, fake, lambda: fake.frames > before + 20 and fake.pings >= 1, 10))

        fake.mark(1, "teste pela USB")
        self.assertTrue(self.run_until(bridge, fake, lambda: 1 in fake.acked, 5), "marca sem confirmação do Node")

        page = parse_panel_page(self.ask(f"PANEL PAGE {uuid}"))
        self.assertEqual(page["source"], "MIRROR")
        self.assertTrue(page["session"])
        self.assertEqual((page["node"], page["child"]), (2, fake.child))
        texts = " ".join(item["text"] for item in page["items"])
        self.assertIn("Rede normal", texts)
        self.assertGreater(len(page["items"]), 10)

        # Entradas do Studio seguem para o painel físico.
        self.ask(f"PANEL BUTTON {uuid} A")
        self.ask(f"PANEL NOTE {uuid} ok")
        replies: list[str] = []
        original = fake.handle
        fake.handle = lambda line: (replies.append(line), original(line))  # type: ignore[method-assign]
        self.run_until(bridge, fake, lambda: any(r.startswith("#IOC KEY 13") for r in replies), 3)
        self.assertIn("#IOC BUTTON A", replies)
        self.assertIn("#IOC KEY 111", replies)  # 'o'
        # Segurar o botão de energia pelo Studio chega ao painel físico.
        self.ask(f"PANEL BUTTON {uuid} H")
        self.assertTrue(self.run_until(bridge, fake, lambda: "#IOC BUTTON H" in replies, 3))

        # Desligado pelo botão de energia: a bancada o vê desligado; religado, volta a ligar.
        fake.emit("#IOC OFF")
        bridge.step()
        self.assertFalse(self._powered(uuid))
        fake.emit(f"#IOC READY uuid=0x{UUID:016X} profile=PANEL fw=teste")
        bridge.step()
        self.assertTrue(self._powered(uuid))
        bridge.detach()
        self.assertFalse(self._powered(uuid))

    def _powered(self, uuid: str) -> bool:
        return {s["uuid"]: s["powered"] for s in parse_status(self.ask("STATUS"))["sensors"]}[uuid]


if __name__ == "__main__":
    unittest.main()


class PanelStudioProtocolTest(unittest.TestCase):
    """Comandos do Studio para o painel: só operações enumeradas e validadas."""

    def test_panel_actions_build_safe_commands(self) -> None:
        from studio.protocol import build_command, validate_native_command

        uuid = f"0x{UUID:016X}"
        self.assertEqual(build_command({"type": "sensor.add", "uuid": uuid, "profile": "panel"}), f"SENSOR ADD {uuid} PANEL")
        self.assertEqual(build_command({"type": "panel.button", "uuid": uuid, "button": "pp"}), f"PANEL BUTTON {uuid} PP")
        self.assertEqual(build_command({"type": "panel.note", "uuid": uuid, "text": "troca do cabo"}),
                         f"PANEL NOTE {uuid} troca do cabo")
        self.assertEqual(validate_native_command(f"PANEL BATTERY {uuid} ON"), f"PANEL BATTERY {uuid} ON")
        for bad in ("a;b", "", "x" * 97, "ação"):
            with self.assertRaises(ValueError):
                build_command({"type": "panel.note", "uuid": uuid, "text": bad})
        with self.assertRaises(ValueError):
            build_command({"type": "panel.button", "uuid": uuid, "button": "Z"})

    def test_page_parser_decodes_display_list(self) -> None:
        line = ("#CTRL OK PANEL PAGE uuid=0x504300000000BEEF source=VIRTUAL powered=1 hash=0A0B0C0D level=2 renders=3 "
                "beeps=1 session=1 node=2 child=1 battery=0 dl=1,2,0,0,0,0,400,48,|3,0,24,0,46,12,0,24,4174656ec3a7c3a36f")
        page = parse_panel_page(line + "\n")
        self.assertEqual((page["level"], page["node"], page["renders"]), (2, 2, 3))
        self.assertEqual(page["items"][1]["text"], "Atenção")
