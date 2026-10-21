"""Testes das ferramentas Python e do instalador, sem equipamento fisico."""
from __future__ import annotations

import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

PACKAGE = Path(__file__).resolve().parents[2]


@unittest.skipUnless((PACKAGE / "install_stage1.sh").is_file(), "Instalador disponivel apenas no pacote de distribuicao")
class TestInstaller(unittest.TestCase):
    def test_install_idempotent_and_scenarios_remain(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            host = root / "Codigo/node-can/test/host"
            host.mkdir(parents=True)
            (root / "Codigo/scripts").mkdir(parents=True)
            (root / "Codigo/scripts/build_network_sim.sh").write_text("#!/bin/sh\n")
            source = host / "sim_network.cpp"
            source.write_text("""#include <vector>
bool g_verbose = false;
int g_failures = 0;
void collectSerial() {
    n.log.emplace_back(nowMs(), std::move(line));
}
}  // namespace

uint64_t sim_now_us() { return 0; }
int main(int argc, char** argv) {
  const char* name = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--verbose") == 0) g_verbose = true;
    else if (strcmp(argv[i], "--list") == 0) {
      return 0;
    } else name = argv[i];
  }
  if (!name) {
    fprintf(stderr, "Uso: %s <cenario> [--verbose] | --list\\n", argv[0]);
    return 2;
  }
  if (!ioc_link_hex_to_key(IOT_LINK_MASTER_KEY_HEX, g_master)) return 2;
  ioc_link_derive_network_key(g_master, g_network_key);

  for (const Scenario& s : SCENARIOS) {
    s.run();
  }
}
""")
            for _ in range(2):
                result = subprocess.run(["bash", str(PACKAGE / "install_stage1.sh"), str(root)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
            patched = source.read_text()
            self.assertEqual(patched.count('#include "sim_interactive.inc"'), 1)
            self.assertEqual(patched.count("return simInteractiveLoop()"), 1)
            self.assertIn("for (const Scenario& s : SCENARIOS)", patched)
            self.assertTrue((root / "Simulacao/sim_bridge.py").exists())


class TestBridge(unittest.TestCase):
    def test_control_and_probe_pty_roundtrip(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            binary = root / "mock_sim.py"
            binary.write_text('''#!/usr/bin/env python3
import sys
print("#CTRL OK READY mode=INTERACTIVE protocol=1", flush=True)
for line in sys.stdin:
    command = line.strip()
    if command == "STATUS":
        print("#CTRL OK STATUS t_ms=100 running=1 leader=3", flush=True)
        print("#CTRL NODE id=1 powered=1", flush=True)
        print("#CTRL END", flush=True)
    elif command.startswith("PROBE "):
        print("@101 [GW] PROBE_VERSION PROTOCOL=6", flush=True)
        print("#CTRL OK PROBE", flush=True)
    elif command == "QUIT":
        print("#CTRL OK QUIT", flush=True)
        break
    else:
        print("#CTRL OK " + command, flush=True)
''')
            binary.chmod(0o700)
            p = subprocess.Popen(
                [sys.executable, str(PACKAGE / "Simulacao/sim_bridge.py"), "--binary", str(binary)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
            )
            try:
                port_line = p.stdout.readline().strip()
                ctl_line = p.stdout.readline().strip()
                self.assertTrue(port_line.startswith("PORT=/dev/pts/"), port_line)
                self.assertTrue(ctl_line.startswith("CONTROL=/"), ctl_line)
                port = port_line.removeprefix("PORT=")
                control = ctl_line.removeprefix("CONTROL=")
                ctl = subprocess.run([sys.executable, str(PACKAGE / "Simulacao/sim_ctl.py"),
                                      "--socket", control, "STATUS"], capture_output=True, text=True, timeout=5)
                self.assertEqual(ctl.returncode, 0, ctl.stderr)
                self.assertIn("#CTRL NODE id=1 powered=1", ctl.stdout)
                fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                try:
                    os.write(fd, b"PROBE_STATUS\r\n")
                    received = b""
                    deadline = time.monotonic() + 3
                    while time.monotonic() < deadline and b"PROBE_VERSION" not in received:
                        try:
                            received += os.read(fd, 8192)
                        except BlockingIOError:
                            time.sleep(0.02)
                    self.assertIn(b"[GW] PROBE_VERSION", received)
                    self.assertNotIn(b"@101", received)
                finally:
                    os.close(fd)
            finally:
                if p.poll() is None:
                    p.terminate()
                    try:
                        p.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        p.kill()
                        p.wait(timeout=3)
                if p.stdout:
                    p.stdout.close()
                if p.stderr:
                    p.stderr.close()


if __name__ == "__main__":
    unittest.main()
