"""Regression: an invalid acquisition interval must not consume the FFT request."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import usb_sensor_bridge as bridge


class UsbFftRetryTests(unittest.TestCase):
    def run_case(self, lines):
        class FakeSerial:
            def __init__(self):
                self.writes = []
            def write(self, message):
                self.writes.append(message)
            def flush(self):
                pass

        device = FakeSerial()
        def replay(*_):
            if lines:
                return lines.pop(0)
            raise KeyboardInterrupt()

        status={'AUTH': 'LOCKED', 'TELEMETRY': 'ON', 'PERIOD_MS': '1000'}
        with (patch.object(bridge, 'fft_request_count', return_value=1),
              patch.object(bridge, 'get_complete_serial_line', side_effect=replay),
              patch.object(bridge, 'deliver_fft') as delivery,
              patch.object(bridge, 'request', return_value='#CTRL OK SENSOR SAMPLE')):
            with self.assertRaises(KeyboardInterrupt):
                bridge.stream(device, '/tmp/not-real.sock', '0x0000000000000123',
                              'ACOUSTIC', status, None, passive=True,
                              period_ms=1000, fft_enabled=True)
        return device.writes, delivery

    def test_fft_no_then_valid_succeeds_without_fake_values(self):
        writes,delivery=self.run_case([
            'FFT VALID=NO BINS=0',
            'FFT VALID=YES BINS=2 RATE_HZ=16000 FFT_SIZE=4 UNIT=PA VALUES=0.2,0.1',
        ])
        self.assertEqual(writes.count(b'FFT ONCE\n'),2)
        delivery.assert_called_once()
        args=delivery.call_args.args
        self.assertEqual(args[-1],(16000.0,4,[0.2,0.1]))

    def test_three_missing_spectra_do_not_fabricate_fft(self):
        writes,delivery=self.run_case(['FFT VALID=NO BINS=0']*3)
        self.assertEqual(writes.count(b'FFT ONCE\n'),3)
        delivery.assert_not_called()

if __name__ == '__main__':
    unittest.main()
