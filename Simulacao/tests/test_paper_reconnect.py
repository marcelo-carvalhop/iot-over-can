"""Regressões da reconexão do M5Stack PaperColor por USB e HMAC no firmware."""
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Simulacao"))
from usb_panel_bridge import PanelBridge

UUID = "0x504300000000BEEF"


class BootSerial:
    """Reproduz o firmware enviando JOIN antes de READY num único lote USB."""

    def __init__(self):
        self.out = bytearray()
        self.pending = bytearray()
        self.written = []

    @property
    def in_waiting(self):
        return len(self.out)

    def read(self, size):
        data = bytes(self.out[:size])
        del self.out[:size]
        return data

    def write(self, data):
        self.pending.extend(data)
        while b"\n" in self.pending:
            raw, _, rest = bytes(self.pending).partition(b"\n")
            self.pending = bytearray(rest)
            line = raw.decode()
            self.written.append(line)
            if line == "#IOC BRIDGE 1":
                self.out.extend((f"#IOC JOIN 2\n#IOC READY uuid={UUID} profile=PANEL fw=0.20\n").encode())
        return len(data)


class PanelBootstrapTests(unittest.TestCase):
    def test_join_before_ready_is_forwarded_after_sensor_setup(self):
        serial = BootSerial()
        commands = []

        def ask(command):
            commands.append(command)
            if command == "STATUS":
                return "#CTRL OK STATUS t_ms=0 running=1 leader=1 frames=0 bits=0\n#CTRL END\n"
            return "#CTRL OK\n"

        bridge = PanelBridge(serial, ask, node=2, rssi=-52, log=lambda _: None)
        self.assertEqual(bridge.attach(timeout=1), UUID)
        add_idx = commands.index(f"SENSOR ADD {UUID} PANEL")
        source_idx = commands.index(f"SENSOR SOURCE {UUID} USB")
        join_idx = commands.index(f"PANEL JOIN {UUID} 2")
        self.assertLess(add_idx, source_idx)
        self.assertLess(source_idx, join_idx)
        self.assertIn("#IOC TIME ", " ".join(serial.written))


@unittest.skipUnless(shutil.which("cc") and shutil.which("c++"), "Compiladores C e C++ indisponíveis")
class PaperColorNativeTests(unittest.TestCase):
    def test_retries_and_reauthentication_from_actual_panel_app(self):
        link = ROOT / "Codigo/common/ioc_link"
        panel = ROOT / "Codigo/common/ioc_panel"
        app = ROOT / "Codigo/node-paper/src"
        fixture = ROOT / "Codigo/node-paper/tests/test_panel_reconnect.cpp"
        files = [
            link / "ioc_sha256.c", link / "ioc_link.c",
            panel / "ioc_panel_proto.c", panel / "ioc_panel_cache.c",
            panel / "ioc_view.c", panel / "ioc_page.c",
        ]
        with tempfile.TemporaryDirectory(prefix="ioc_panel_auth_") as work:
            workdir = Path(work)
            includes = [f"-I{p}" for p in (link, panel, app)]
            objects = []
            for source in files:
                obj = workdir / (source.stem + ".o")
                subprocess.run(["cc", "-std=c11", "-O0", *includes,
                                "-c", str(source), "-o", str(obj)],
                               check=True, capture_output=True, text=True)
                objects.append(str(obj))
            exe = workdir / "panel-reconnect"
            build = subprocess.run(["c++", "-std=c++17", "-O0", *includes,
                                    str(fixture), str(app / "panel_app.cpp"),
                                    *objects, "-lm", "-o", str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + "\n" + build.stderr)
            result = subprocess.run([str(exe)], timeout=12, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + "\n" + result.stderr)


if __name__ == "__main__":
    unittest.main()
