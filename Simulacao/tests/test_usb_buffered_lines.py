"""USB CDC: mensagens completas no buffer não podem aguardar outro readline."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from usb_sensor_bridge import get_complete_serial_line


class ReadBufferTests(unittest.TestCase):
    def test_drains_pending_lines_before_reading_serial(self):
        class NoRead:
            def readline(self):
                raise AssertionError("Leitura bloqueante desnecessária")
        buf = bytearray(b"TEL LAEQ_DB=60.0\r\nFFT VALID=YES BINS=2 RATE_HZ=16000 FFT_SIZE=4 VALUES=0.1,0.2\r\n")
        self.assertEqual(get_complete_serial_line(NoRead(), buf), "TEL LAEQ_DB=60.0")
        self.assertTrue(get_complete_serial_line(NoRead(), buf).startswith("FFT VALID=YES"))
        self.assertEqual(buf, bytearray())

    def test_partial_line_is_retained_until_terminator_arrives(self):
        class Fragmented:
            def __init__(self):
                self.parts = [b"FFT VALID=YES ", b"BINS=2\n"]
                self.reads = 0
            def readline(self):
                self.reads += 1
                return self.parts.pop(0)
        serial = Fragmented()
        buf = bytearray()
        self.assertEqual(get_complete_serial_line(serial, buf), "")
        self.assertEqual(get_complete_serial_line(serial, buf), "FFT VALID=YES BINS=2")
        self.assertEqual(serial.reads, 2)

    def test_oversized_incomplete_line_is_rejected(self):
        class NoRead:
            def readline(self):
                return b""
        buf = bytearray(b"x" * 16385)
        with self.assertRaisesRegex(ValueError, "16 KiB"):
            get_complete_serial_line(NoRead(), buf)
        self.assertFalse(buf)


if __name__ == "__main__":
    unittest.main()
