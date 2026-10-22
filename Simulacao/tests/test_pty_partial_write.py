"""Regressões de escrita não bloqueante da PTY usada pelo Studio."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sim_bridge import SimBridge


class PtyOutputTests(unittest.TestCase):
    def test_partial_writes_preserve_entire_probe_line(self):
        bridge = SimBridge(Path("/tmp/not-needed"))
        bridge.master = 123
        sent = bytearray()

        def write(fd, data):
            self.assertEqual(fd, 123)
            count = min(5, len(data))
            sent.extend(data[:count])
            return count

        line = b"[GW] FRAG TYPE=FFT INDEX=0 COUNT=4 DATA=ABCDEF\r\n"
        with patch("sim_bridge.os.write", side_effect=write):
            self.assertTrue(bridge._write_pty_line(line))
        self.assertEqual(bytes(sent), line)

    def test_eagain_between_partial_writes_is_retried(self):
        bridge = SimBridge(Path("/tmp/not-needed"))
        bridge.master = 123
        sent = bytearray()
        calls = 0

        def write(_fd, data):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise BlockingIOError()
            count = min(3, len(data))
            sent.extend(data[:count])
            return count

        line = b"[GW] PROBE_STATUS\r\n"
        with patch("sim_bridge.os.write", side_effect=write), \
             patch("sim_bridge.select.select", return_value=([], [123], [])):
            self.assertTrue(bridge._write_pty_line(line))
        self.assertEqual(bytes(sent), line)

    def test_full_buffer_does_not_block_indefinitely(self):
        bridge = SimBridge(Path("/tmp/not-needed"))
        bridge.master = 123
        with patch("sim_bridge.os.write", side_effect=BlockingIOError()), \
             patch("sim_bridge.select.select", return_value=([], [], [])):
            self.assertFalse(bridge._write_pty_line(b"NO_DATA\r\n", timeout=0.01))


if __name__ == "__main__":
    unittest.main()
