"""Teste de ciclo de vida da simulacao incorporada a TUI."""
from __future__ import annotations

import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"Simulacao"))
from sim_tui_runtime import local_simulation, SimStartupError
from sim_ctl import discover_socket


class LocalSessionTest(unittest.TestCase):
    def test_launch_and_cleanup_mock_firmware(self):
        with tempfile.TemporaryDirectory() as t:
            binary=Path(t)/"sim.py"
            binary.write_text('''#!/usr/bin/env python3
import sys
print("#CTRL OK READY mode=INTERACTIVE protocol=1",flush=True)
for raw in sys.stdin:
    cmd=raw.strip()
    if cmd=="STATUS":
        print("#CTRL OK STATUS t_ms=1 running=1 leader=3",flush=True)
        print("#CTRL END",flush=True)
    elif cmd.startswith("PROBE "):
        print("@1 [GW] PROBE_STATUS",flush=True)
        print("#CTRL OK PROBE",flush=True)
    elif cmd=="QUIT":
        print("#CTRL OK QUIT",flush=True)
        break
''')
            binary.chmod(0o700)
            # Cada teste isola seus sockets. O Studio pode permanecer aberto
            # durante a suite, sem tornar discover_socket() ambiguo.
            with patch("tempfile.gettempdir", return_value=t):
                with contextlib.redirect_stdout(io.StringIO()):
                    with local_simulation(binary) as port:
                        self.assertTrue(port.startswith("/dev/pts/"))
                        self.assertTrue(os.path.exists(port))
                        socket_path = Path(discover_socket())
                        self.assertTrue(socket_path.exists())
                        self.assertTrue(socket_path.is_relative_to(Path(t)))
                with self.assertRaises(SystemExit):
                    discover_socket()

    def test_missing_binary_is_rejected(self):
        with self.assertRaises(SimStartupError):
            with local_simulation(Path('/nonexistent/ioc-sim-binary')):
                pass
